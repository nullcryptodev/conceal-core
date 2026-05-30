// SidechainValidator.h — block production and transaction validation with BFT
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include "SidechainStorage.h"
#include "SidechainConfig.h"
#include "GossipManager.h"
#include "BftConsensus.h"
#include <memory>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>

namespace Sidechain
{
  namespace BoltDex
  {
    class Engine;
  }
  namespace BoltAMM
  {
    class Engine;
  }
}

namespace Sidechain
{
  class SidechainValidator
  {
  public:
    SidechainValidator(SidechainStorage &storage,
                       const ValidatorInfo &self,
                       const std::vector<ValidatorInfo> &validators,
                       GossipManager &gossip,
                       bool testnet = false);
    ~SidechainValidator();

    void start();
    void stop();

    bool submitTransaction(const Transaction &tx);
    std::vector<Transaction> getPendingTransactions() const;
    std::vector<ValidatorInfo> getValidators() const;

    void setRewardKey(const crypto::PublicKey &key) { m_consensus.setRewardKey(key); }
    void setDexEngine(Sidechain::BoltDex::Engine *engine) { m_consensus.setDexEngine(engine); }
    void setAmmEngine(Sidechain::BoltAMM::Engine *engine) { m_ammEngine = engine; }
    void setBridgeKey(const crypto::PublicKey &key) { m_consensus.setBridgeKey(key); }
    void onBridgeBurn(std::function<void(const Transaction &)> cb) { m_consensus.onBridgeBurn(cb); }

    void onSseBlock(std::function<void(uint64_t, uint64_t, size_t)> cb) { m_consensus.onBlockEvent(std::move(cb)); }

  private:
    void consensusLoop();
    bool validateTransaction(const Transaction &tx) const;
    bool validateTokenBacking(const Transaction &tx, const TokenInfo &token) const;

    SidechainStorage &m_storage;
    ValidatorInfo m_self;
    std::vector<ValidatorInfo> m_validators;
    GossipManager &m_gossip;
    BftConsensus m_consensus;
    Sidechain::BoltAMM::Engine *m_ammEngine = nullptr;

    std::queue<Transaction> m_pendingTransactions;
    mutable std::mutex m_txMutex;
    mutable std::mutex m_validatorMutex;

    std::thread m_consensusThread;
    std::atomic<bool> m_running{false};
    bool m_testnet = false;
  };
}