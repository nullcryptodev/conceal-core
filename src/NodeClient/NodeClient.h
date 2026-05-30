// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "INode.h"

namespace NodeClient
{

  // ─── NodeClient ──────────────────────────────────────────────────────────
  //
  // A lightweight cn::INode implementation that uses BoltHttp::HttpClient
  // instead of the legacy coroutine-based HttpClient.
  // No Dispatcher, no coroutines. Plain synchronous HTTP.

  class NodeClient : public cn::INode
  {
  public:
    NodeClient(const std::string &host, uint16_t port);
    ~NodeClient() override;

    // ── Connection ──────────────────────────────────────────────────────
    bool init();
    bool shutdown() override;
    bool isConnected() const;

    // ── cn::INode interface ─────────────────────────────────────────────
    bool addObserver(cn::INodeObserver *observer) override;
    bool removeObserver(cn::INodeObserver *observer) override;

    void init(const Callback &callback) override;

    size_t getPeerCount() const override;
    uint32_t getLastLocalBlockHeight() const override;
    uint32_t getLastKnownBlockHeight() const override;
    uint32_t getLocalBlockCount() const override;
    uint32_t getKnownBlockCount() const override;
    uint64_t getLastLocalBlockTimestamp() const override;

    void relayTransaction(const cn::Transaction &transaction, const Callback &callback) override;
    void getRandomOutsByAmounts(std::vector<uint64_t> &&amounts, uint64_t outsCount,
                                std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &result,
                                const Callback &callback) override;
    void getNewBlocks(std::vector<crypto::Hash> &&knownBlockIds,
                      std::vector<cn::block_complete_entry> &newBlocks,
                      uint32_t &startHeight, const Callback &callback) override;
    void getTransactionOutsGlobalIndices(const crypto::Hash &transactionHash,
                                         std::vector<uint32_t> &outsGlobalIndices,
                                         const Callback &callback) override;
    void queryBlocks(std::vector<crypto::Hash> &&knownBlockIds, uint64_t timestamp,
                     std::vector<cn::BlockShortEntry> &newBlocks,
                     uint32_t &startHeight, const Callback &callback) override;
    void getPoolSymmetricDifference(std::vector<crypto::Hash> &&knownPoolTxIds,
                                    crypto::Hash knownBlockId, bool &isBcActual,
                                    std::vector<std::unique_ptr<cn::ITransactionReader>> &newTxs,
                                    std::vector<crypto::Hash> &deletedTxIds,
                                    const Callback &callback) override;
    void getMultisignatureOutputByGlobalIndex(uint64_t amount, uint32_t gindex,
                                              cn::MultisignatureOutput &out,
                                              const Callback &callback) override;
    void getBlocks(const std::vector<uint32_t> &blockHeights,
                   std::vector<std::vector<cn::BlockDetails>> &blocks,
                   const Callback &callback) override;
    void getBlocks(const std::vector<crypto::Hash> &blockHashes,
                   std::vector<cn::BlockDetails> &blocks,
                   const Callback &callback) override;
    void getBlocks(uint64_t timestampBegin, uint64_t timestampEnd, uint32_t blocksNumberLimit,
                   std::vector<cn::BlockDetails> &blocks,
                   uint32_t &blocksNumberWithinTimestamps, const Callback &callback) override;
    void getTransactions(const std::vector<crypto::Hash> &transactionHashes,
                         std::vector<cn::TransactionDetails> &transactions,
                         const Callback &callback) override;
    void getTransactionsByPaymentId(const crypto::Hash &paymentId,
                                    std::vector<cn::TransactionDetails> &transactions,
                                    const Callback &callback) override;
    void getPoolTransactions(uint64_t timestampBegin, uint64_t timestampEnd,
                             uint32_t transactionsNumberLimit,
                             std::vector<cn::TransactionDetails> &transactions,
                             uint64_t &transactionsNumberWithinTimestamps,
                             const Callback &callback) override;
    void isSynchronized(bool &syncStatus, const Callback &callback) override;
    void getTransaction(const crypto::Hash &transactionHash,
                        cn::Transaction &transaction, const Callback &callback) override;

    // Sync versions
    std::vector<crypto::Hash> getPoolTransactions() override;
    bool getTransactionSync(const crypto::Hash &txHash, cn::Transaction &tx) override;

    // Additional helpers used by SyncManager
    void getBlockDetailsByHeight(uint32_t blockHeight, cn::BlockDetails &blockDetails);
    void getBlocksByHeight(const std::vector<uint32_t> &blockHeights,
                           std::vector<std::vector<cn::BlockDetails>> &blocks);

  private:
    std::string jsonRpcCall(const std::string &method, const std::string &paramsJson);
    std::string m_host;
    uint16_t m_port;
    std::atomic<bool> m_connected{false};

    mutable std::mutex m_mutex;
    uint32_t m_lastLocalBlockHeight = 0;
    uint32_t m_lastKnownBlockHeight = 0;
    uint64_t m_lastLocalBlockTimestamp = 0;
    size_t m_peerCount = 0;
  };

} // namespace NodeClient