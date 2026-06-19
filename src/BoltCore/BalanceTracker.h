// BalanceTracker - wallet balance calculation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace BoltCore
{
  class BalanceTracker
  {
  public:
    struct PendingTx
    {
      crypto::Hash txHash;
      uint64_t amount;
      uint64_t fee;
      uint64_t timestamp;
      bool incoming = false;
    };

    void loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight = 0);

    Balance getTotalBalance() const;
    Balance getBalance(const std::string &address) const;

    void addOutput(const OutputInfo &output);
    // Mempool output: tracked in wallet but not counted in Available until confirmed.
    void addUnconfirmedOutput(const OutputInfo &output);
    // Insert or merge; credits Available at most once per (txHash, outputIndex).
    bool ingestOutput(const OutputInfo &output);
    // Update metadata (e.g. global output index) when sync re-discovers an existing output.
    bool mergeOutput(const OutputInfo &output);
    bool markSpent(const crypto::KeyImage &keyImage);
    bool markSpentByRef(const crypto::Hash &txHash, uint32_t outputIndex);
    bool markDepositSpent(const crypto::Hash &txHash, uint32_t outputIndex);

    void setCurrentHeight(uint32_t height);
    uint32_t getCurrentHeight() const;

    uint64_t totalActual() const;
    uint64_t totalPending() const;

    std::vector<OutputInfo> getOutputs() const;

    void addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee);
    void addPendingIncoming(const crypto::Hash &txHash, uint64_t amount);
    bool hasTransaction(const crypto::Hash &txHash) const;
    bool hasPendingOutgoing(const crypto::Hash &txHash) const;
    bool incomingTxAlreadyCredited(const crypto::Hash &txHash) const;
    bool txHasUnconfirmedOutputs(const crypto::Hash &txHash) const;
    bool incomingTxBatchAlreadyRecorded(const crypto::Hash &txHash,
                                        uint32_t blockHeight,
                                        const std::vector<OutputInfo> &outputs) const;
    void mergeDiscoveredOutputs(const crypto::Hash &txHash,
                                const std::vector<OutputInfo> &outputs,
                                uint32_t blockHeight);
    void applyDiscoveredOutputs(const crypto::Hash &txHash,
                                const std::vector<OutputInfo> &outputs,
                                uint32_t blockHeight);
    void confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight);
    uint64_t getPendingOutgoingAmount() const;
    uint64_t getPendingIncomingAmount() const;
    std::vector<PendingTx> getPendingTransactions() const;

    void addTransaction(const TransactionRecord &tx);
    std::vector<TransactionRecord> getTransactions(uint32_t offset = 0, uint32_t limit = 50) const;
    uint32_t getTransactionCount() const;

  private:
    struct AddressBalance
    {
      uint64_t actual = 0;
      uint64_t pending = 0;
      uint64_t lockedDeposit = 0;
      uint64_t unlockedDeposit = 0;
    };

    struct OutputRefHash
    {
      size_t operator()(const std::pair<crypto::Hash, uint32_t> &ref) const;
    };

    bool outputExists(const crypto::Hash &txHash, uint32_t outputIndex) const;
    OutputInfo *findExistingOutput(const OutputInfo &out);
    bool txHasCreditedAmount(const crypto::Hash &txHash, uint64_t amount, bool isDeposit) const;
    bool txHasCreditedIncoming(const crypto::Hash &txHash) const;
    static bool isSamePhysicalOutput(const OutputInfo &a, const OutputInfo &b);
    static void mergeOutputFields(OutputInfo &existing, const OutputInfo &out);
    void creditOutputToBalanceOnce(const OutputInfo &out);
    void creditOutputToBalance(const OutputInfo &out);
    void rebuildActualBalance();
    void rebuildCreditedRefs();
    void compactDuplicateOutputs();
    void subtractFromBalance(const OutputInfo &out);
    void refreshDepositBuckets();

    mutable std::recursive_mutex m_dataMutex;
    mutable std::mutex m_pendingMutex;
    std::unordered_map<crypto::Hash, PendingTx> m_pendingOutgoing;
    uint64_t m_pendingOutgoingAmount = 0;
    uint64_t m_pendingIncomingAmount = 0;
    uint32_t m_currentHeight = 0;
    std::vector<OutputInfo> m_outputs;
    std::unordered_set<std::pair<crypto::Hash, uint32_t>, OutputRefHash> m_creditedRefs;
    std::unordered_map<std::string, AddressBalance> m_byAddress;
    Balance m_total;
    std::vector<TransactionRecord> m_transactions;
  };
}
