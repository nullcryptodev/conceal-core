// BalanceTracker implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BalanceTracker.h"

namespace BoltCore
{
  void BalanceTracker::loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight)
  {
    m_currentHeight = currentHeight;
    m_outputs.clear();
    m_byAddress.clear();
    m_transactions.clear();
    m_total = {0, 0, 0, 0, 0, 0};

    for (const auto &out : outputs)
    {
      addOutput(out);
    }
  }

  void BalanceTracker::addOutput(const OutputInfo &out)
  {
    m_outputs.push_back(out);

    if (out.spent)
      return;

    Balance &total = m_total;
    AddressBalance &addr = m_byAddress[out.subAddress];

    if (out.isDeposit)
    {
      bool unlocked = (out.term > 0) && (m_currentHeight + 1 >= out.blockHeight + out.term);

      if (unlocked)
      {
        addr.unlockedDeposit += out.amount;
        total.unlockedDeposit += out.amount;
      }
      else
      {
        addr.lockedDeposit += out.amount;
        total.lockedDeposit += out.amount;
      }
    }
    else
    {
      addr.actual += out.amount;
      total.actual += out.amount;
    }
  }

  void BalanceTracker::markSpent(const crypto::KeyImage &keyImage)
  {
    for (auto &out : m_outputs)
    {
      if (out.keyImage == keyImage && !out.spent)
      {
        out.spent = true;
        Balance &total = m_total;
        AddressBalance &addr = m_byAddress[out.subAddress];

        if (out.isDeposit)
        {
          addr.lockedDeposit -= out.amount;
          total.lockedDeposit -= out.amount;
        }
        else
        {
          addr.actual -= out.amount;
          total.actual -= out.amount;
        }
        return;
      }
    }
  }

  Balance BalanceTracker::getTotalBalance() const
  {
    return m_total;
  }

  Balance BalanceTracker::getBalance(const std::string &address) const
  {
    auto it = m_byAddress.find(address);
    if (it != m_byAddress.end())
    {
      return {it->second.actual, it->second.pending,
              it->second.lockedDeposit, it->second.unlockedDeposit, 0, m_currentHeight};
    }
    return {0, 0, 0, 0, 0, m_currentHeight};
  }

  uint64_t BalanceTracker::totalActual() const
  {
    return m_total.actual;
  }

  uint64_t BalanceTracker::totalPending() const
  {
    return m_total.pending;
  }

  void BalanceTracker::addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee)
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    PendingTx pending;
    pending.txHash = txHash;
    pending.amount = amount;
    pending.fee = fee;
    pending.timestamp = std::time(nullptr);

    m_pendingOutgoing[txHash] = pending;
    m_pendingOutgoingAmount += amount + fee;
  }

  void BalanceTracker::confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight)
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    auto it = m_pendingOutgoing.find(txHash);
    if (it != m_pendingOutgoing.end())
    {
      m_pendingOutgoingAmount -= it->second.amount + it->second.fee;
      m_pendingOutgoing.erase(it);
    }
  }

  uint64_t BalanceTracker::getPendingOutgoingAmount() const
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_pendingOutgoingAmount;
  }

  std::vector<BalanceTracker::PendingTx> BalanceTracker::getPendingTransactions() const
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    std::vector<PendingTx> result;
    result.reserve(m_pendingOutgoing.size());
    for (const auto &pair : m_pendingOutgoing)
    {
      result.push_back(pair.second);
    }
    return result;
  }

  void BalanceTracker::addTransaction(const TransactionRecord &tx)
  {
    // Avoid duplicates
    for (const auto &existing : m_transactions)
    {
      if (existing.txHash == tx.txHash)
        return;
    }
    m_transactions.push_back(tx);
  }

  std::vector<TransactionRecord> BalanceTracker::getTransactions(uint32_t offset, uint32_t limit) const
  {
    std::vector<TransactionRecord> result;
    if (offset >= m_transactions.size())
      return result;

    uint32_t end = std::min(offset + limit, static_cast<uint32_t>(m_transactions.size()));
    result.assign(m_transactions.begin() + offset, m_transactions.begin() + end);
    return result;
  }

  uint32_t BalanceTracker::getTransactionCount() const
  {
    return static_cast<uint32_t>(m_transactions.size());
  }
}