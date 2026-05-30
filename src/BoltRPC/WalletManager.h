// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "SyncManager.h"
#include "StateManager.h"
#include "CryptoNoteConfig.h"

namespace cn
{
  class INode;
  class Currency;
}

namespace BoltRPC
{

  // ── Key storage (encrypted at rest) ────────────────────────────────────────

  struct WalletKeys
  {
    crypto::SecretKey viewSecretKey;
    crypto::SecretKey spendSecretKey;
    crypto::PublicKey spendPublicKey;
    std::string address; // Base58 encoded

    bool valid() const
    {
      return viewSecretKey != crypto::SecretKey() &&
             spendSecretKey != crypto::SecretKey();
    }
  };

  // ── Wallet status ──────────────────────────────────────────────────────────

  struct WalletStatus
  {
    bool locked = true;
    bool synced = false;
    uint32_t blockHeight = 0;
    uint64_t balance = 0;
    uint64_t unlockedBalance = 0;
    uint32_t outputCount = 0;
    uint32_t transactionCount = 0;
    SyncProgress syncProgress;
  };

  // ── Transaction building ───────────────────────────────────────────────────

  struct TransferRequest
  {
    std::string address;
    uint64_t amount;
    std::string paymentId;
    uint64_t mixin = cn::parameters::MINIMUM_MIXIN;
    uint64_t fee = 0;
  };

  struct TransferResult
  {
    bool success = false;
    std::string txHash;
    std::string errorMessage;
    uint64_t fee = 0;
  };

  struct DepositRequest
  {
    uint64_t amount;
    uint32_t term;
    uint64_t fee = 0;
  };

  struct WithdrawalRequest
  {
    uint64_t amount;
    uint64_t depositId;
    uint64_t fee = 0;
  };

  // ── WalletManager ──────────────────────────────────────────────────────────

  class WalletManager
  {
  public:
    using StatusCallback = std::function<void(const WalletStatus &)>;

    WalletManager(cn::INode &node,
                  const cn::Currency &currency,
                  const std::string &dataDir,
                  const std::string &daemonHost,
                  uint16_t daemonPort);
    ~WalletManager();

    // ── Key management ─────────────────────────────────────────────────────
    bool generateNewWallet(const std::string &password);
    bool importFromKeys(const std::string &viewKeyHex,
                        const std::string &spendKeyHex,
                        const std::string &password);
    bool importFromMnemonic(const std::string &mnemonic,
                            const std::string &password);
    bool unlock(const std::string &password);
    void lock();

    // ── Sync ────────────────────────────────────────────────────────────────
    void startSync(StatusCallback onStatus);
    void stopSync();
    void syncNow();

    // ── Wallet queries ──────────────────────────────────────────────────────
    WalletStatus getStatus() const;
    std::vector<OutputInfo> getOutputs(bool unspentOnly = true) const;
    std::vector<TransactionRecord> getTransactions(uint32_t offset = 0,
                                                   uint32_t limit = 50) const;
    uint64_t getBalance() const;
    uint64_t getUnlockedBalance() const;
    std::string getAddress() const;

    // ── Transactions ────────────────────────────────────────────────────────
    TransferResult sendTransfer(const TransferRequest &req);
    TransferResult sendDeposit(const DepositRequest &req);
    TransferResult sendWithdrawal(const WithdrawalRequest &req);

    // ── Export ──────────────────────────────────────────────────────────────
    std::string exportKeys() const;
    std::string exportMnemonic() const;
    std::string exportState() const;

    // ── State file ──────────────────────────────────────────────────────────
    bool hasExistingWallet() const;

    void setDaemonRpcCallback(DaemonRpcCallback callback) { m_rpcCallback = std::move(callback); }

    void setActiveWallet(const std::string &walletName);
    std::vector<std::string> listWalletFiles() const;

  private:
    // ── Internal helpers ────────────────────────────────────────────────────
    void onSyncProgress(const SyncProgress &progress);
    void onNewOutputs(const std::vector<OutputInfo> &newOutputs,
                      const std::vector<crypto::KeyImage> &spentKeyImages);

    bool encryptKeys(const WalletKeys &keys, const std::string &password);
    bool decryptKeys(const std::string &password, WalletKeys &keys) const;

    void updateStatus();
    void loadState();
    void saveState();

    std::string keysFilePath() const;
    std::string stateFilePath() const;

    // ── Members ────────────────────────────────────────────────────────────
    cn::INode &m_node;
    const cn::Currency &m_currency;
    std::string m_dataDir;
    DaemonRpcCallback m_rpcCallback;

    std::string m_daemonHost;
    uint16_t m_daemonPort;

    std::string m_activeWallet;

    WalletKeys m_keys;
    WalletState m_state;
    std::atomic<bool> m_locked{true};

    std::unique_ptr<SyncManager> m_syncManager;
    std::unique_ptr<StateManager> m_stateManager;
    StatusCallback m_onStatus;
    mutable std::mutex m_stateMutex;
  };

} // namespace BoltRPC