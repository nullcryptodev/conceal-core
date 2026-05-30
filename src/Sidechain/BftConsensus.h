// BftConsensus.h — block proposal, voting, and commit logic
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include "SidechainConfig.h"
#include "GossipManager.h"
#include <functional>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace Sidechain
{
  namespace BoltDex
  {
    class Engine;
  }
}

namespace Sidechain
{
  class SidechainStorage;

  class BftConsensus
  {
  public:
    using BlockCommittedCallback = std::function<void(const Block &)>;
    using BridgeBurnCallback = std::function<void(const Transaction &)>;

    // Callback for real-time push: height, txCount, voteCount
    using BlockEventCallback = std::function<void(uint64_t height, uint64_t txCount, size_t votes)>;

    BftConsensus(SidechainStorage &storage,
                 const ValidatorInfo &self,
                 GossipManager &gossip);

    // Propose a new block with the given transactions
    bool proposeBlock(const std::vector<Transaction> &transactions, Block &outBlock);

    // Register callback for committed blocks
    void onBlockCommitted(BlockCommittedCallback callback);

    // Register callback for SSE/WebSocket block push
    void onBlockEvent(BlockEventCallback callback) { m_blockEventCallback = std::move(callback); }

    // Handle incoming gossip message
    void handleMessage(const std::vector<uint8_t> &data, const std::string &fromIp);

    // Get current consensus height
    uint64_t getHeight() const { return m_currentHeight; }

    // Sync validator set from seed on first connect
    void syncValidators(const std::string &seedHost, uint16_t seedPort);
    bool handleSyncResponse(const std::vector<uint8_t> &data);
    void requestValidatorSync(const std::string &seedHost, uint16_t seedPort);
    void broadcastNewValidator(const ValidatorInfo &validator);

    // Reward key for block rewards
    void setRewardKey(const crypto::PublicKey &rewardKey);

    // DEX engine
    void setDexEngine(Sidechain::BoltDex::Engine *engine) { m_dexEngine = engine; }

    // Bridge
    void setBridgeKey(const crypto::PublicKey &bridgeKey)
    {
      m_bridgePubKey = bridgeKey;
      m_hasBridgeKey = true;
    }
    void onBridgeBurn(BridgeBurnCallback callback) { m_onBridgeBurn = callback; }

  private:
    void broadcastProposal(const Block &block);
    void broadcastVote(const crypto::Hash &blockHash, const crypto::Signature &signature);
    void checkCommitCondition(const crypto::Hash &blockHash);

    SidechainStorage &m_storage;
    ValidatorInfo m_self;
    GossipManager &m_gossip;
    BlockCommittedCallback m_onBlockCommitted;
    BridgeBurnCallback m_onBridgeBurn;
    BlockEventCallback m_blockEventCallback;

    std::atomic<uint64_t> m_currentHeight{0};
    bool m_validatorsSynced = false;
    Sidechain::BoltDex::Engine *m_dexEngine = nullptr;

    struct PendingBlock
    {
      Block block;
      std::vector<crypto::Signature> votes;
      bool committed = false;
    };

    std::unordered_map<crypto::Hash, PendingBlock> m_pendingBlocks;
    mutable std::mutex m_mutex;
    crypto::Hash m_previousBlockHash;

    crypto::PublicKey m_rewardKey;
    bool m_hasRewardKey = false;

    crypto::PublicKey m_bridgePubKey;
    bool m_hasBridgeKey = false;
  };
}