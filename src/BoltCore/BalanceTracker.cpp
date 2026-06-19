// BalanceTracker implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BalanceTracker.h"
#include "OutputUtils.h"

#include <boost/functional/hash.hpp>
#include <ctime>
#include <cstring>
#include <unordered_map>

namespace BoltCore
{
  namespace
  {
    static const crypto::PublicKey NULL_OUTPUT_KEY = {};
  }

  size_t BalanceTracker::OutputRefHash::operator()(const std::pair<crypto::Hash, uint32_t> &ref) const
  {
    return boost::hash<crypto::Hash>()(ref.first) ^ std::hash<uint32_t>()(ref.second);
  }

  bool BalanceTracker::outputExists(const crypto::Hash &txHash, uint32_t outputIndex) const
  {
    for (const auto &existing : m_outputs)
    {
      if (existing.txHash == txHash && existing.outputIndex == outputIndex)
        return true;
    }
    return false;
  }

  OutputInfo *BalanceTracker::findExistingOutput(const OutputInfo &out)
  {
    // Same tx + output key is the stable identity (output index can differ between scans).
    if (out.outputKey != NULL_OUTPUT_KEY)
    {
      for (auto &existing : m_outputs)
      {
        if (existing.txHash != out.txHash || existing.isDeposit != out.isDeposit || existing.spent)
          continue;
        if (existing.outputKey == out.outputKey)
          return &existing;
      }
    }

    OutputInfo *byIndex = nullptr;
    OutputInfo *byKey = nullptr;
    OutputInfo *byGlobal = nullptr;
    for (auto &existing : m_outputs)
    {
      if (existing.txHash != out.txHash)
        continue;
      if (existing.outputIndex == out.outputIndex)
        byIndex = &existing;
      if (out.outputKey != NULL_OUTPUT_KEY && existing.outputKey == out.outputKey)
        byKey = &existing;
      if (out.hasGlobalOutputIndex && existing.hasGlobalOutputIndex &&
          out.globalOutputIndex == existing.globalOutputIndex)
        byGlobal = &existing;
    }
    if (byIndex)
      return byIndex;
    if (byKey)
      return byKey;
    if (byGlobal)
      return byGlobal;

    // Re-scan may report a different output index or omit the key for the same receive.
    if (!out.isDeposit && !out.spent)
    {
      OutputInfo *byAmount = nullptr;
      uint32_t amountMatches = 0;
      for (auto &existing : m_outputs)
      {
        if (existing.txHash != out.txHash || existing.isDeposit || existing.spent)
          continue;
        if (existing.amount != out.amount)
          continue;
        if (out.blockHeight > 0 && existing.blockHeight > 0 &&
            out.blockHeight != existing.blockHeight)
          continue;
        byAmount = &existing;
        ++amountMatches;
        if (amountMatches > 1)
          break;
      }
      if (amountMatches == 1)
        return byAmount;
    }

    // Mempool pool id vs confirmed tx blob can disagree on hash; promote pending by key/amount.
    if (!out.isDeposit && !out.spent && out.blockHeight > 0)
    {
      if (out.outputKey != NULL_OUTPUT_KEY)
      {
        OutputInfo *byPendingKey = nullptr;
        uint32_t keyMatches = 0;
        for (auto &existing : m_outputs)
        {
          if (existing.spent || existing.isDeposit || existing.blockHeight != 0)
            continue;
          if (existing.outputKey != out.outputKey)
            continue;
          byPendingKey = &existing;
          if (++keyMatches > 1)
            break;
        }
        if (keyMatches == 1)
          return byPendingKey;
      }

      OutputInfo *byPendingAmount = nullptr;
      uint32_t pendingAmountMatches = 0;
      for (auto &existing : m_outputs)
      {
        if (existing.spent || existing.isDeposit || existing.blockHeight != 0)
          continue;
        if (existing.amount != out.amount)
          continue;
        byPendingAmount = &existing;
        if (++pendingAmountMatches > 1)
          break;
      }
      if (pendingAmountMatches == 1)
        return byPendingAmount;
    }

    return nullptr;
  }

