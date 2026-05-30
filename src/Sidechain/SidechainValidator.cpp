// SidechainValidator implementation with BFT
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainValidator.h"
#include "BoltAMM.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include <chrono>
#include <iostream>

namespace Sidechain
{
  SidechainValidator::SidechainValidator(SidechainStorage &storage,
                                         const ValidatorInfo &self,
                                         const std::vector<ValidatorInfo> &validators,
                                         GossipManager &gossip,
                                         bool testnet)
      : m_storage(storage), m_self(self), m_validators(validators),
        m_gossip(gossip), m_consensus(storage, self, gossip), m_testnet(testnet)
  {
    // Register for committed blocks
    m_consensus.onBlockCommitted([this](const Block &block)
                                 {
    std::cout << "Block " << block.header.height << " committed with "
              << block.transactions.size() << " transactions" << std::endl;

    // Process vesting releases and reward accrual on each new block
    m_storage.processVestingReleases(block.header.height);
    m_storage.processRewardAccrual(block.header.height); });
  }

  SidechainValidator::~SidechainValidator()
  {
    stop();
  }

  void SidechainValidator::start()
  {
    if (m_running)
      return;
    m_running = true;
    m_consensusThread = std::thread(&SidechainValidator::consensusLoop, this);
  }

  void SidechainValidator::stop()
  {
    m_running = false;
    if (m_consensusThread.joinable())
      m_consensusThread.join();
  }

  bool SidechainValidator::submitTransaction(const Transaction &tx)
  {
    std::lock_guard<std::mutex> lock(m_txMutex);

    if (!validateTransaction(tx))
      return false;

    m_pendingTransactions.push(tx);
    return true;
  }

  std::vector<Transaction> SidechainValidator::getPendingTransactions() const
  {
    std::lock_guard<std::mutex> lock(m_txMutex);
    std::vector<Transaction> result;
    auto queue = m_pendingTransactions;
    while (!queue.empty())
    {
      result.push_back(queue.front());
      queue.pop();
    }
    return result;
  }

  std::vector<ValidatorInfo> SidechainValidator::getValidators() const
  {
    std::lock_guard<std::mutex> lock(m_validatorMutex);
    return m_validators;
  }

  // Private methods
  void SidechainValidator::consensusLoop()
  {
    while (m_running)
    {
      std::vector<Transaction> batch;
      {
        std::lock_guard<std::mutex> lock(m_txMutex);
        size_t count = 0;
        while (!m_pendingTransactions.empty() && count < SidechainConfig::MAX_TRANSACTIONS_PER_BLOCK)
        {
          Transaction tx = m_pendingTransactions.front();
          m_pendingTransactions.pop();
          if (validateTransaction(tx))
          {
            tx.id = batch.size();
            batch.push_back(tx);
            ++count;
          }
        }
      }

      if (!batch.empty())
      {
        std::cout << "Consensus: proposing block with " << batch.size() << " transactions" << std::endl;
        Block committedBlock;
        if (m_consensus.proposeBlock(batch, committedBlock))
        {
          std::cout << "Consensus: block committed" << std::endl;
          // Minimum interval between blocks to prevent spam
          std::this_thread::sleep_for(std::chrono::milliseconds(SidechainConfig::MIN_BLOCK_INTERVAL_MS));
        }
        else
        {
          std::cout << "Consensus: proposal failed, re-queuing" << std::endl;
          std::lock_guard<std::mutex> lock(m_txMutex);
          for (const auto &tx : batch)
            m_pendingTransactions.push(tx);
        }
      }

      for (int i = 0; i < 10 && m_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  bool SidechainValidator::validateTransaction(const Transaction &tx) const
  {
    // CreateToken: rate-limited, bypasses balance check (bootstraps the chain)
    if (tx.type == TransactionType::CreateToken)
    {
      if (!m_storage.canCreateToken(tx.from, m_storage.topBlockHeight()))
      {
        std::cout << "validateTransaction: rate limited, address created token too recently" << std::endl;
        return false;
      }
      return true;
    }

    // AMM transactions: validated internally by the engine during execution
    if (tx.type == TransactionType::AmmCreatePool ||
        tx.type == TransactionType::AmmAddLiquidity ||
        tx.type == TransactionType::AmmRemoveLiquidity ||
        tx.type == TransactionType::AmmSwap)
    {
      return true;
    }

    // Amount must be > 0 for transfers
    if (tx.amount == 0 && tx.type == TransactionType::Transfer)
      return false;

    // Fee check — testnet uses 0, mainnet uses MINIMUM_FEE
    uint64_t minFee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::MINIMUM_FEE;
    if (tx.fee < minFee)
    {
      std::cout << "validateTransaction: fee too low fee=" << tx.fee
                << " minFee=" << minFee << std::endl;
      return false;
    }

    // Token validation for non-native tokens
    if (tx.tokenId > 0)
    {
      TokenInfo token;
      if (!m_storage.getToken(tx.tokenId, token))
      {
        std::cout << "validateTransaction: token not found id=" << tx.tokenId << std::endl;
        return false;
      }

      if (token.backingModel != TokenBackingModel::Unbacked)
      {
        if (!validateTokenBacking(tx, token))
          return false;
      }

      // Allow fees to be paid in SCCX (token ID 0) or the token being transferred
      if (tx.feeTokenId != 0 && tx.feeTokenId != tx.tokenId)
      {
        std::cout << "validateTransaction: invalid fee token feeTokenId="
                  << tx.feeTokenId << " tokenId=" << tx.tokenId << std::endl;
        return false;
      }
    }

    // Balance check for the token being transferred
    uint64_t tokenBalance = 0;
    m_storage.getBalance(tx.from, tx.tokenId, tokenBalance);

    // If fee is paid in the same token, add fee to required amount
    uint64_t requiredTokenAmount = tx.amount;
    if (tx.feeTokenId == tx.tokenId)
      requiredTokenAmount += tx.fee;

    std::cout << "validateTransaction: from=" << common::podToHex(tx.from).substr(0, 16)
              << " tokenId=" << tx.tokenId << " balance=" << tokenBalance
              << " amount=" << tx.amount << " fee=" << tx.fee
              << " feeTokenId=" << tx.feeTokenId << std::endl;

    if (tokenBalance < requiredTokenAmount)
    {
      std::cout << "validateTransaction: insufficient token balance" << std::endl;
      return false;
    }

    // If fee is paid in a different token, check that balance too
    if (tx.feeTokenId != tx.tokenId && tx.fee > 0)
    {
      uint64_t feeBalance = 0;
      m_storage.getBalance(tx.from, tx.feeTokenId, feeBalance);
      if (feeBalance < tx.fee)
      {
        std::cout << "validateTransaction: insufficient fee balance, have=" << feeBalance
                  << " need=" << tx.fee << " feeTokenId=" << tx.feeTokenId << std::endl;
        return false;
      }
    }

    return true;
  }

  bool SidechainValidator::validateTokenBacking(const Transaction &tx, const TokenInfo &token) const
  {
    if (tx.type == TransactionType::Mint)
    {
      uint64_t newSupply = token.totalSupply + tx.amount;
      uint64_t maxAllowed = token.lockedCCXAmount * token.backingRatio;
      if (newSupply > maxAllowed)
        return false;
    }
    return true;
  }
}