// SyncEngine - multi-strategy wallet sync for ClientWallet
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/functional/hash.hpp>

#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "BoltRPC/SyncManager.h"

namespace cn
{
  class INode;
}
namespace CryptoNote
{
  class MDBXBlockchainStorage;
}
namespace BoltCore
{
  struct OutputInfo;
}
namespace BoltRPC
{
  struct WalletState;
}

namespace ClientWallet
{

  enum class SyncStrategy
  {
    None,
    DirectScan, // Local MDBX: block-by-block scan with two-pass filter
    Polling,    // Remote daemon: filter records via RPC
    Offline     // State file only
  };

  struct SyncStatus
  {
    SyncStrategy strategy = SyncStrategy::None;
    uint32_t currentHeight = 0;
    uint32_t scannedHeight = 0;
    uint32_t ownedOutputs = 0;
    uint64_t totalReceived = 0;
    bool isSyncing = false;
    bool hasNewOutputs = false;
    std::string error;
  };

  class SyncEngine
  {
  public:
    using OutputCallback = std::function<void(const std::vector<BoltCore::OutputInfo> &newOutputs)>;
    using StatusCallback = std::function<void(const SyncStatus &status)>;
    using SpentCallback = std::function<void(
        const std::vector<crypto::KeyImage> &keyImages,
        const std::vector<std::pair<crypto::Hash, uint32_t>> &depositSpends,
        const std::vector<std::pair<crypto::Hash, uint32_t>> &outputSpends)>;
    // WalletGreen-style: merge global output index onto existing UTXOs (no balance credit).
    using MetadataCallback = std::function<void(const std::vector<BoltCore::OutputInfo> &updates)>;

    SyncEngine(const std::string &dataDir,
               const crypto::SecretKey &viewKey,
               const crypto::PublicKey &spendPub,
               const crypto::SecretKey *spendKey,
               bool enableChainSync = true);
    ~SyncEngine();

    // ── Lifecycle ──────────────────────────────────────────────────────

    void setNode(cn::INode *node);
    void setDaemonRpc(std::function<std::string(const std::string &method, const std::string &params)> rpc);

    void start(OutputCallback onOutputs, StatusCallback onStatus, SpentCallback onSpent = {},
               MetadataCallback onMetadata = {});
    void stop();
    void requestStop() { m_stop = true; }

    // ── Manual triggers ────────────────────────────────────────────────

    void fullSync();
    void incrementalSync();
    void onNewBlockHeight(uint32_t newHeight);
    void notifyDaemonHeight(uint32_t height);
    void syncIfBehind();
    uint32_t peekMdbxTopHeight() const;

    // ── State file ─────────────────────────────────────────────────────

    bool loadStateFile(const std::string &path);
    bool saveStateFile(const std::string &path,
                       const std::vector<BoltCore::OutputInfo> &outputs);
    std::vector<BoltCore::OutputInfo> getCachedOutputs() const;

    // Fill global output indices for cached outputs (MDBX lookup, then daemon RPC).
    std::vector<BoltCore::OutputInfo> backfillMissingGlobalOutputIndices();

    // Spend-time fallback: MDBX block lookup, then daemon get_o_indexes.
    bool lookupGlobalOutputIndex(const BoltCore::OutputInfo &out, uint32_t &globalIndex) const;

    // Merge confirmed scan data into the sync cache (e.g. after mempool inclusion).
    // When a global index is newly stored, walletIndexUpdate receives the cached row for Wallet::mergeOutput.
    void mergeCachedOutput(const BoltCore::OutputInfo &out,
                           BoltCore::OutputInfo *walletIndexUpdate = nullptr);

    // Wallet already credited this incoming tx (mempool confirm) — block scan must not re-dispatch.
    void noteIncomingTxApplied(const crypto::Hash &txHash);

    // Called after a successful transfer/deposit so the cache reflects the spent state
    // and captures the key images computed during signing (needed for MDBX spend detection).
    void notifyOutputsSpent(const std::vector<BoltCore::OutputInfo> &spentInputs);

    struct IncomingMempoolTx
    {
      crypto::Hash txHash;
      uint64_t totalAmount = 0;
      std::vector<BoltCore::OutputInfo> outputs;
    };

    struct MempoolUpdate
    {
      std::vector<IncomingMempoolTx> newIncoming;
      std::vector<crypto::Hash> removedFromPool;
      std::vector<BoltCore::OutputInfo> confirmedOutputs;
    };

    // Poll daemon mempool for incoming transfers not yet in a block.
    MempoolUpdate pollMempool();

    // Fetch outputs from daemon blocks not yet in local MDBX (daemon ahead of mdbx).
    std::vector<BoltCore::OutputInfo> scanDaemonGapBlocks();

