// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <mdbx.h>

#include "Common/JsonValue.h"
#include "WalletManager.h" // For WalletKeys, WalletStatus, TransferRequest, etc.

// Forward declare MDBX types we use internally
// (Actual mdbx.hpp include goes in the .cpp file)
namespace mdbx
{
  class env_managed;
  class map_handle;
}

namespace cn
{
  class INode;
  class Currency;
}

namespace BoltRPC
{

  // Per-wallet metadata stored in wallets.db
  struct WalletEntry
  {
    std::string id;            // User-assigned wallet identifier
    std::string address;       // Base58 public address
    std::string stateFilePath; // Path to the encrypted .state file
    uint64_t createdAt;        // Unix timestamp
    uint64_t lastUsed;         // Unix timestamp
  };

  // MultiWalletManager
  //
  // Manages multiple independent wallets in a single process.
  // Only ONE wallet's outputs are held in memory at a time (flat memory).
  //
  // Directory structure:
  //   walletsDir/
  //   ├── wallets.db          # MDBX index of all wallets
  //   ├── <wallet_id>.state   # Encrypted state file (same format as single-wallet)
  //   └── <wallet_id>.state   # ...

  class MultiWalletManager
  {
  public:
    using StatusCallback = std::function<void(const WalletStatus &)>;

    // Construction
    MultiWalletManager(const std::string &walletsDir,
                       cn::INode *node,
                       const cn::Currency &currency);
    ~MultiWalletManager();

    // Non-copyable, non-movable (owns MDBX environment)
    MultiWalletManager(const MultiWalletManager &) = delete;
    MultiWalletManager &operator=(const MultiWalletManager &) = delete;

    // Wallet lifecycle

    /// Create a new wallet with a fresh keypair, encrypted with `password`.
    /// Returns {wallet_id, address, stateFilePath}
    common::JsonValue createWallet(const std::string &walletId,
                                   const std::string &password);

    /// Import a wallet from spend/view secret keys.
    common::JsonValue importWallet(const std::string &walletId,
                                   const std::string &viewKeyHex,
                                   const std::string &spendKeyHex,
                                   const std::string &password);

    /// Import a wallet from a backed-up .state file.
    /// Copies the file into walletsDir and registers it.
    common::JsonValue importFromStateFile(const std::string &walletId,
                                          const std::string &sourceFilePath,
                                          const std::string &password);

    /// Load (decrypt) a wallet into memory. Saves & unloads any currently
    /// loaded wallet first. Starts background sync if node is connected.
    common::JsonValue loadWallet(const std::string &walletId,
                                 const std::string &password);

    /// Unload the active wallet: encrypt & save state, free memory.
    common::JsonValue unloadWallet();

    /// Delete a wallet permanently. Must be unloaded first.
    common::JsonValue deleteWallet(const std::string &walletId,
                                   const std::string &password);

    /// List all registered wallets (no passwords needed — metadata only).
    common::JsonValue listWallets() const;

    // Wallet operations (delegated to the currently loaded wallet)
    common::JsonValue getStatus() const;
    common::JsonValue getBalance() const;
    common::JsonValue getAddress() const;
    common::JsonValue getOutputs(const common::JsonValue &params) const;
    common::JsonValue getTransactions(const common::JsonValue &params) const;

    common::JsonValue sendTransfer(const common::JsonValue &params);
    common::JsonValue sendDeposit(const common::JsonValue &params);
    common::JsonValue sendWithdrawal(const common::JsonValue &params);

    // Export (requires active wallet)

    /// Export the active wallet's secret keys. Returns {viewKey, spendKey, mnemonic}.
    common::JsonValue exportKeys() const;

    /// Export the active wallet's state file path for backup.
    common::JsonValue exportState() const;

    /// Change password for the active wallet. Re-encrypts the state file.
    common::JsonValue changePassword(const std::string &oldPassword,
                                     const std::string &newPassword);

    // Queries

    /// Check whether any wallet is currently loaded
    bool hasActiveWallet() const;

    /// Get the ID of the currently loaded wallet (empty if none)
    std::string activeWalletId() const;

    /// Check whether a wallet with the given ID exists in the index
    bool walletExists(const std::string &walletId) const;

  private:
    // Internal helpers

    /// Open (or create) the wallets.db MDBX index
    void openIndex();

    /// Insert or update a wallet entry in the index
    void saveIndexEntry(const WalletEntry &entry);

    /// Remove a wallet from the index
    void removeIndexEntry(const std::string &walletId);

    /// Read all entries from the index
    std::vector<WalletEntry> readAllIndexEntries() const;

    /// Read a single entry by wallet ID. Throws if not found.
    WalletEntry getIndexEntry(const std::string &walletId) const;

    /// Generate the state file path for a given wallet ID
    std::string stateFilePath(const std::string &walletId) const;

    /// Save the currently loaded wallet's state to disk
    void saveActiveWallet();

    /// Load a wallet's state from disk into memory (decrypt)
    bool loadWalletState(const std::string &walletId,
                         const std::string &password,
                         WalletKeys &keys,
                         WalletState &state);

    /// Start background sync for the active wallet
    void startSyncForActive();

    /// Stop any ongoing sync
    void stopActiveSync();

    // Members
    std::string m_walletsDir;
    cn::INode *m_node; // May be nullptr (offline mode)
    const cn::Currency &m_currency;

    // wallets.db index
    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiWallets = 0;

    // Currently loaded wallet (only one at a time)
    std::string m_activeWalletId;
    WalletKeys m_activeKeys;
    WalletState m_activeState;
    std::atomic<bool> m_locked{true}; // true = no wallet loaded

    // Cached password hash for re-saving state without re-prompting
    crypto::Hash m_passwordHash;

    // Sync
    std::unique_ptr<SyncManager> m_syncManager;
    StatusCallback m_onStatus;

    // Thread safety
    mutable std::mutex m_mutex;
  };

} // namespace BoltRPC