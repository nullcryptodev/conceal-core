// BoltDex.h — order matching engine with settlement, deposits, and withdrawals
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltDexTypes.h"
#include "SidechainTypes.h"
#include <vector>
#include <mutex>
#include <string>
#include <functional>
#include <unordered_map>

namespace Sidechain
{
  class SidechainStorage;
  class SidechainValidator;

  namespace BoltDex
  {
    class Engine
    {
    public:
      using TradeCallback = std::function<void(const Trade &)>;

      Engine();

      // Set storage and validator references for direct access
      void setStorage(SidechainStorage &storage);
      void setValidator(SidechainValidator &validator);

      // Set the DEX keypair
      void setDexKeys(const crypto::PublicKey &pubKey, const crypto::SecretKey &secKey);

      // Get the DEX public key (users send deposits here)
      crypto::PublicKey getDexPublicKey() const { return m_dexPubKey; }

      // Process a newly committed block for deposits
      void processBlock(const Block &block);

      // Submit a new order to the book
      bool submitOrder(const Order &order);

      // Cancel an existing order
      bool cancelOrder(uint64_t orderId, const crypto::PublicKey &owner);

      // Withdraw funds from escrow
      bool withdraw(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount);

      // Get all open orders for a token pair
      std::vector<Order> getOrders(uint64_t baseTokenId, uint64_t quoteTokenId) const;

      // Get order by ID
      bool getOrder(uint64_t orderId, Order &order) const;

      // Get trade history for a token pair
      std::vector<Trade> getTrades(uint64_t baseTokenId, uint64_t quoteTokenId, size_t limit = 50) const;

      // Get all trades
      std::vector<Trade> getAllTrades(size_t limit = 100) const;

      // Get escrow balance for an address
      uint64_t getEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId) const;

      // Set callback for when a trade executes (internal use)
      void onTrade(TradeCallback callback);

      // Set callback for SSE/WebSocket trade push (real-time to GUI)
      void onTradeEvent(TradeCallback callback) { m_tradeEventCallback = std::move(callback); }

      // Process all pending settlements
      void processSettlements();

      // Set the trading fee percentage (e.g. 0.5 = 0.5%)
      void setTradingFee(double feePercent) { m_tradingFee = feePercent; }

      uint64_t getNextOrderId() const { return m_nextOrderId; }

    private:
      void matchOrders(uint64_t baseTokenId, uint64_t quoteTokenId);
      bool executeTrade(const Order &buy, const Order &sell, uint64_t amount);

      SidechainStorage *m_storage = nullptr;
      SidechainValidator *m_validator = nullptr;
      std::vector<Order> m_orders;
      std::vector<Trade> m_trades;
      std::vector<Settlement> m_pendingSettlements;
      uint64_t m_nextOrderId = 1;
      uint64_t m_nextTradeId = 1;
      mutable std::mutex m_mutex;
      TradeCallback m_tradeCallback;
      TradeCallback m_tradeEventCallback;

      double m_tradingFee = 0.0;

      // DEX identity
      crypto::PublicKey m_dexPubKey;
      crypto::SecretKey m_dexSecKey;

      // Escrow balances: key = "owner_tokenId"
      std::unordered_map<std::string, uint64_t> m_escrow;
      std::string escrowKey(const crypto::PublicKey &owner, uint64_t tokenId) const;
      void addEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount);
      void subEscrowBalance(const crypto::PublicKey &owner, uint64_t tokenId, uint64_t amount);
    };
  }
}