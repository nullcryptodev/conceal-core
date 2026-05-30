// BoltDex.cpp — order matching engine with settlement
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltDex.h"
#include "SidechainStorage.h"
#include "SidechainValidator.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include <algorithm>
#include <iostream>
#include <chrono>

namespace Sidechain
{
  namespace BoltDex
  {
    Engine::Engine()
    {
    }

    void Engine::setStorage(SidechainStorage &storage)
    {
      m_storage = &storage;
    }

    void Engine::setValidator(SidechainValidator &validator)
    {
      m_validator = &validator;
    }

    std::string Engine::escrowKey(const crypto::PublicKey &owner, uint64_t tokenId) const
    {
      return common::podToHex(owner) + "_" + std::to_string(tokenId);
    }

    uint64_t Engine::getEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId) const
    {
      auto it = m_escrow.find(escrowKey(owner, tokenId));
      return it != m_escrow.end() ? it->second : 0;
    }

    void Engine::addEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount)
    {
      m_escrow[escrowKey(owner, tokenId)] += amount;
    }

    void Engine::subEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount)
    {
      auto it = m_escrow.find(escrowKey(owner, tokenId));
      if (it != m_escrow.end() && it->second >= amount)
        it->second -= amount;
    }

    void Engine::setDexKeys(const crypto::PublicKey &pubKey, const crypto::SecretKey &secKey)
    {
      m_dexPubKey = pubKey;
      m_dexSecKey = secKey;
      std::cout << "DEX public key: " << common::podToHex(pubKey) << std::endl;
    }

    void Engine::processBlock(const Block &block)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      std::string dexAddrHex = common::podToHex(m_dexPubKey);

      for (const auto &tx : block.transactions)
      {
        // Only process transfers TO the DEX address
        if (tx.type != TransactionType::Transfer)
          continue;

        std::string toHex = common::podToHex(tx.to);
        if (toHex != dexAddrHex)
          continue;

        // Credit the sender's escrow balance
        addEscrowBalance(tx.from, tx.tokenId, tx.amount);

        std::cout << "DEX: deposit credited in block " << block.header.height
                  << ": " << tx.amount << " of token " << tx.tokenId
                  << " from " << common::podToHex(tx.from).substr(0, 16) << std::endl;
      }
    }

    bool Engine::submitOrder(const Order &order)
    {
      std::lock_guard<std::mutex> lock(m_mutex);

      if (order.type == OrderType::Sell)
      {
        uint64_t escrowed = getEscrowBalance(order.owner, order.baseTokenId);
        if (escrowed < order.amount)
        {
          std::cerr << "DEX: insufficient escrow balance for sell order. "
                    << "Have " << escrowed << ", need " << order.amount << std::endl;
          return false;
        }
        subEscrowBalance(order.owner, order.baseTokenId, order.amount);
      }
      else
      {
        uint64_t totalCost = order.amount * order.price;
        uint64_t escrowed = getEscrowBalance(order.owner, order.quoteTokenId);
        if (escrowed < totalCost)
        {
          std::cerr << "DEX: insufficient escrow balance for buy order. "
                    << "Have " << escrowed << ", need " << totalCost << std::endl;
          return false;
        }
        subEscrowBalance(order.owner, order.quoteTokenId, totalCost);
      }

      Order newOrder = order;
      newOrder.id = m_nextOrderId++;
      newOrder.status = OrderStatus::Open;
      newOrder.filled = 0;
      newOrder.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());

      m_orders.push_back(newOrder);

      std::cout << "DEX: new " << (order.type == OrderType::Buy ? "buy" : "sell")
                << " order #" << newOrder.id
                << " amount=" << order.amount
                << " price=" << order.price << std::endl;

      matchOrders(order.baseTokenId, order.quoteTokenId);

      return true;
    }

    bool Engine::cancelOrder(uint64_t orderId, const crypto::PublicKey &owner)
    {
      std::lock_guard<std::mutex> lock(m_mutex);

      for (auto &order : m_orders)
      {
        if (order.id == orderId && order.owner == owner && order.status == OrderStatus::Open)
        {
          order.status = OrderStatus::Cancelled;

          if (order.type == OrderType::Sell)
          {
            uint64_t remaining = order.amount - order.filled;
            addEscrowBalance(order.owner, order.baseTokenId, remaining);
          }
          else
          {
            uint64_t remaining = (order.amount - order.filled) * order.price;
            addEscrowBalance(order.owner, order.quoteTokenId, remaining);
          }

          std::cout << "DEX: order #" << orderId << " cancelled, funds returned to escrow" << std::endl;
          return true;
        }
      }
      return false;
    }

    std::vector<Order> Engine::getOrders(uint64_t baseTokenId, uint64_t quoteTokenId) const
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      std::vector<Order> result;
      for (const auto &order : m_orders)
      {
        if (order.baseTokenId == baseTokenId &&
            order.quoteTokenId == quoteTokenId &&
            order.status == OrderStatus::Open)
        {
          result.push_back(order);
        }
      }
      return result;
    }

    bool Engine::getOrder(uint64_t orderId, Order &order) const
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      for (const auto &o : m_orders)
      {
        if (o.id == orderId)
        {
          order = o;
          return true;
        }
      }
      return false;
    }

    std::vector<Trade> Engine::getTrades(uint64_t baseTokenId, uint64_t quoteTokenId, size_t limit) const
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      std::vector<Trade> result;
      for (const auto &trade : m_trades)
      {
        if (trade.baseTokenId == baseTokenId && trade.quoteTokenId == quoteTokenId)
        {
          result.push_back(trade);
        }
      }
      std::reverse(result.begin(), result.end());
      if (result.size() > limit)
        result.resize(limit);
      return result;
    }

    std::vector<Trade> Engine::getAllTrades(size_t limit) const
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      std::vector<Trade> result = m_trades;
      std::reverse(result.begin(), result.end());
      if (result.size() > limit)
        result.resize(limit);
      return result;
    }

    void Engine::onTrade(TradeCallback callback)
    {
      m_tradeCallback = callback;
    }

    void Engine::processSettlements()
    {
      if (!m_storage || !m_validator)
        return;

      std::lock_guard<std::mutex> lock(m_mutex);

      for (auto &settlement : m_pendingSettlements)
      {
        // Create a transfer transaction
        Transaction tx;
        tx.type = TransactionType::Transfer;
        tx.from = settlement.from;
        tx.to = settlement.to;
        tx.amount = settlement.amount;
        tx.tokenId = settlement.tokenId;
        tx.fee = 0; // DEX pays fees from its own balance
        tx.feeTokenId = 0;
        tx.signature = crypto::Signature{};
        tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
        tx.extra.clear();

        cn::BinaryArray txBytes = cn::toBinaryArray(tx);
        crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

        // Update balances directly in storage
        uint64_t fromBalance = 0;
        m_storage->getBalance(tx.from, tx.tokenId, fromBalance);
        if (fromBalance >= tx.amount)
        {
          m_storage->setBalance(tx.from, tx.tokenId, fromBalance - tx.amount);
          uint64_t toBalance = 0;
          m_storage->getBalance(tx.to, tx.tokenId, toBalance);
          m_storage->setBalance(tx.to, tx.tokenId, toBalance + tx.amount);

          std::cout << "DEX: settlement #" << settlement.tradeId
                    << " completed: " << settlement.description << std::endl;
        }
      }
      m_pendingSettlements.clear();
    }

    void Engine::matchOrders(uint64_t baseTokenId, uint64_t quoteTokenId)
    {
      std::vector<Order *> buys;
      std::vector<Order *> sells;

      for (auto &order : m_orders)
      {
        if (order.status != OrderStatus::Open)
          continue;
        if (order.baseTokenId != baseTokenId || order.quoteTokenId != quoteTokenId)
          continue;

        if (order.type == OrderType::Buy)
          buys.push_back(&order);
        else
          sells.push_back(&order);
      }

      std::sort(buys.begin(), buys.end(), [](Order *a, Order *b)
                { return a->price > b->price; });
      std::sort(sells.begin(), sells.end(), [](Order *a, Order *b)
                { return a->price < b->price; });

      for (auto *buy : buys)
      {
        for (auto *sell : sells)
        {
          if (buy->price >= sell->price &&
              buy->status == OrderStatus::Open &&
              sell->status == OrderStatus::Open)
          {
            uint64_t buyRemaining = buy->amount - buy->filled;
            uint64_t sellRemaining = sell->amount - sell->filled;
            uint64_t matchAmount = std::min(buyRemaining, sellRemaining);

            if (matchAmount > 0)
            {
              executeTrade(*buy, *sell, matchAmount);
            }
          }
        }
      }
    }

    bool Engine::executeTrade(const Order &buy, const Order &sell, uint64_t amount)
    {
      Trade trade;
      trade.id = m_nextTradeId++;
      trade.buyOrderId = buy.id;
      trade.sellOrderId = sell.id;
      trade.buyer = buy.owner;
      trade.seller = sell.owner;
      trade.baseTokenId = buy.baseTokenId;
      trade.quoteTokenId = buy.quoteTokenId;
      trade.amount = amount;
      trade.price = sell.price;
      trade.timestamp = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
      trade.settled = false;

      m_trades.push_back(trade);

      uint64_t totalQuoteAmount = amount * sell.price;

      // Calculate DEX trading fee (in quote token)
      uint64_t dexFee = 0;
      if (m_tradingFee > 0.0)
      {
        dexFee = static_cast<uint64_t>(totalQuoteAmount * (m_tradingFee / 100.0));
        if (dexFee > 0 && dexFee < totalQuoteAmount)
        {
          Settlement feeSettlement;
          feeSettlement.tradeId = trade.id;
          feeSettlement.from = buy.owner;
          feeSettlement.to = m_dexPubKey;
          feeSettlement.tokenId = buy.quoteTokenId;
          feeSettlement.amount = dexFee;
          feeSettlement.description = "Trade #" + std::to_string(trade.id) + " trading fee";
          m_pendingSettlements.push_back(feeSettlement);
        }
      }

      // Calculate creator royalty
      uint64_t royaltyAmount = 0;
      TokenInfo tokenInfo;
      if (m_storage && m_storage->getToken(trade.baseTokenId, tokenInfo))
      {
        if (tokenInfo.royaltiesEnabled && tokenInfo.royaltyBasisPoints > 0)
        {
          royaltyAmount = (totalQuoteAmount * tokenInfo.royaltyBasisPoints) / 10000;
          if (royaltyAmount > 0 && royaltyAmount < totalQuoteAmount)
          {
            Settlement royaltySettlement;
            royaltySettlement.tradeId = trade.id;
            royaltySettlement.from = buy.owner;
            royaltySettlement.to = tokenInfo.creator;
            royaltySettlement.tokenId = buy.quoteTokenId;
            royaltySettlement.amount = royaltyAmount;
            royaltySettlement.description = "Trade #" + std::to_string(trade.id) + " creator royalty (" +
                                            std::to_string(tokenInfo.royaltyBasisPoints / 100) + "." +
                                            std::to_string(tokenInfo.royaltyBasisPoints % 100) + "%)";
            m_pendingSettlements.push_back(royaltySettlement);
          }
        }
      }

      uint64_t sellerAmount = totalQuoteAmount - dexFee - royaltyAmount;

      Settlement sellerSettlement;
      sellerSettlement.tradeId = trade.id;
      sellerSettlement.from = buy.owner;
      sellerSettlement.to = sell.owner;
      sellerSettlement.tokenId = buy.quoteTokenId;
      sellerSettlement.amount = sellerAmount;
      sellerSettlement.description = "Trade #" + std::to_string(trade.id) + " seller payment";
      m_pendingSettlements.push_back(sellerSettlement);

      Settlement buyerSettlement;
      buyerSettlement.tradeId = trade.id;
      buyerSettlement.from = sell.owner;
      buyerSettlement.to = buy.owner;
      buyerSettlement.tokenId = buy.baseTokenId;
      buyerSettlement.amount = amount;
      buyerSettlement.description = "Trade #" + std::to_string(trade.id) + " buyer delivery";
      m_pendingSettlements.push_back(buyerSettlement);

      for (auto &order : m_orders)
      {
        if (order.id == buy.id)
        {
          order.filled += amount;
          if (order.filled >= order.amount)
            order.status = OrderStatus::Filled;
        }
        if (order.id == sell.id)
        {
          order.filled += amount;
          if (order.filled >= order.amount)
            order.status = OrderStatus::Filled;
        }
      }

      std::cout << "DEX: trade executed #" << trade.id
                << " amount=" << amount
                << " price=" << trade.price
                << " fee=" << dexFee
                << " royalty=" << royaltyAmount << std::endl;

      if (m_tradeCallback)
        m_tradeCallback(trade);

      if (m_tradeEventCallback)
        m_tradeEventCallback(trade);

      return true;
    }

    bool Engine::withdraw(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount)
    {
      std::lock_guard<std::mutex> lock(m_mutex);

      uint64_t balance = getEscrowBalance(owner, tokenId);
      if (balance < amount)
      {
        std::cerr << "DEX: insufficient escrow balance for withdrawal" << std::endl;
        return false;
      }

      subEscrowBalance(owner, tokenId, amount);

      // Credit user's sidechain balance directly via storage
      uint64_t userBalance = 0;
      m_storage->getBalance(owner, tokenId, userBalance);
      m_storage->setBalance(owner, tokenId, userBalance + amount);

      // Debit DEX balance
      uint64_t dexBalance = 0;
      m_storage->getBalance(m_dexPubKey, tokenId, dexBalance);
      if (dexBalance >= amount)
        m_storage->setBalance(m_dexPubKey, tokenId, dexBalance - amount);

      std::cout << "DEX: withdrawal of " << amount
                << " of token " << tokenId
                << " to " << common::podToHex(owner).substr(0, 16) << std::endl;
      return true;
    }
  }
}