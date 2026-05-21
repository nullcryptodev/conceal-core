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

#include "crypto/crypto.h"

#include "CryptoNoteCore/CryptoNoteBasic.h"

#include "Common/StringTools.h"

// Forward declarations
namespace CryptoNote
{
  class MDBXBlockchainStorage;
}

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

  struct SyncProgress
  {
    enum Phase
    {
      IDLE,
      FETCHING_KEYS,    // Downloading tx_pub_keys from daemon
      FETCHING_OUTPUTS, // Downloading candidate outputs
      DERIVING,         // Locally deriving ownership
      INCREMENTAL,      // Normal background polling
      COMPLETE
    };

    Phase phase = IDLE;
    uint32_t totalKeys = 0;
    uint32_t processedKeys = 0;
    uint32_t totalOutputs = 0;
    uint32_t processedOutputs = 0;
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
    bool isFirstSync() const { return m_knownTxPubKeys.empty(); }
    const std::vector<crypto::PublicKey> &knownTxPubKeys() const { return m_knownTxPubKeys; }

  private:
    // ── Internal sync methods ──────────────────────────────────────────────
    void runLoop();
    void doBootstrap(SyncProgress &progress);
    void doIncrementalSync(SyncProgress &progress);

    // ── Daemon communication ───────────────────────────────────────────────
    bool callGetWalletSnapshot(
        const std::vector<crypto::PublicKey> &txPubKeys,
        uint32_t walletHeight,
        std::vector<OutputInfo> &outputs,
        std::unordered_set<std::string> &spentKeyImages,
        std::vector<crypto::PublicKey> &newTxPubKeys,
        uint32_t &currentHeight,
        std::string *errorOut = nullptr,
        SyncProgress *progressOut = nullptr);

    // Query outputs/spent images in batches (avoids multi‑hundred‑MB get_wallet_snapshot requests).
    bool fetchOutputsForKeys(
        const std::vector<crypto::PublicKey> &keys,
        uint32_t walletHeight,
        std::vector<OutputInfo> &outputs,
        std::unordered_set<std::string> &spentKeyImages,
        std::vector<crypto::PublicKey> &additionalKeys,
        uint32_t &currentHeight,
        SyncProgress *progress,
        std::string *errorOut = nullptr);

    // ── Local derivation ───────────────────────────────────────────────────
    void deriveOwnedOutputs(
        const std::vector<OutputInfo> &candidates,
        const std::unordered_set<std::string> &spentKeyImages,
        std::vector<OutputInfo> &owned);

    bool isOurOutput(const OutputInfo &candidate) const;
    crypto::KeyImage deriveKeyImage(const OutputInfo &output) const;

    // ── Persistence ────────────────────────────────────────────────────────
    void loadCachedKeys();
    void saveCachedKeys();
    std::string keysCachePath() const;
    void addKnownKey(const crypto::PublicKey &txPubKey);

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

    std::vector<crypto::PublicKey> m_knownTxPubKeys;
    mutable std::mutex m_keysMutex;

    ProgressCallback m_onProgress;
    OutputCallback m_onOutputs;
    std::thread m_thread;

    static constexpr uint32_t POLL_INTERVAL_SECONDS = 30;
    static constexpr size_t MAX_OUTPUTS_PER_CALL = 50000;
    static constexpr size_t WALLET_SNAPSHOT_KEY_BATCH_SIZE = 5000;
  };

} // namespace BoltRPC