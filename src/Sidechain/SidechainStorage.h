// SidechainStorage.h — MDBX-backed block and state storage
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include "SidechainMdbxStorage.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Sidechain
{
  // ── Asset registry entry for provenance tracking ─────────────────────

  struct AssetRegistryEntry
  {
    uint64_t tokenId = 0;
    std::string fingerprint;          // hash(sourceChain:sourceAsset:bridgeOperatorPubKey)
    std::string sourceChain;          // "conceal", "polygon", "ethereum", "bitcoin"
    std::string sourceAsset;          // Contract address or "native"
    crypto::PublicKey bridgeOperator; // Public key of the bridge operator
    std::string equivalenceClass;     // sourceChain:sourceAsset (for DEX token equivalence)
    bool verified = false;

    void serialize(cn::ISerializer &s)
    {
      s(tokenId, "tokenId");
      s(fingerprint, "fingerprint");
      s(sourceChain, "sourceChain");
      s(sourceAsset, "sourceAsset");
      s(bridgeOperator, "bridgeOperator");
      s(equivalenceClass, "equivalenceClass");
      s(verified, "verified");
    }
  };

  // ── Bridge lock entry tracking main chain deposits ───────────────────

  struct BridgeLockEntry
  {
    uint64_t id = 0;
    crypto::PublicKey userAddress;
    uint64_t tokenId = 0;
    uint64_t amount = 0;
    crypto::Hash mainChainTxHash;
    uint64_t mainChainBlockHeight = 0;
    bool unlocked = false;

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      s(userAddress, "userAddress");
      s(tokenId, "tokenId");
      s(amount, "amount");
      s(mainChainTxHash, "mainChainTxHash");
      s(mainChainBlockHeight, "mainChainBlockHeight");
      s(unlocked, "unlocked");
    }
  };

  // ── Storage class ────────────────────────────────────────────────────

  class SidechainStorage
  {
  public:
    explicit SidechainStorage(const std::string &dataDir);
    ~SidechainStorage();

    // ── Block storage ────────────────────────────────────────────────

    bool addBlock(const Block &block, const crypto::Hash &hash);
    bool getBlock(uint64_t height, Block &block) const;
    bool getBlock(const crypto::Hash &hash, Block &block) const;
    uint64_t topBlockHeight() const;
    crypto::Hash getBlockHash(uint64_t height) const;

    // ── Fingerprint generation ───────────────────────────────────────

    std::string generateFingerprint(
        const std::string &sourceChain,
        const std::string &sourceAsset,
        const crypto::PublicKey &bridgeOperator) const;

    // ── Token operations ─────────────────────────────────────────────

    bool addToken(const TokenInfo &token);
    bool updateToken(const TokenInfo &token);
    bool getToken(uint64_t tokenId, TokenInfo &token) const;
    bool getTokenByFingerprint(const std::string &fingerprint, TokenInfo &token) const;
    std::vector<TokenInfo> getAllTokens() const;
    uint64_t nextTokenId() const;

    // ── Rate limiting ────────────────────────────────────────────────

    bool canCreateToken(const crypto::PublicKey &address, uint64_t currentHeight) const;
    void recordTokenCreation(const crypto::PublicKey &address, uint64_t currentHeight);

    // ── Asset registry ───────────────────────────────────────────────

    bool registerAsset(const std::string &sourceChain,
                       const std::string &sourceAsset,
                       const crypto::PublicKey &bridgeOperator,
                       uint64_t tokenId,
                       bool verified = false);
    bool getAssetBySource(const std::string &sourceChain,
                          const std::string &sourceAsset,
                          const crypto::PublicKey &bridgeOperator,
                          uint64_t &tokenId) const;
    bool getAssetByTokenId(uint64_t tokenId, AssetRegistryEntry &entry) const;
    std::vector<AssetRegistryEntry> getAllAssets() const;

    // ── Equivalence groups ───────────────────────────────────────────

    std::string getEquivalenceClass(const std::string &sourceChain,
                                    const std::string &sourceAsset) const;
    std::vector<uint64_t> getTokensByEquivalenceClass(const std::string &equivalenceClass) const;
    bool addToEquivalenceGroup(const std::string &equivalenceClass, uint64_t tokenId);

    // ── Bridge locks ─────────────────────────────────────────────────

    bool addBridgeLock(const crypto::PublicKey &userAddress,
                       uint64_t tokenId,
                       uint64_t amount,
                       const crypto::Hash &mainChainTxHash,
                       uint64_t mainChainBlockHeight);
    bool getBridgeLock(uint64_t lockId, BridgeLockEntry &lock) const;
    bool markBridgeLockUnlocked(uint64_t lockId);
    std::vector<BridgeLockEntry> getPendingUnlocks() const;
    uint64_t getTotalLockedForToken(uint64_t tokenId) const;
    uint64_t nextBridgeLockId() const;

    // ── Vesting ──────────────────────────────────────────────────────

    bool addVestingSchedule(const VestingSchedule &schedule);
    bool getVestingSchedule(uint64_t scheduleId, VestingSchedule &schedule) const;
    bool updateVestingSchedule(const VestingSchedule &schedule);
    std::vector<VestingSchedule> getActiveVestingSchedules() const;
    std::vector<VestingSchedule> getVestingSchedulesByBeneficiary(const crypto::PublicKey &beneficiary) const;
    uint64_t nextVestingScheduleId() const;

    // ── Reward pools ─────────────────────────────────────────────────

    bool addRewardPool(const RewardPool &pool);
    bool getRewardPool(uint64_t poolId, RewardPool &pool) const;
    bool updateRewardPool(const RewardPool &pool);
    std::vector<RewardPool> getActiveRewardPools() const;
    std::vector<RewardPool> getRewardPoolsByToken(uint64_t tokenId) const;
    uint64_t nextRewardPoolId() const;

    // ── Stakes ───────────────────────────────────────────────────────

    bool addStakeEntry(const StakeEntry &entry);
    bool getStakeEntry(uint64_t entryId, StakeEntry &entry) const;
    bool updateStakeEntry(const StakeEntry &entry);
    std::vector<StakeEntry> getStakesByOwner(const crypto::PublicKey &owner) const;
    std::vector<StakeEntry> getStakesByPool(uint64_t poolId) const;
    uint64_t nextStakeEntryId() const;

    // ── Per-block processing ─────────────────────────────────────────

    void processVestingReleases(uint64_t currentBlock);
    void processRewardAccrual(uint64_t currentBlock);

    // ── Account balances ─────────────────────────────────────────────

    bool getBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t &balance) const;
    bool setBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t balance);

    // ── Transaction execution ────────────────────────────────────────

    bool applyTransaction(const Transaction &tx);

    // ── Validators ───────────────────────────────────────────────────

    bool addValidator(const ValidatorInfo &validator);
    std::vector<ValidatorInfo> getActiveValidators() const;

    // ── Meta ─────────────────────────────────────────────────────────

    void flush();
    bool getMeta(const std::string &key, std::vector<uint8_t> &value) const;
    void putMeta(const std::string &key, const std::vector<uint8_t> &value);

  private:
    SidechainMdbxStorage m_storage;
    mutable std::recursive_mutex m_mutex;

    // In-memory hash → height index
    mutable std::unordered_map<crypto::Hash, uint64_t> m_hashToHeight;
  };

} // namespace Sidechain