  void BalanceTracker::mergeOutputFields(OutputInfo &existing, const OutputInfo &out)
  {
    if (out.blockHeight > 0 && existing.blockHeight == 0)
    {
      existing.blockHeight = out.blockHeight;
      static const crypto::Hash NULL_HASH = {};
      if (existing.txHash != out.txHash && out.txHash != NULL_HASH)
        existing.txHash = out.txHash;
    }
    if (out.hasGlobalOutputIndex && !existing.hasGlobalOutputIndex)
    {
      existing.globalOutputIndex = out.globalOutputIndex;
      existing.hasGlobalOutputIndex = true;
    }
    if (out.hasKeyDerivationIndex)
    {
      if (!existing.hasKeyDerivationIndex ||
          existing.keyDerivationIndex != out.keyDerivationIndex)
      {
        existing.keyDerivationIndex = out.keyDerivationIndex;
        existing.hasKeyDerivationIndex = true;
      }
    }
    else if (existing.hasKeyDerivationIndex)
    {
      existing.hasKeyDerivationIndex = false;
    }
    if (out.isDeposit && !existing.isDeposit)
      existing.isDeposit = true;
    if (out.term != 0 && existing.term == 0)
      existing.term = out.term;
    if (existing.outputKey == NULL_OUTPUT_KEY && out.outputKey != NULL_OUTPUT_KEY)
      existing.outputKey = out.outputKey;
    if (existing.keyImage == crypto::KeyImage{} && out.keyImage != crypto::KeyImage{})
      existing.keyImage = out.keyImage;
  }

  void BalanceTracker::rebuildCreditedRefs()
  {
    m_creditedRefs.clear();
    for (const auto &out : m_outputs)
    {
      if (out.spent || out.blockHeight == 0)
        continue;
      m_creditedRefs.insert(std::make_pair(out.txHash, out.outputIndex));
    }
  }

  void BalanceTracker::refreshDepositBuckets()
  {
    m_total.lockedDeposit = 0;
    m_total.unlockedDeposit = 0;
    for (auto &pair : m_byAddress)
    {
      pair.second.lockedDeposit = 0;
      pair.second.unlockedDeposit = 0;
    }

    for (const auto &out : m_outputs)
    {
      if (out.spent || !out.isDeposit)
        continue;

      AddressBalance &addr = m_byAddress[out.subAddress];
      if (isDepositUnlocked(out, m_currentHeight))
      {
        addr.unlockedDeposit += out.amount;
        m_total.unlockedDeposit += out.amount;
      }
      else
      {
        addr.lockedDeposit += out.amount;
        m_total.lockedDeposit += out.amount;
      }
    }
  }

  void BalanceTracker::subtractFromBalance(const OutputInfo &out)
  {
    Balance &total = m_total;
    AddressBalance &addr = m_byAddress[out.subAddress];

    if (out.isDeposit)
    {
      if (isDepositUnlocked(out, m_currentHeight))
      {
        addr.unlockedDeposit -= out.amount;
        total.unlockedDeposit -= out.amount;
      }
      else
      {
        addr.lockedDeposit -= out.amount;
        total.lockedDeposit -= out.amount;
      }
    }
    else
    {
      addr.actual -= out.amount;
      total.actual -= out.amount;
    }
  }

  void BalanceTracker::loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    m_currentHeight = currentHeight;
    m_outputs.clear();
    m_creditedRefs.clear();
    m_byAddress.clear();
    m_transactions.clear();
    m_total = {0, 0, 0, 0, 0, 0, currentHeight};

