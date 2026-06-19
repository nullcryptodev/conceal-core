// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "SyncManager.h"

#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace cn
{
  class INode;
}

namespace BoltRPC
{

  // ── Data structures ────────────────────────────────────────────────────────

  struct TransactionRecord
  {
    crypto::Hash txHash;
    uint32_t blockHeight;
    uint64_t timestamp;
    uint64_t fee;
    uint64_t totalSent;
    uint64_t totalReceived;
    std::vector<crypto::PublicKey> extraKeys; // tx_pub_keys involved
    std::string paymentId;

    enum Type
    {
      INCOMING,
      OUTGOING,
      SELF,
      DEPOSIT,
      WITHDRAWAL
    };
    Type type = INCOMING;
    bool confirmed = false;
  };

  struct WalletState
  {
    uint32_t lastHeight = 0;
    uint64_t balance = 0;
    uint64_t unlockedBalance = 0;
    std::vector<OutputInfo> ownedOutputs;
    std::vector<crypto::KeyImage> spentKeyImages;
    std::vector<TransactionRecord> transactions;
  };

  // ── StateManager ────────────────────────────────────────────────────────────

  class StateManager
  {
  public:
    // dataDir/wallet_state.bin
    explicit StateManager(const std::string &dataDir);
    // exact file path (e.g. from --state)
    StateManager(const std::string &filePath, bool pathIsFullFile);
    ~StateManager();

    // ── Load / Save ────────────────────────────────────────────────────────
    bool load(WalletState &state);
    bool save(const WalletState &state);

    // ── Incremental updates ────────────────────────────────────────────────
    void addOutput(const OutputInfo &output);
    void markSpent(const crypto::KeyImage &keyImage);
    void addTransaction(const TransactionRecord &tx);
    void setHeight(uint32_t height);
    void setBalance(uint64_t balance, uint64_t unlocked);

    // ── Atomic batch update ────────────────────────────────────────────────
    void commit(const WalletState &state);

    // ── File info ──────────────────────────────────────────────────────────
    std::string filePath() const;
    bool exists() const;
    size_t fileSize() const;

  private:
    std::string m_filePath;
    mutable std::mutex m_mutex;
  };

} // namespace BoltRPC