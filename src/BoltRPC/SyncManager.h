// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Blockchain/BlockchainFilter.h"
#include "BoltSync/BoltSync.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Common/StringTools.h"

namespace cn
{
  class INode;
}

namespace BoltRPC
{

  // ── Data structures ────────────────────────────────────────────────────────

  struct OutputInfo
  {
    uint32_t blockHeight;
    crypto::Hash txHash;
    uint64_t amount;
    uint32_t outputIndex;
    crypto::PublicKey outputKey;
    crypto::PublicKey txPublicKey;
    bool spent;
    bool isDeposit;
    uint32_t term;
  };

  struct FilterCandidate
  {
    uint32_t blockHeight;
    uint16_t txIndex;
    uint8_t outputIndex;
  };

  struct SyncProgress
  {
    enum Phase
    {
      IDLE,
      FETCHING_FILTERS,   // Pre-fork: downloading filter records
      FILTERING,          // Pre-fork: running view-tag filter locally
      FETCHING_BLOCKS,    // Fetching full blocks (post-fork batches or candidate blocks)
      SCANNING_POST_FORK, // Post-fork: scanning blocks with on-chain view tags
      DERIVING,           // Full ECDH derivation on candidates
      INCREMENTAL,        // Normal background polling
      COMPLETE
    };

    Phase phase = IDLE;
    uint32_t totalBlocks = 0;
    uint32_t processedBlocks = 0;
    uint32_t candidatesFound = 0;
    uint32_t ownedOutputs = 0;
    uint32_t currentHeight = 0;
    std::string errorMessage;
  };

  // Callback for daemon JSON-RPC calls: takes method name + params JSON, returns response body
  using DaemonRpcCallback = std::function<std::string(const std::string &method, const std::string &paramsJson)>;

  // ── SyncManager ────────────────────────────────────────────────────────────

  class SyncManager
  {
  public:
    using ProgressCallback = std::function<void(const SyncProgress &)>;
    using OutputCallback = std::function<void(const std::vector<OutputInfo> &newOutputs,
                                              const std::vector<crypto::KeyImage> &spentKeyImages)>;

    SyncManager(cn::INode &node,
                const crypto::SecretKey &viewSecretKey,
                const crypto::PublicKey &spendPublicKey,
                const std::string &dataDir,
                DaemonRpcCallback rpcCallback);
    ~SyncManager();

    // ── Lifecycle ──────────────────────────────────────────────────────────
    void start(ProgressCallback onProgress, OutputCallback onOutputs);
    void stop();
    void syncNow();

    // ── Getters ────────────────────────────────────────────────────────────
    uint32_t lastScannedHeight() const { return m_lastScannedHeight.load(); }
    bool isActive() const { return m_active.load(); }

  private:
    // ── Internal sync methods ──────────────────────────────────────────────
    void runLoop();
    void doBootstrap(SyncProgress &progress);
    void doIncrementalSync(SyncProgress &progress);

    // ── Pre-fork: filter-based sync ────────────────────────────────────────
    bool syncPreForkFilters(uint32_t startHeight, uint32_t endHeight,
                            SyncProgress &progress,
                            std::vector<OutputInfo> &owned);

    // ── Post-fork: direct block scanning with on-chain view tags ───────────
    void scanPostForkBlocks(uint32_t startHeight, uint32_t endHeight,
                            SyncProgress &progress,
                            std::vector<OutputInfo> &owned);

    // ── Daemon communication ───────────────────────────────────────────────
    bool callGetFilterRecords(uint32_t startHeight, uint32_t endHeight,
                              std::vector<cn::BlockFilterRecord> &records,
                              uint32_t &chainHeight);

    bool callGetBlocks(const std::vector<uint32_t> &heights,
                       std::vector<cn::Block> &blocks,
                       std::vector<std::vector<cn::Transaction>> &transactions);

    // ── View-tag filter ────────────────────────────────────────────────────
    void runFilterPass(const std::vector<cn::BlockFilterRecord> &records,
                       std::vector<FilterCandidate> &candidates);

    // ── Full derivation on candidates (pre-fork only) ─────────────────────
    void deriveFromCandidates(const std::vector<FilterCandidate> &candidates,
                              const std::vector<cn::Block> &blocks,
                              const std::vector<std::vector<cn::Transaction>> &transactions,
                              std::vector<OutputInfo> &owned);

    bool isOurOutput(const crypto::PublicKey &txPubKey,
                     size_t outputIndex,
                     const crypto::PublicKey &outputKey) const;

    // ── Helpers ────────────────────────────────────────────────────────────
    uint32_t getForkHeight() const;

    // ── Persistence ────────────────────────────────────────────────────────
    void loadProgress();
    void saveProgress();
    std::string progressPath() const;

    // ── Members ────────────────────────────────────────────────────────────
    cn::INode &m_node;
    crypto::SecretKey m_viewSecretKey;
    crypto::PublicKey m_spendPublicKey;
    std::string m_dataDir;
    DaemonRpcCallback m_rpcCallback;

    std::atomic<uint32_t> m_lastScannedHeight{0};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_triggerSync{false};

    ProgressCallback m_onProgress;
    OutputCallback m_onOutputs;
    std::thread m_thread;

    static const uint32_t POLL_INTERVAL_SECONDS = 30;
    static const uint32_t FILTER_BATCH_SIZE = 1000;
    static const uint32_t BLOCK_BATCH_SIZE = 100;
    static const uint32_t POST_FORK_BLOCK_BATCH = 100; // blocks per batch for post-fork scanning
    static const uint32_t MAX_OUTPUTS_PER_CALL = 50000;
  };

} // namespace BoltRPC