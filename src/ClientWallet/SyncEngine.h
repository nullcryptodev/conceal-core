// SyncEngine - multi-strategy wallet sync for ClientWallet
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

    SyncEngine(const std::string &dataDir,
               const crypto::SecretKey &viewKey,
               const crypto::PublicKey &spendPub,
               const crypto::SecretKey *spendKey);
    ~SyncEngine();

    // ── Lifecycle ──────────────────────────────────────────────────────

    void setNode(cn::INode *node);
    void setDaemonRpc(std::function<std::string(const std::string &method, const std::string &params)> rpc);

    void start(OutputCallback onOutputs, StatusCallback onStatus);
    void stop();

    // ── Manual triggers ────────────────────────────────────────────────

    void fullSync();
    void incrementalSync();
    void onNewBlockHeight(uint32_t newHeight);

    // ── State file ─────────────────────────────────────────────────────

    bool loadStateFile(const std::string &path);
    bool saveStateFile(const std::string &path);
    std::vector<BoltCore::OutputInfo> getCachedOutputs() const;

    // ── Getters ────────────────────────────────────────────────────────

    SyncStrategy activeStrategy() const { return m_strategy; }
    uint32_t lastScannedHeight() const { return m_scannedHeight; }
    bool isActive() const { return m_active; }

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

    // ── State ──────────────────────────────────────────────────────────
    std::string m_dataDir;
    crypto::SecretKey m_viewKey;
    crypto::PublicKey m_spendPub;
    const crypto::SecretKey *m_spendKey;

    cn::INode *m_node = nullptr;
    std::function<std::string(const std::string &, const std::string &)> m_daemonRpc;

    SyncStrategy m_strategy = SyncStrategy::None;
    std::atomic<uint32_t> m_scannedHeight{0};
    std::atomic<bool> m_active{false};
    std::atomic<bool> m_stop{false};

    OutputCallback m_onOutputs;
    StatusCallback m_onStatus;
    std::thread m_thread;

    std::vector<crypto::PublicKey> m_knownPubKeys;
    std::mutex m_pubkeyMutex;

    // Cached outputs for state file save/load
    std::vector<BoltCore::OutputInfo> m_cachedOutputs;
    mutable std::mutex m_outputCacheMutex;
  };

} // namespace ClientWallet