    // Scan transaction inputs against cached outputs and mark any key-image matches as spent.
    // Returns the list of newly-spent key images and deposit refs for wallet notification.
    struct SpendDetectionResult
    {
      std::vector<crypto::KeyImage> spentKeyImages;
      std::vector<std::pair<crypto::Hash, uint32_t>> outputSpends;
      std::vector<std::pair<crypto::Hash, uint32_t>> depositSpends;
    };
    SpendDetectionResult detectSpendsInTransactions(const std::vector<cn::Transaction> &txs);

    // ── Getters ────────────────────────────────────────────────────────

    SyncStrategy activeStrategy() const { return m_strategy; }
    uint32_t lastScannedHeight() const { return m_scannedHeight; }
    const std::string &walletStateBinPath() const { return m_walletStateBinPath; }
    bool isActive() const { return m_active; }
    bool isStopping() const { return m_stop.load(); }

    void setScannedHeight(uint32_t height) { m_scannedHeight.store(height); }

  private:
    // ── Strategy detection ─────────────────────────────────────────────
    SyncStrategy detectBestStrategy();

    // ── Strategy implementations ───────────────────────────────────────
    void runDirectScan();
    void runPollingSync();

    // ── Output derivation ──────────────────────────────────────────────
    bool isOurOutput(const BoltCore::OutputInfo &candidate) const;
    crypto::KeyImage deriveKeyImage(const BoltCore::OutputInfo &output) const;
    void filterOwnedOutputs(std::vector<BoltCore::OutputInfo> &candidates);

    void markSpentOutputs(uint32_t topHeight);

    struct MsigOutputRef
    {
      crypto::Hash txHash;
      uint32_t outputIndex;
      uint32_t term;
    };

    void scanBlockTransaction(const cn::Transaction &tx,
                              uint32_t blockHeight,
                              const std::vector<uint32_t> *globalIndexes,
                              std::vector<BoltCore::OutputInfo> &outputs,
                              cn::BlockFilterRecord *filterRecord,
                              bool &haveFilter,
                              size_t &filterIdx,
                              const crypto::Hash *canonicalTxHash = nullptr);

    std::vector<BoltCore::OutputInfo> scanTransactionOutputs(const cn::Transaction &tx,
                                                             const crypto::Hash &txHash,
                                                             uint32_t blockHeight);

    bool tryResolveIncomingTx(const crypto::Hash &txHash,
                              std::vector<BoltCore::OutputInfo> &outputs,
                              uint64_t &totalAmount,
                              bool &confirmed,
                              cn::Transaction *rawTxOut = nullptr);

    // Push only outputs not already in cache to the wallet callback.
    void dispatchDiscoveredOutputs(const std::vector<BoltCore::OutputInfo> &outputs);

    // ── State ──────────────────────────────────────────────────────────
    std::string m_dataDir;
    std::string m_walletStateBinPath;
    crypto::SecretKey m_viewKey;
    crypto::PublicKey m_spendPub;
    const crypto::SecretKey *m_spendKey;

    cn::INode *m_node = nullptr;
    std::function<std::string(const std::string &, const std::string &)> m_daemonRpc;

    bool m_enableChainSync = true;
    SyncStrategy m_strategy = SyncStrategy::None;
    std::atomic<uint32_t> m_scannedHeight{0};
    std::atomic<uint32_t> m_daemonHeight{0};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stop{false};

    OutputCallback m_onOutputs;
    StatusCallback m_onStatus;
    SpentCallback m_onSpent;
    MetadataCallback m_onMetadata;
    std::thread m_thread;

    std::vector<crypto::PublicKey> m_knownPubKeys;
    std::mutex m_pubkeyMutex;
    std::mutex m_threadMutex;

    // Cached outputs for state file save/load
    std::vector<BoltCore::OutputInfo> m_cachedOutputs;
    mutable std::mutex m_outputCacheMutex;

    std::vector<crypto::Hash> m_knownPoolTxIds;
    mutable std::mutex m_poolMutex;

    uint32_t m_lastLoggedMdbxLag = 0;
    uint32_t m_lastSpentScanHeight = 0;
    uint32_t m_lastDaemonGapScanHeight = 0;
    std::unordered_map<uint64_t, std::vector<MsigOutputRef>> m_multisigIndex;

    // Tx ids not yet in the daemon chain (mempool); avoid repeated get_o_indexes RPC spam.
    mutable std::mutex m_backfillMutex;
    std::unordered_set<crypto::Hash, boost::hash<crypto::Hash>> m_unresolvedGlobalIndexTxs;

    // Incoming txs already applied to the wallet (same txHash — MDBX scan must skip).
    mutable std::mutex m_appliedTxMutex;
    std::unordered_set<crypto::Hash, boost::hash<crypto::Hash>> m_walletAppliedTxHashes;
  };

} // namespace ClientWallet