    for (const auto &out : outputs)
    {
      addOutput(out);
    }
    compactDuplicateOutputs();
    rebuildActualBalance();
    rebuildCreditedRefs();
    refreshDepositBuckets();
  }

  bool BalanceTracker::mergeOutput(const OutputInfo &out)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    if (OutputInfo *existing = findExistingOutput(out))
    {
      mergeOutputFields(*existing, out);
      return true;
    }
    return false;
  }

  void BalanceTracker::creditOutputToBalance(const OutputInfo &out)
  {
    if (out.spent)
      return;

    Balance &total = m_total;
    AddressBalance &addr = m_byAddress[out.subAddress];

    if (out.isDeposit)
    {
      if (isDepositUnlocked(out, m_currentHeight))
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

  void BalanceTracker::creditOutputToBalanceOnce(const OutputInfo &out)
  {
    if (out.spent || out.blockHeight == 0)
      return;

    const auto ref = std::make_pair(out.txHash, out.outputIndex);
    if (m_creditedRefs.count(ref))
      return;

    if (out.outputKey != NULL_OUTPUT_KEY)
    {
      for (const auto &existing : m_outputs)
      {
        if (existing.txHash != out.txHash || existing.outputKey != out.outputKey)
          continue;
        const auto existingRef = std::make_pair(existing.txHash, existing.outputIndex);
        if (m_creditedRefs.count(existingRef))
          return;
      }
    }

    if (txHasCreditedAmount(out.txHash, out.amount, out.isDeposit))
      return;

    creditOutputToBalance(out);
    m_creditedRefs.insert(ref);
  }

  bool BalanceTracker::txHasCreditedAmount(const crypto::Hash &txHash, uint64_t amount,
                                           bool isDeposit) const
  {
    for (const auto &existing : m_outputs)
    {
      if (existing.txHash != txHash || existing.isDeposit != isDeposit || existing.amount != amount)
        continue;
      const auto existingRef = std::make_pair(existing.txHash, existing.outputIndex);
      if (m_creditedRefs.count(existingRef))
        return true;
    }
    return false;
  }

  bool BalanceTracker::txHasCreditedIncoming(const crypto::Hash &txHash) const
  {
    for (const auto &ref : m_creditedRefs)
    {
      if (ref.first != txHash)
        continue;
      for (const auto &out : m_outputs)
      {
        if (out.txHash == ref.first && out.outputIndex == ref.second && !out.isDeposit)
          return true;
      }
    }
    return false;
  }

  bool BalanceTracker::isSamePhysicalOutput(const OutputInfo &a, const OutputInfo &b)
  {
    if (a.isDeposit != b.isDeposit)
      return false;
    if (a.txHash != b.txHash)
      return false;
    if (a.outputIndex == b.outputIndex)
      return true;
    if (a.outputKey != NULL_OUTPUT_KEY && b.outputKey != NULL_OUTPUT_KEY &&
        a.outputKey == b.outputKey)
      return true;
    if (a.hasGlobalOutputIndex && b.hasGlobalOutputIndex &&
        a.globalOutputIndex == b.globalOutputIndex)
      return true;
    if (!a.isDeposit && !b.isDeposit && !a.spent && !b.spent && a.amount == b.amount)
    {
      if (a.blockHeight > 0 && b.blockHeight > 0 && a.blockHeight == b.blockHeight)
        return true;
      if (a.blockHeight == 0 || b.blockHeight == 0)
        return true;
    }
    return false;
  }

  void BalanceTracker::rebuildActualBalance()
  {
    m_total.actual = 0;
    for (auto &pair : m_byAddress)
      pair.second.actual = 0;

    for (size_t i = 0; i < m_outputs.size(); ++i)
    {
      const auto &out = m_outputs[i];
      if (out.spent || out.blockHeight == 0 || out.isDeposit)
        continue;

      bool duplicate = false;
      for (size_t j = 0; j < i; ++j)
      {
        if (isSamePhysicalOutput(m_outputs[j], out))
        {
          duplicate = true;
          break;
        }
      }
      if (duplicate)
        continue;

      AddressBalance &addr = m_byAddress[out.subAddress];
      addr.actual += out.amount;
      m_total.actual += out.amount;
    }
  }

  void BalanceTracker::compactDuplicateOutputs()
  {
    std::vector<OutputInfo> unique;
    unique.reserve(m_outputs.size());
    for (const auto &out : m_outputs)
    {
      OutputInfo *existing = nullptr;
      for (auto &candidate : unique)
      {
        if (isSamePhysicalOutput(candidate, out))
        {
          existing = &candidate;
          break;
        }
      }
      if (existing)
        mergeOutputFields(*existing, out);
      else
        unique.push_back(out);
    }
    m_outputs.swap(unique);
  }

  bool BalanceTracker::incomingTxAlreadyCredited(const crypto::Hash &txHash) const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return txHasCreditedIncoming(txHash);
  }

  bool BalanceTracker::txHasUnconfirmedOutputs(const crypto::Hash &txHash) const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (const auto &out : m_outputs)
    {
      if (out.txHash == txHash && out.blockHeight == 0)
        return true;
    }
    return false;
  }

  bool BalanceTracker::incomingTxBatchAlreadyRecorded(const crypto::Hash &txHash,
                                                      uint32_t blockHeight,
                                                      const std::vector<OutputInfo> &outputs) const
  {
    if (blockHeight == 0 || outputs.empty())
      return false;

    uint32_t batchIncoming = 0;
    for (const auto &out : outputs)
    {
      if (out.isDeposit || out.spent)
        continue;
      ++batchIncoming;
    }
    if (batchIncoming == 0)
      return false;

    uint32_t walletIncoming = 0;
    for (const auto &out : m_outputs)
    {
      if (out.txHash != txHash || out.isDeposit || out.spent || out.blockHeight != blockHeight)
        continue;
      ++walletIncoming;
    }
    return walletIncoming >= batchIncoming;
  }

  void BalanceTracker::mergeDiscoveredOutputs(const crypto::Hash &txHash,
                                              const std::vector<OutputInfo> &outputs,
                                              uint32_t blockHeight)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (const auto &out : outputs)
    {
      OutputInfo ingest = out;
      ingest.txHash = txHash;
      ingest.blockHeight = blockHeight > 0 ? blockHeight : 0;
      if (OutputInfo *existing = findExistingOutput(ingest))
        mergeOutputFields(*existing, ingest);
    }
    compactDuplicateOutputs();
    rebuildActualBalance();
    rebuildCreditedRefs();
  }

  void BalanceTracker::applyDiscoveredOutputs(const crypto::Hash &txHash,
                                              const std::vector<OutputInfo> &outputs,
                                              uint32_t blockHeight)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);

    bool known = false;
    for (const auto &existing : m_outputs)
    {
      if (existing.txHash == txHash)
      {
        known = true;
        break;
      }
    }

    bool outgoing = false;
    {
      std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
      auto it = m_pendingOutgoing.find(txHash);
      if (it != m_pendingOutgoing.end())
      {
        known = true;
        outgoing = !it->second.incoming;
      }
    }

    // Confirmed incoming tx already credited — merge metadata only, never add or credit again.
    if (known && blockHeight > 0 && !outgoing &&
        (txHasCreditedIncoming(txHash) ||
         incomingTxBatchAlreadyRecorded(txHash, blockHeight, outputs)))
    {
      for (const auto &out : outputs)
      {
        OutputInfo ingest = out;
        ingest.txHash = txHash;
        ingest.blockHeight = blockHeight;
        if (OutputInfo *existing = findExistingOutput(ingest))
          mergeOutputFields(*existing, ingest);
      }
      compactDuplicateOutputs();
      rebuildActualBalance();
      rebuildCreditedRefs();
      return;
    }

    for (const auto &out : outputs)
    {
      OutputInfo ingest = out;
      ingest.txHash = txHash;
      ingest.blockHeight = blockHeight > 0 ? blockHeight : 0;

      if (OutputInfo *existing = findExistingOutput(ingest))
      {
        const bool promote =
            existing->blockHeight == 0 && ingest.blockHeight > 0 && !existing->spent;
        mergeOutputFields(*existing, ingest);
        if (promote)
          creditOutputToBalanceOnce(*existing);
        continue;
      }

      // Promote mempool row when confirm metadata differs (index/key mismatch).
      if (known && !outgoing && ingest.blockHeight > 0)
      {
        bool handled = false;
        for (auto &existing : m_outputs)
        {
          if (existing.txHash != txHash || existing.blockHeight != 0 || existing.spent)
            continue;
          if (existing.isDeposit != ingest.isDeposit || existing.amount != ingest.amount)
            continue;
          mergeOutputFields(existing, ingest);
          creditOutputToBalanceOnce(existing);
          handled = true;
          break;
        }
        if (handled)
          continue;
      }

      if (ingest.blockHeight == 0)
      {
        m_outputs.push_back(ingest);
        continue;
      }

      m_outputs.push_back(ingest);
      if (!ingest.spent)
        creditOutputToBalanceOnce(ingest);
    }

    compactDuplicateOutputs();
    rebuildActualBalance();
    rebuildCreditedRefs();
  }

  void BalanceTracker::addUnconfirmedOutput(const OutputInfo &out)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    if (findExistingOutput(out))
      return;

    OutputInfo unconfirmed = out;
    unconfirmed.blockHeight = 0;
    m_outputs.push_back(unconfirmed);
  }

  bool BalanceTracker::ingestOutput(const OutputInfo &out)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);

    if (OutputInfo *existing = findExistingOutput(out))
    {
      const bool promote = existing->blockHeight == 0 && out.blockHeight > 0 && !existing->spent;
      mergeOutputFields(*existing, out);

      if (promote)
      {
        creditOutputToBalanceOnce(*existing);
        return true;
      }
      return false;
    }

    if (out.blockHeight == 0)
    {
      OutputInfo unconfirmed = out;
      unconfirmed.blockHeight = 0;
      m_outputs.push_back(unconfirmed);
      return false;
    }

    m_outputs.push_back(out);
    if (!out.spent)
      creditOutputToBalanceOnce(out);
    return !out.spent;
  }

  void BalanceTracker::addOutput(const OutputInfo &out)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    if (findExistingOutput(out))
      return;

    m_outputs.push_back(out);

    if (out.spent || out.blockHeight == 0)
      return;

    creditOutputToBalanceOnce(out);
  }

  bool BalanceTracker::markSpent(const crypto::KeyImage &keyImage)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    static const crypto::KeyImage NULL_KI = {};
    if (std::memcmp(&keyImage, &NULL_KI, sizeof(crypto::KeyImage)) == 0)
      return false;

    for (auto &out : m_outputs)
    {
      if (out.keyImage == keyImage && !out.spent)
      {
        if (out.isDeposit)
          continue;
        out.spent = true;
        rebuildActualBalance();
        refreshDepositBuckets();
        return true;
      }
    }
    return false;
  }

  bool BalanceTracker::markSpentByRef(const crypto::Hash &txHash, uint32_t outputIndex)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (auto &out : m_outputs)
    {
      if (out.txHash == txHash && out.outputIndex == outputIndex && !out.isDeposit && !out.spent)
      {
        out.spent = true;
        rebuildActualBalance();
        refreshDepositBuckets();
        return true;
      }
    }
    return false;
  }

  bool BalanceTracker::markDepositSpent(const crypto::Hash &txHash, uint32_t outputIndex)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (auto &out : m_outputs)
    {
      if (out.txHash == txHash && out.outputIndex == outputIndex && out.isDeposit && !out.spent)
      {
        out.spent = true;
        rebuildActualBalance();
        refreshDepositBuckets();
        return true;
      }
    }
    return false;
  }

  void BalanceTracker::setCurrentHeight(uint32_t height)
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    if (height == m_currentHeight)
      return;
    m_currentHeight = height;
    refreshDepositBuckets();
  }

  uint32_t BalanceTracker::getCurrentHeight() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_currentHeight;
  }

  Balance BalanceTracker::getTotalBalance() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    Balance balance = m_total;
    balance.currentHeight = m_currentHeight;
    {
      std::lock_guard<std::mutex> pendingLock(m_pendingMutex);
      balance.pending = m_pendingIncomingAmount;
    }
    return balance;
  }

  Balance BalanceTracker::getBalance(const std::string &address) const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    auto it = m_byAddress.find(address);
    if (it != m_byAddress.end())
    {
      return {it->second.actual, it->second.pending,
              it->second.lockedDeposit, it->second.unlockedDeposit, 0, 0, m_currentHeight};
    }
    return {0, 0, 0, 0, 0, 0, m_currentHeight};
  }

  uint64_t BalanceTracker::totalActual() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_total.actual;
  }

  uint64_t BalanceTracker::totalPending() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return m_total.pending;
  }

  std::vector<OutputInfo> BalanceTracker::getOutputs() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    std::vector<OutputInfo> unique;
    unique.reserve(m_outputs.size());
    for (const auto &out : m_outputs)
    {
      OutputInfo *existing = nullptr;
      for (auto &candidate : unique)
      {
        if (isSamePhysicalOutput(candidate, out))
        {
          existing = &candidate;
          break;
        }
      }
      if (existing)
        mergeOutputFields(*existing, out);
      else
        unique.push_back(out);
    }
    return unique;
  }

  void BalanceTracker::addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee)
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    PendingTx pending;
    pending.txHash = txHash;
    pending.amount = amount;
    pending.fee = fee;
    pending.timestamp = std::time(nullptr);
    pending.incoming = false;

    m_pendingOutgoing[txHash] = pending;
    m_pendingOutgoingAmount += amount + fee;
  }

  void BalanceTracker::addPendingIncoming(const crypto::Hash &txHash, uint64_t amount)
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    auto it = m_pendingOutgoing.find(txHash);
    if (it != m_pendingOutgoing.end() && it->second.incoming)
      return;

    PendingTx pending;
    pending.txHash = txHash;
    pending.amount = amount;
    pending.fee = 0;
    pending.timestamp = std::time(nullptr);
    pending.incoming = true;

    m_pendingOutgoing[txHash] = pending;
    m_pendingIncomingAmount += amount;
  }

  bool BalanceTracker::hasPendingOutgoing(const crypto::Hash &txHash) const
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    auto it = m_pendingOutgoing.find(txHash);
    return it != m_pendingOutgoing.end() && !it->second.incoming;
  }

  bool BalanceTracker::hasTransaction(const crypto::Hash &txHash) const
  {
    {
      std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
      for (const auto &out : m_outputs)
      {
        if (out.txHash == txHash)
          return true;
      }
    }

    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_pendingOutgoing.find(txHash) != m_pendingOutgoing.end();
  }

  void BalanceTracker::confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight)
  {
    (void)blockHeight;
    std::lock_guard<std::mutex> lock(m_pendingMutex);

    auto it = m_pendingOutgoing.find(txHash);
    if (it != m_pendingOutgoing.end())
    {
      if (it->second.incoming)
        m_pendingIncomingAmount -= it->second.amount;
      else
        m_pendingOutgoingAmount -= it->second.amount + it->second.fee;
      m_pendingOutgoing.erase(it);
    }
  }

  uint64_t BalanceTracker::getPendingIncomingAmount() const
  {
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_pendingIncomingAmount;
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
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    for (const auto &existing : m_transactions)
    {
      if (existing.txHash == tx.txHash)
        return;
    }
    m_transactions.push_back(tx);
  }

  std::vector<TransactionRecord> BalanceTracker::getTransactions(uint32_t offset, uint32_t limit) const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    std::vector<TransactionRecord> result;
    if (offset >= m_transactions.size())
      return result;

    uint32_t end = std::min(offset + limit, static_cast<uint32_t>(m_transactions.size()));
    result.assign(m_transactions.begin() + offset, m_transactions.begin() + end);
    return result;
  }

  uint32_t BalanceTracker::getTransactionCount() const
  {
    std::lock_guard<std::recursive_mutex> lock(m_dataMutex);
    return static_cast<uint32_t>(m_transactions.size());
  }
}
