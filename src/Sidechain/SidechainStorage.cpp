// SidechainStorage implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainStorage.h"
#include "SidechainConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Serialization/SerializationTools.h"
#include <boost/filesystem.hpp>
#include <iostream>

namespace Sidechain
{
  namespace
  {
    std::string ensureDataDir(const std::string &dataDir)
    {
      std::string dbPath = dataDir + "/" + SidechainConfig::DATABASE_NAME;
      boost::filesystem::create_directories(dbPath);
      return dbPath;
    }
  }

  SidechainStorage::SidechainStorage(const std::string &dataDir)
      : m_storage(ensureDataDir(dataDir))
  {
    if (topBlockHeight() == 0)
    {
      Block genesis;
      genesis.header.height = 0;
      genesis.header.timestamp = SidechainConfig::GENESIS_TIMESTAMP;
      crypto::Hash hash = cn::getObjectHash(genesis);
      genesis.header.blockHash = hash;
      addBlock(genesis, hash);
    }
  }

  SidechainStorage::~SidechainStorage()
  {
    flush();
  }

  // ── Block storage ─────────────────────────────────────────────────────

  bool SidechainStorage::addBlock(const Block &block, const crypto::Hash &hash)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(block);
    uint64_t height = block.header.height;
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    m_storage.pushBlockEntry(static_cast<uint32_t>(height), vec);
    m_hashToHeight[hash] = height;
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getBlock(uint64_t height, Block &block) const
  {
    std::vector<uint8_t> vec;
    if (!m_storage.getBlockEntry(static_cast<uint32_t>(height), vec))
      return false;
    cn::BinaryArray ba(vec.begin(), vec.end());
    return cn::fromBinaryArray(block, ba);
  }

  bool SidechainStorage::getBlock(const crypto::Hash &hash, Block &block) const
  {
    auto it = m_hashToHeight.find(hash);
    if (it == m_hashToHeight.end())
      return false;
    return getBlock(it->second, block);
  }

  uint64_t SidechainStorage::topBlockHeight() const
  {
    return m_storage.topBlockHeight();
  }

  crypto::Hash SidechainStorage::getBlockHash(uint64_t height) const
  {
    Block block;
    if (!getBlock(height, block))
      return crypto::Hash{};
    return block.header.blockHash;
  }

  // ── Fingerprint generation ────────────────────────────────────────────
  std::string SidechainStorage::generateFingerprint(
      const std::string &sourceChain,
      const std::string &sourceAsset,
      const crypto::PublicKey &bridgeOperator) const
  {
    std::string input = sourceChain + ":" + sourceAsset + ":" + common::podToHex(bridgeOperator);
    crypto::Hash hash;
    crypto::cn_fast_hash(input.data(), input.size(), hash);
    return common::podToHex(hash);
  }

  // ── Internal KV helpers ───────────────────────────────────────────────

  bool SidechainStorage::getMeta(const std::string &key, std::vector<uint8_t> &value) const
  {
    return m_storage.get(key, value);
  }

  void SidechainStorage::putMeta(const std::string &key, const std::vector<uint8_t> &value)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_storage.put(key, value);
    m_storage.flush();
  }

  // ── Token operations ──────────────────────────────────────────────────

  bool SidechainStorage::addToken(const TokenInfo &token)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(token);
    std::string key = "token_" + std::to_string(token.id);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::cout << "addToken: key=" << key << " id=" << token.id
              << " name=" << token.name << " symbol=" << token.symbol
              << " fingerprint=" << token.fingerprint
              << " model=" << static_cast<int>(token.backingModel)
              << " lockedCCX=" << token.lockedCCXAmount
              << " size=" << vec.size() << std::endl;
    m_storage.put(key, vec);

    // Index by fingerprint if set
    if (!token.fingerprint.empty())
    {
      std::string fpKey = "token_fp_" + token.fingerprint;
      std::vector<uint8_t> fpValue(sizeof(uint64_t));
      uint64_t id = token.id;
      memcpy(fpValue.data(), &id, sizeof(uint64_t));
      m_storage.put(fpKey, fpValue);
    }

    m_storage.flush();
    return true;
  }

  bool SidechainStorage::updateToken(const TokenInfo &token)
  {
    return addToken(token);
  }

  bool SidechainStorage::getToken(uint64_t tokenId, TokenInfo &token) const
  {
    std::string key = "token_" + std::to_string(tokenId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return false;
    cn::BinaryArray ba(value.begin(), value.end());
    return cn::fromBinaryArray(token, ba);
  }

  bool SidechainStorage::getTokenByFingerprint(const std::string &fingerprint, TokenInfo &token) const
  {
    std::string fpKey = "token_fp_" + fingerprint;
    std::vector<uint8_t> value;
    if (!m_storage.get(fpKey, value) || value.size() < sizeof(uint64_t))
      return false;
    uint64_t tokenId;
    memcpy(&tokenId, value.data(), sizeof(uint64_t));
    return getToken(tokenId, token);
  }

  std::vector<TokenInfo> SidechainStorage::getAllTokens() const
  {
    std::vector<TokenInfo> result;
    uint64_t id = 1;
    TokenInfo token;
    while (getToken(id, token))
    {
      result.push_back(token);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::nextTokenId() const
  {
    uint64_t id = 1;
    TokenInfo token;
    while (getToken(id, token))
      ++id;
    return id;
  }

  // ── Rate limiting ─────────────────────────────────────────────────────

  bool SidechainStorage::canCreateToken(const crypto::PublicKey &address, uint64_t currentHeight) const
  {
    std::string key = "create_limit_" + common::podToHex(address);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value) || value.size() < sizeof(uint64_t))
      return true;

    uint64_t lastCreateHeight;
    memcpy(&lastCreateHeight, value.data(), sizeof(uint64_t));

    return (currentHeight - lastCreateHeight) >= SidechainConfig::TOKEN_CREATE_COOLDOWN_BLOCKS;
  }

  void SidechainStorage::recordTokenCreation(const crypto::PublicKey &address, uint64_t currentHeight)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string key = "create_limit_" + common::podToHex(address);
    std::vector<uint8_t> value(sizeof(uint64_t));
    memcpy(value.data(), &currentHeight, sizeof(uint64_t));
    m_storage.put(key, value);
    m_storage.flush();
  }

  // ── Asset registry operations ─────────────────────────────────────────

  bool SidechainStorage::registerAsset(const std::string &sourceChain,
                                       const std::string &sourceAsset,
                                       const crypto::PublicKey &bridgeOperator,
                                       uint64_t tokenId,
                                       bool verified)
  {
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    std::string fingerprint = generateFingerprint(sourceChain, sourceAsset, bridgeOperator);
    std::string equivalenceClass = sourceChain + ":" + sourceAsset;

    // Register by compound key
    std::string sourceKey = "asset_src_" + sourceChain + "_" + sourceAsset + "_" + common::podToHex(bridgeOperator);
    std::vector<uint8_t> sourceValue(sizeof(uint64_t));
    memcpy(sourceValue.data(), &tokenId, sizeof(uint64_t));
    m_storage.put(sourceKey, sourceValue);

    // Register by token ID
    std::string tokenKey = "asset_tok_" + std::to_string(tokenId);
    AssetRegistryEntry entry;
    entry.tokenId = tokenId;
    entry.fingerprint = fingerprint;
    entry.sourceChain = sourceChain;
    entry.sourceAsset = sourceAsset;
    entry.bridgeOperator = bridgeOperator;
    entry.equivalenceClass = equivalenceClass;
    entry.verified = verified;
    cn::BinaryArray ba = cn::toBinaryArray(entry);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    m_storage.put(tokenKey, vec);

    // Update the token's fingerprint
    TokenInfo token;
    if (getToken(tokenId, token))
    {
      token.fingerprint = fingerprint;
      updateToken(token);
    }

    // Add to equivalence group
    addToEquivalenceGroup(equivalenceClass, tokenId);

    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getAssetBySource(const std::string &sourceChain,
                                          const std::string &sourceAsset,
                                          const crypto::PublicKey &bridgeOperator,
                                          uint64_t &tokenId) const
  {
    std::string key = "asset_src_" + sourceChain + "_" + sourceAsset + "_" + common::podToHex(bridgeOperator);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value) || value.size() < sizeof(uint64_t))
      return false;
    memcpy(&tokenId, value.data(), sizeof(uint64_t));
    return true;
  }

  bool SidechainStorage::getAssetByTokenId(uint64_t tokenId,
                                           AssetRegistryEntry &entry) const
  {
    std::string key = "asset_tok_" + std::to_string(tokenId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return false;
    cn::BinaryArray ba(value.begin(), value.end());
    return cn::fromBinaryArray(entry, ba);
  }

  std::vector<AssetRegistryEntry> SidechainStorage::getAllAssets() const
  {
    std::vector<AssetRegistryEntry> result;
    std::vector<TokenInfo> tokens = getAllTokens();
    for (const auto &token : tokens)
    {
      AssetRegistryEntry entry;
      if (getAssetByTokenId(token.id, entry))
        result.push_back(entry);
    }
    return result;
  }

  // ── Equivalence group operations ──────────────────────────────────────

  std::string SidechainStorage::getEquivalenceClass(const std::string &sourceChain,
                                                    const std::string &sourceAsset) const
  {
    return sourceChain + ":" + sourceAsset;
  }

  std::vector<uint64_t> SidechainStorage::getTokensByEquivalenceClass(const std::string &equivalenceClass) const
  {
    std::vector<uint64_t> result;
    std::string key = "equiv_" + equivalenceClass;
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return result;

    if (value.size() < sizeof(uint32_t))
      return result;

    uint32_t count;
    memcpy(&count, value.data(), sizeof(uint32_t));
    const uint8_t *ptr = value.data() + sizeof(uint32_t);

    for (uint32_t i = 0; i < count && (ptr + sizeof(uint64_t)) <= (value.data() + value.size()); ++i)
    {
      uint64_t tokenId;
      memcpy(&tokenId, ptr, sizeof(uint64_t));
      ptr += sizeof(uint64_t);
      result.push_back(tokenId);
    }
    return result;
  }

  bool SidechainStorage::addToEquivalenceGroup(const std::string &equivalenceClass, uint64_t tokenId)
  {
    std::lock_guard<std::recursive_mutex> guard(m_mutex);
    std::string key = "equiv_" + equivalenceClass;

    auto existing = getTokensByEquivalenceClass(equivalenceClass);

    for (uint64_t existingId : existing)
    {
      if (existingId == tokenId)
        return true;
    }

    existing.push_back(tokenId);

    std::vector<uint8_t> value;
    uint32_t count = static_cast<uint32_t>(existing.size());
    value.insert(value.end(), (uint8_t *)&count, (uint8_t *)&count + sizeof(count));
    for (uint64_t id : existing)
    {
      value.insert(value.end(), (uint8_t *)&id, (uint8_t *)&id + sizeof(id));
    }

    m_storage.put(key, value);
    m_storage.flush();
    return true;
  }

  // ── Bridge operations ─────────────────────────────────────────────────

  bool SidechainStorage::addBridgeLock(const crypto::PublicKey &userAddress,
                                       uint64_t tokenId,
                                       uint64_t amount,
                                       const crypto::Hash &mainChainTxHash,
                                       uint64_t mainChainBlockHeight)
  {
    std::lock_guard<std::recursive_mutex> guard(m_mutex);

    BridgeLockEntry entry;
    entry.id = nextBridgeLockId();
    entry.userAddress = userAddress;
    entry.tokenId = tokenId;
    entry.amount = amount;
    entry.mainChainTxHash = mainChainTxHash;
    entry.mainChainBlockHeight = mainChainBlockHeight;
    entry.unlocked = false;

    cn::BinaryArray ba = cn::toBinaryArray(entry);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::string key = "bridge_lock_" + std::to_string(entry.id);
    m_storage.put(key, vec);
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getBridgeLock(uint64_t lockId, BridgeLockEntry &lock) const
  {
    std::string key = "bridge_lock_" + std::to_string(lockId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return false;
    cn::BinaryArray ba(value.begin(), value.end());
    return cn::fromBinaryArray(lock, ba);
  }

  bool SidechainStorage::markBridgeLockUnlocked(uint64_t lockId)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    BridgeLockEntry entry;
    if (!getBridgeLock(lockId, entry))
      return false;
    entry.unlocked = true;
    cn::BinaryArray ba = cn::toBinaryArray(entry);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::string key = "bridge_lock_" + std::to_string(lockId);
    m_storage.put(key, vec);
    m_storage.flush();
    return true;
  }

  std::vector<BridgeLockEntry> SidechainStorage::getPendingUnlocks() const
  {
    std::vector<BridgeLockEntry> result;
    uint64_t id = 1;
    while (true)
    {
      BridgeLockEntry entry;
      if (!getBridgeLock(id, entry))
        break;
      if (!entry.unlocked)
        result.push_back(entry);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::getTotalLockedForToken(uint64_t tokenId) const
  {
    uint64_t total = 0;
    uint64_t id = 1;
    while (true)
    {
      BridgeLockEntry entry;
      if (!getBridgeLock(id, entry))
        break;
      if (!entry.unlocked && entry.tokenId == tokenId)
        total += entry.amount;
      ++id;
    }
    return total;
  }

  uint64_t SidechainStorage::nextBridgeLockId() const
  {
    uint64_t id = 1;
    BridgeLockEntry entry;
    while (getBridgeLock(id, entry))
      ++id;
    return id;
  }

  // ── Vesting operations ────────────────────────────────────────────────

  bool SidechainStorage::addVestingSchedule(const VestingSchedule &schedule)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(schedule);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::string key = "vesting_" + std::to_string(schedule.scheduleId);
    m_storage.put(key, vec);
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getVestingSchedule(uint64_t scheduleId, VestingSchedule &schedule) const
  {
    std::string key = "vesting_" + std::to_string(scheduleId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return false;
    cn::BinaryArray ba(value.begin(), value.end());
    return cn::fromBinaryArray(schedule, ba);
  }

  bool SidechainStorage::updateVestingSchedule(const VestingSchedule &schedule)
  {
    return addVestingSchedule(schedule);
  }

  std::vector<VestingSchedule> SidechainStorage::getActiveVestingSchedules() const
  {
    std::vector<VestingSchedule> result;
    uint64_t id = 1;
    VestingSchedule schedule;
    while (getVestingSchedule(id, schedule))
    {
      if (schedule.status == VestingStatus::Active)
        result.push_back(schedule);
      ++id;
    }
    return result;
  }

  std::vector<VestingSchedule> SidechainStorage::getVestingSchedulesByBeneficiary(const crypto::PublicKey &beneficiary) const
  {
    std::vector<VestingSchedule> result;
    uint64_t id = 1;
    VestingSchedule schedule;
    while (getVestingSchedule(id, schedule))
    {
      if (schedule.beneficiary == beneficiary)
        result.push_back(schedule);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::nextVestingScheduleId() const
  {
    uint64_t id = 1;
    VestingSchedule schedule;
    while (getVestingSchedule(id, schedule))
      ++id;
    return id;
  }

  // ── Reward pool operations ────────────────────────────────────────────

  bool SidechainStorage::addRewardPool(const RewardPool &pool)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(pool);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::string key = "reward_pool_" + std::to_string(pool.poolId);
    m_storage.put(key, vec);
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getRewardPool(uint64_t poolId, RewardPool &pool) const
  {
    std::string key = "reward_pool_" + std::to_string(poolId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return false;
    cn::BinaryArray ba(value.begin(), value.end());
    return cn::fromBinaryArray(pool, ba);
  }

  bool SidechainStorage::updateRewardPool(const RewardPool &pool)
  {
    return addRewardPool(pool);
  }

  std::vector<RewardPool> SidechainStorage::getActiveRewardPools() const
  {
    std::vector<RewardPool> result;
    uint64_t id = 1;
    RewardPool pool;
    while (getRewardPool(id, pool))
    {
      if (pool.active)
        result.push_back(pool);
      ++id;
    }
    return result;
  }

  std::vector<RewardPool> SidechainStorage::getRewardPoolsByToken(uint64_t tokenId) const
  {
    std::vector<RewardPool> result;
    uint64_t id = 1;
    RewardPool pool;
    while (getRewardPool(id, pool))
    {
      if (pool.tokenId == tokenId)
        result.push_back(pool);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::nextRewardPoolId() const
  {
    uint64_t id = 1;
    RewardPool pool;
    while (getRewardPool(id, pool))
      ++id;
    return id;
  }

  // ── Stake operations ──────────────────────────────────────────────────

  bool SidechainStorage::addStakeEntry(const StakeEntry &entry)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(entry);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::string key = "stake_" + std::to_string(entry.entryId);
    m_storage.put(key, vec);
    m_storage.flush();
    return true;
  }

  bool SidechainStorage::getStakeEntry(uint64_t entryId, StakeEntry &entry) const
  {
    std::string key = "stake_" + std::to_string(entryId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value))
      return false;
    cn::BinaryArray ba(value.begin(), value.end());
    return cn::fromBinaryArray(entry, ba);
  }

  bool SidechainStorage::updateStakeEntry(const StakeEntry &entry)
  {
    return addStakeEntry(entry);
  }

  std::vector<StakeEntry> SidechainStorage::getStakesByOwner(const crypto::PublicKey &owner) const
  {
    std::vector<StakeEntry> result;
    uint64_t id = 1;
    StakeEntry entry;
    while (getStakeEntry(id, entry))
    {
      if (entry.owner == owner)
        result.push_back(entry);
      ++id;
    }
    return result;
  }

  std::vector<StakeEntry> SidechainStorage::getStakesByPool(uint64_t poolId) const
  {
    std::vector<StakeEntry> result;
    uint64_t id = 1;
    StakeEntry entry;
    while (getStakeEntry(id, entry))
    {
      if (entry.poolId == poolId)
        result.push_back(entry);
      ++id;
    }
    return result;
  }

  uint64_t SidechainStorage::nextStakeEntryId() const
  {
    uint64_t id = 1;
    StakeEntry entry;
    while (getStakeEntry(id, entry))
      ++id;
    return id;
  }

  // ── Per-block processing ──────────────────────────────────────────────

  void SidechainStorage::processVestingReleases(uint64_t currentBlock)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    uint64_t id = 1;
    VestingSchedule schedule;

    while (getVestingSchedule(id, schedule))
    {
      if (schedule.status != VestingStatus::Active)
      {
        ++id;
        continue;
      }

      if (now < schedule.cliffTimestamp)
      {
        ++id;
        continue;
      }

      uint64_t totalVested = 0;
      if (now >= schedule.vestingEndTimestamp)
      {
        totalVested = schedule.totalAllocated;
        schedule.status = VestingStatus::Completed;
      }
      else
      {
        uint64_t totalDuration = schedule.vestingEndTimestamp - schedule.cliffTimestamp;
        uint64_t elapsed = now - schedule.cliffTimestamp;
        totalVested = (schedule.totalAllocated * elapsed) / totalDuration;
      }

      if (totalVested > schedule.releasedAmount)
      {
        uint64_t toRelease = totalVested - schedule.releasedAmount;
        schedule.releasedAmount = totalVested;

        uint64_t balance = 0;
        getBalance(schedule.creator, schedule.tokenId, balance);
        if (balance >= toRelease)
        {
          setBalance(schedule.creator, schedule.tokenId, balance - toRelease);
          uint64_t beneficiaryBalance = 0;
          getBalance(schedule.beneficiary, schedule.tokenId, beneficiaryBalance);
          setBalance(schedule.beneficiary, schedule.tokenId, beneficiaryBalance + toRelease);
        }
      }

      updateVestingSchedule(schedule);
      ++id;
    }

    m_storage.flush();
  }

  void SidechainStorage::processRewardAccrual(uint64_t currentBlock)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    uint64_t now = static_cast<uint64_t>(std::time(nullptr));
    uint64_t poolId = 1;
    RewardPool pool;

    while (getRewardPool(poolId, pool))
    {
      if (!pool.active || pool.totalStaked == 0 || pool.remainingRewards == 0)
      {
        ++poolId;
        continue;
      }

      if (now < pool.startBlock)
      {
        ++poolId;
        continue;
      }

      if (pool.endBlock > 0 && now > pool.endBlock)
      {
        pool.active = false;
        updateRewardPool(pool);
        ++poolId;
        continue;
      }

      uint64_t elapsed = 10;
      if (pool.lastAccrualTimestamp > 0 && now > pool.lastAccrualTimestamp)
        elapsed = now - pool.lastAccrualTimestamp;

      pool.lastAccrualTimestamp = now;

      uint64_t annualRate = pool.rewardRateBasisPoints;
      uint64_t rewardsPerSecond = (pool.totalStaked * annualRate) / 10000 / 31536000;

      if (rewardsPerSecond > 0)
      {
        uint64_t rewardsForElapsed = rewardsPerSecond * elapsed;

        if (rewardsForElapsed > 0 && rewardsForElapsed <= pool.remainingRewards)
        {
          auto stakes = getStakesByPool(poolId);
          for (auto &stake : stakes)
          {
            uint64_t stakeReward = (rewardsForElapsed * stake.amount) / pool.totalStaked;
            if (stakeReward > 0)
            {
              stake.pendingRewards += stakeReward;
              updateStakeEntry(stake);
            }
          }

          pool.remainingRewards -= rewardsForElapsed;
          updateRewardPool(pool);
        }
        else if (rewardsForElapsed > pool.remainingRewards && pool.remainingRewards > 0)
        {
          auto stakes = getStakesByPool(poolId);
          uint64_t remaining = pool.remainingRewards;
          for (auto &stake : stakes)
          {
            uint64_t stakeReward = (remaining * stake.amount) / pool.totalStaked;
            if (stakeReward > 0)
            {
              stake.pendingRewards += stakeReward;
              updateStakeEntry(stake);
            }
          }
          pool.remainingRewards = 0;
          pool.active = false;
          updateRewardPool(pool);
        }
      }

      ++poolId;
    }

    m_storage.flush();
  }

  // ── Account operations ────────────────────────────────────────────────

  bool SidechainStorage::getBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t &balance) const
  {
    std::string key = "bal_" + common::podToHex(address) + "_" + std::to_string(tokenId);
    std::vector<uint8_t> value;
    if (!m_storage.get(key, value) || value.size() < sizeof(uint64_t))
    {
      balance = 0;
      return true;
    }
    memcpy(&balance, value.data(), sizeof(uint64_t));
    return true;
  }

  bool SidechainStorage::setBalance(const crypto::PublicKey &address, uint64_t tokenId, uint64_t balance)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    std::string key = "bal_" + common::podToHex(address) + "_" + std::to_string(tokenId);
    std::vector<uint8_t> value(sizeof(uint64_t));
    memcpy(value.data(), &balance, sizeof(uint64_t));
    m_storage.put(key, value);
    m_storage.flush();
    return true;
  }

  // ── Transaction execution ─────────────────────────────────────────────

  bool SidechainStorage::applyTransaction(const Transaction &tx)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    uint64_t fromBalance, toBalance;
    TokenInfo token;

    switch (tx.type)
    {
    case TransactionType::CreateToken:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      std::cout << "applyTransaction CreateToken: extra=" << extraStr << std::endl;

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos)
      {
        std::cout << "applyTransaction CreateToken: PARSE FAILED" << std::endl;
        return false;
      }

      std::string name = extraStr.substr(0, firstColon);
      std::string symbol = extraStr.substr(firstColon + 1, secondColon - firstColon - 1);

      size_t thirdColon = extraStr.find(':', secondColon + 1);
      size_t fourthColon = std::string::npos;
      size_t fifthColon = std::string::npos;
      size_t sixthColon = std::string::npos;
      uint8_t backingModel = 0;
      uint8_t decimals = 6;
      uint64_t backingRatio = 0;
      uint64_t lockedCCXAmount = 0;

      if (thirdColon != std::string::npos)
      {
        backingModel = static_cast<uint8_t>(std::stoi(extraStr.substr(secondColon + 1, thirdColon - secondColon - 1)));
        fourthColon = extraStr.find(':', thirdColon + 1);

        if (fourthColon != std::string::npos)
        {
          decimals = static_cast<uint8_t>(std::stoi(extraStr.substr(thirdColon + 1, fourthColon - thirdColon - 1)));

          fifthColon = extraStr.find(':', fourthColon + 1);
          if (fifthColon != std::string::npos)
          {
            backingRatio = std::stoull(extraStr.substr(fourthColon + 1, fifthColon - fourthColon - 1));

            sixthColon = extraStr.find(':', fifthColon + 1);
            if (sixthColon != std::string::npos)
            {
              lockedCCXAmount = std::stoull(extraStr.substr(fifthColon + 1, sixthColon - fifthColon - 1));
            }
            else
            {
              lockedCCXAmount = std::stoull(extraStr.substr(fifthColon + 1));
            }
          }
          else
          {
            backingRatio = std::stoull(extraStr.substr(fourthColon + 1));
          }
        }
        else
        {
          decimals = static_cast<uint8_t>(std::stoi(extraStr.substr(thirdColon + 1)));
        }
      }
      else
      {
        backingModel = static_cast<uint8_t>(std::stoi(extraStr.substr(secondColon + 1)));
      }

      if (backingModel == static_cast<uint8_t>(TokenBackingModel::Backed) ||
          backingModel == static_cast<uint8_t>(TokenBackingModel::Hybrid))
      {
        if (lockedCCXAmount == 0)
        {
          std::cout << "applyTransaction CreateToken: backed/hybrid token requires locked CCX" << std::endl;
          return false;
        }
        if (backingRatio == 0)
          backingRatio = SidechainConfig::DEFAULT_BACKING_RATIO;
      }

      TokenInfo newToken;
      newToken.id = nextTokenId();
      newToken.name = name;
      newToken.symbol = symbol;
      newToken.totalSupply = tx.amount;
      newToken.maxSupply = tx.amount * 10;
      newToken.decimals = decimals;
      newToken.backingModel = static_cast<TokenBackingModel>(backingModel);
      newToken.backingRatio = backingRatio;
      newToken.lockedCCXAmount = lockedCCXAmount;

      if (tx.txHash != crypto::Hash())
        newToken.backingTxHash = tx.txHash;

      size_t lastParsedColon = std::string::npos;
      if (sixthColon != std::string::npos)
        lastParsedColon = sixthColon;
      else if (fifthColon != std::string::npos)
        lastParsedColon = fifthColon;
      else if (fourthColon != std::string::npos)
        lastParsedColon = fourthColon;
      else if (thirdColon != std::string::npos)
        lastParsedColon = thirdColon;
      else
        lastParsedColon = secondColon;

      size_t royaltyColon = extraStr.find(':', lastParsedColon + 1);
      if (royaltyColon != std::string::npos)
      {
        std::string royaltyBpsStr = extraStr.substr(lastParsedColon + 1, royaltyColon - lastParsedColon - 1);
        newToken.royaltyBasisPoints = static_cast<uint16_t>(std::stoi(royaltyBpsStr));

        size_t creatorColon = extraStr.find(':', royaltyColon + 1);
        std::string creatorHex;
        if (creatorColon != std::string::npos)
        {
          creatorHex = extraStr.substr(royaltyColon + 1, creatorColon - royaltyColon - 1);
        }
        else
        {
          creatorHex = extraStr.substr(royaltyColon + 1);
        }

        if (!creatorHex.empty())
        {
          common::podFromHex(creatorHex, newToken.creator);
          newToken.royaltiesEnabled = true;
        }
      }

      addToken(newToken);
      setBalance(tx.from, newToken.id, tx.amount);

      if (backingModel == static_cast<uint8_t>(TokenBackingModel::Backed) ||
          backingModel == static_cast<uint8_t>(TokenBackingModel::Hybrid))
      {
        registerAsset("conceal", "native", tx.from, newToken.id, false);
      }

      std::cout << "applyTransaction CreateToken: token created successfully id=" << newToken.id
                << " royalties=" << (newToken.royaltiesEnabled ? "yes" : "no")
                << " royaltyBps=" << newToken.royaltyBasisPoints << std::endl;
      break;
    }

    case TransactionType::Transfer:
    {
      getBalance(tx.from, tx.tokenId, fromBalance);
      getBalance(tx.to, tx.tokenId, toBalance);

      if (fromBalance < tx.amount + tx.fee)
        return false;

      setBalance(tx.from, tx.tokenId, fromBalance - tx.amount - tx.fee);
      setBalance(tx.to, tx.tokenId, toBalance + tx.amount);
      break;
    }

    case TransactionType::Mint:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      bool isNewAsset = (extraStr.find(":new_asset") != std::string::npos);

      uint64_t mintTokenId = tx.tokenId;

      if (isNewAsset && mintTokenId == 0)
      {
        TokenInfo newToken;
        newToken.id = nextTokenId();
        newToken.name = "CCX (Bridged)";
        newToken.symbol = "bCCX";
        newToken.totalSupply = 0;
        newToken.maxSupply = 0;
        newToken.decimals = 6;
        newToken.backingModel = TokenBackingModel::Backed;
        newToken.backingRatio = 100;
        newToken.lockedCCXAmount = 0;

        newToken.fingerprint = generateFingerprint("conceal", "native", tx.from);

        addToken(newToken);

        registerAsset("conceal", "native", tx.from, newToken.id, true);

        mintTokenId = newToken.id;
        token = newToken;

        std::cout << "applyTransaction Mint: auto-created canonical CCX token id="
                  << newToken.id << " fingerprint=" << newToken.fingerprint << std::endl;
      }
      else if (!getToken(mintTokenId, token))
      {
        std::cout << "applyTransaction Mint: token not found id=" << mintTokenId << std::endl;
        return false;
      }

      if (token.backingModel != TokenBackingModel::Backed &&
          token.backingModel != TokenBackingModel::Hybrid)
      {
        std::cout << "applyTransaction Mint: token is not backed/hybrid" << std::endl;
        return false;
      }

      token.totalSupply += tx.amount;
      token.lockedCCXAmount += tx.amount;

      addBridgeLock(tx.to, mintTokenId, tx.amount, tx.txHash, 0);

      updateToken(token);

      getBalance(tx.to, mintTokenId, toBalance);
      setBalance(tx.to, mintTokenId, toBalance + tx.amount);

      std::cout << "applyTransaction Mint: minted " << tx.amount
                << " of token " << mintTokenId
                << " new supply=" << token.totalSupply
                << " lockedCCX=" << token.lockedCCXAmount << std::endl;
      break;
    }

    case TransactionType::Burn:
    {
      if (!getToken(tx.tokenId, token))
      {
        std::cout << "applyTransaction Burn: token not found id=" << tx.tokenId << std::endl;
        return false;
      }

      getBalance(tx.from, tx.tokenId, fromBalance);

      if (fromBalance < tx.amount + tx.fee)
      {
        std::cout << "applyTransaction Burn: insufficient balance" << std::endl;
        return false;
      }

      if (token.totalSupply < tx.amount)
      {
        std::cout << "applyTransaction Burn: amount exceeds supply" << std::endl;
        return false;
      }

      token.totalSupply -= tx.amount;
      updateToken(token);
      setBalance(tx.from, tx.tokenId, fromBalance - tx.amount - tx.fee);

      if (token.backingModel == TokenBackingModel::Backed ||
          token.backingModel == TokenBackingModel::Hybrid)
      {
        if (token.lockedCCXAmount >= tx.amount)
          token.lockedCCXAmount -= tx.amount;
        else
          token.lockedCCXAmount = 0;
        updateToken(token);

        uint64_t remainingToUnlock = tx.amount;
        uint64_t lockId = 1;
        BridgeLockEntry lockEntry;
        while (remainingToUnlock > 0 && getBridgeLock(lockId, lockEntry))
        {
          if (!lockEntry.unlocked && lockEntry.tokenId == tx.tokenId &&
              lockEntry.userAddress == tx.from)
          {
            uint64_t unlockAmount = std::min(remainingToUnlock, lockEntry.amount);
            if (unlockAmount == lockEntry.amount)
              markBridgeLockUnlocked(lockId);
            remainingToUnlock -= unlockAmount;
          }
          ++lockId;
        }

        std::cout << "applyTransaction Burn: burned " << tx.amount
                  << " of token " << tx.tokenId
                  << " remaining supply=" << token.totalSupply
                  << " queued CCX unlock for " << common::podToHex(tx.from).substr(0, 16) << std::endl;
      }

      break;
    }

    case TransactionType::CreateVesting:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);
      size_t thirdColon = extraStr.find(':', secondColon + 1);
      size_t fourthColon = extraStr.find(':', thirdColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos ||
          thirdColon == std::string::npos || fourthColon == std::string::npos)
      {
        std::cout << "applyTransaction CreateVesting: PARSE FAILED" << std::endl;
        return false;
      }

      crypto::PublicKey beneficiary;
      std::string beneficiaryHex = extraStr.substr(0, firstColon);
      common::podFromHex(beneficiaryHex, beneficiary);

      uint64_t totalAllocated = std::stoull(extraStr.substr(firstColon + 1, secondColon - firstColon - 1));
      uint64_t cliffTimestamp = std::stoull(extraStr.substr(secondColon + 1, thirdColon - secondColon - 1));
      uint64_t vestingEndTimestamp = std::stoull(extraStr.substr(thirdColon + 1, fourthColon - thirdColon - 1));

      bool revocable = false;
      size_t fifthColon = extraStr.find(':', fourthColon + 1);
      if (fifthColon != std::string::npos)
      {
        revocable = (std::stoi(extraStr.substr(fourthColon + 1, fifthColon - fourthColon - 1)) != 0);
      }
      else
      {
        revocable = (std::stoi(extraStr.substr(fourthColon + 1)) != 0);
      }

      getBalance(tx.from, tx.tokenId, fromBalance);
      if (fromBalance < totalAllocated + tx.fee)
      {
        std::cout << "applyTransaction CreateVesting: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, tx.tokenId, fromBalance - totalAllocated);

      VestingSchedule schedule;
      schedule.scheduleId = nextVestingScheduleId();
      schedule.creator = tx.from;
      schedule.beneficiary = beneficiary;
      schedule.tokenId = tx.tokenId;
      schedule.totalAllocated = totalAllocated;
      schedule.releasedAmount = 0;
      schedule.cliffTimestamp = cliffTimestamp;
      schedule.vestingEndTimestamp = vestingEndTimestamp;
      schedule.createdAt = static_cast<uint64_t>(std::time(nullptr));
      schedule.revocable = revocable;
      schedule.status = VestingStatus::Active;

      addVestingSchedule(schedule);

      std::cout << "applyTransaction CreateVesting: schedule created id=" << schedule.scheduleId
                << " total=" << totalAllocated << std::endl;
      break;
    }

    case TransactionType::RevokeVesting:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      uint64_t scheduleId = std::stoull(extraStr);

      VestingSchedule schedule;
      if (!getVestingSchedule(scheduleId, schedule))
      {
        std::cout << "applyTransaction RevokeVesting: schedule not found" << std::endl;
        return false;
      }

      if (schedule.creator != tx.from)
      {
        std::cout << "applyTransaction RevokeVesting: not the creator" << std::endl;
        return false;
      }

      if (!schedule.revocable || schedule.status != VestingStatus::Active)
      {
        std::cout << "applyTransaction RevokeVesting: not revocable or not active" << std::endl;
        return false;
      }

      uint64_t unvested = schedule.totalAllocated - schedule.releasedAmount;
      getBalance(tx.from, schedule.tokenId, fromBalance);
      setBalance(tx.from, schedule.tokenId, fromBalance + unvested);

      schedule.status = VestingStatus::Revoked;
      updateVestingSchedule(schedule);

      std::cout << "applyTransaction RevokeVesting: schedule " << scheduleId
                << " revoked, returned " << unvested << " tokens" << std::endl;
      break;
    }

    case TransactionType::CreateRewardPool:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);
      size_t thirdColon = extraStr.find(':', secondColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos || thirdColon == std::string::npos)
      {
        std::cout << "applyTransaction CreateRewardPool: PARSE FAILED" << std::endl;
        return false;
      }

      uint64_t rewardAmount = std::stoull(extraStr.substr(0, firstColon));
      uint64_t rewardRate = std::stoull(extraStr.substr(firstColon + 1, secondColon - firstColon - 1));
      uint64_t endBlock = std::stoull(extraStr.substr(secondColon + 1, thirdColon - secondColon - 1));

      getBalance(tx.from, tx.tokenId, fromBalance);
      if (fromBalance < rewardAmount + tx.fee)
      {
        std::cout << "applyTransaction CreateRewardPool: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, tx.tokenId, fromBalance - rewardAmount);

      RewardPool pool;
      pool.poolId = nextRewardPoolId();
      pool.creator = tx.from;
      pool.tokenId = tx.tokenId;
      pool.totalRewards = rewardAmount;
      pool.remainingRewards = rewardAmount;
      pool.rewardRateBasisPoints = static_cast<uint16_t>(rewardRate);
      pool.totalStaked = 0;
      pool.startBlock = topBlockHeight();
      pool.endBlock = endBlock;
      pool.active = true;

      addRewardPool(pool);

      std::cout << "applyTransaction CreateRewardPool: pool created id=" << pool.poolId
                << " rewards=" << rewardAmount << std::endl;
      break;
    }

    case TransactionType::FundRewardPool:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());

      size_t firstColon = extraStr.find(':');
      uint64_t poolId = std::stoull(extraStr.substr(0, firstColon));
      uint64_t fundAmount = tx.amount;

      if (firstColon != std::string::npos)
      {
        fundAmount = std::stoull(extraStr.substr(firstColon + 1));
      }

      RewardPool pool;
      if (!getRewardPool(poolId, pool))
      {
        std::cout << "applyTransaction FundRewardPool: pool not found" << std::endl;
        return false;
      }

      getBalance(tx.from, pool.tokenId, fromBalance);
      if (fromBalance < fundAmount + tx.fee)
      {
        std::cout << "applyTransaction FundRewardPool: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, pool.tokenId, fromBalance - fundAmount);
      pool.totalRewards += fundAmount;
      pool.remainingRewards += fundAmount;
      updateRewardPool(pool);

      std::cout << "applyTransaction FundRewardPool: pool " << poolId
                << " funded with " << fundAmount << std::endl;
      break;
    }
    case TransactionType::Stake:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      uint64_t poolId = std::stoull(extraStr);

      RewardPool pool;
      if (!getRewardPool(poolId, pool) || !pool.active)
      {
        std::cout << "applyTransaction Stake: pool not found or inactive" << std::endl;
        return false;
      }

      getBalance(tx.from, tx.tokenId, fromBalance);
      if (fromBalance < tx.amount + tx.fee)
      {
        std::cout << "applyTransaction Stake: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, tx.tokenId, fromBalance - tx.amount);

      StakeEntry entry;
      entry.entryId = nextStakeEntryId();
      entry.owner = tx.from;
      entry.poolId = poolId;
      entry.tokenId = tx.tokenId;
      entry.amount = tx.amount;
      entry.startBlock = topBlockHeight();
      entry.lastClaimBlock = topBlockHeight();
      entry.pendingRewards = 0;

      addStakeEntry(entry);

      pool.totalStaked += tx.amount;
      updateRewardPool(pool);

      std::cout << "applyTransaction Stake: staked " << tx.amount
                << " in pool " << poolId << std::endl;
      break;
    }

    case TransactionType::Unstake:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      uint64_t entryId = std::stoull(extraStr);

      StakeEntry entry;
      if (!getStakeEntry(entryId, entry) || entry.owner != tx.from)
      {
        std::cout << "applyTransaction Unstake: entry not found or not owner" << std::endl;
        return false;
      }

      RewardPool pool;
      if (!getRewardPool(entry.poolId, pool))
      {
        std::cout << "applyTransaction Unstake: pool not found" << std::endl;
        return false;
      }

      uint64_t returnAmount = entry.amount + entry.pendingRewards;
      getBalance(tx.from, entry.tokenId, fromBalance);
      setBalance(tx.from, entry.tokenId, fromBalance + returnAmount);

      if (pool.totalStaked >= entry.amount)
        pool.totalStaked -= entry.amount;
      updateRewardPool(pool);

      entry.amount = 0;
      entry.pendingRewards = 0;
      updateStakeEntry(entry);

      std::cout << "applyTransaction Unstake: unstaked " << entry.amount
                << " from pool " << entry.poolId << std::endl;
      break;
    }

    case TransactionType::ClaimReward:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      uint64_t entryId = std::stoull(extraStr);

      StakeEntry entry;
      if (!getStakeEntry(entryId, entry) || entry.owner != tx.from)
      {
        std::cout << "applyTransaction ClaimReward: entry not found or not owner" << std::endl;
        return false;
      }

      if (entry.pendingRewards == 0)
      {
        std::cout << "applyTransaction ClaimReward: no pending rewards" << std::endl;
        return false;
      }

      uint64_t reward = entry.pendingRewards;
      entry.pendingRewards = 0;
      entry.lastClaimBlock = topBlockHeight();
      updateStakeEntry(entry);

      getBalance(tx.from, entry.tokenId, fromBalance);
      setBalance(tx.from, entry.tokenId, fromBalance + reward);

      std::cout << "applyTransaction ClaimReward: claimed " << reward << std::endl;
      break;
    }

    case TransactionType::AmmCreatePool:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);
      size_t thirdColon = extraStr.find(':', secondColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos || thirdColon == std::string::npos)
      {
        std::cout << "applyTransaction AmmCreatePool: PARSE FAILED" << std::endl;
        return false;
      }

      uint64_t tokenIdB = std::stoull(extraStr.substr(0, firstColon));
      uint64_t amountA = tx.amount;
      uint64_t amountB = std::stoull(extraStr.substr(firstColon + 1, secondColon - firstColon - 1));
      uint16_t feeBasisPoints = static_cast<uint16_t>(std::stoi(extraStr.substr(secondColon + 1, thirdColon - secondColon - 1)));

      getBalance(tx.from, tx.tokenId, fromBalance);
      uint64_t balanceB = 0;
      getBalance(tx.from, tokenIdB, balanceB);

      if (fromBalance < amountA || balanceB < amountB)
      {
        std::cout << "applyTransaction AmmCreatePool: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, tx.tokenId, fromBalance - amountA);
      setBalance(tx.from, tokenIdB, balanceB - amountB);

      uint64_t poolId = 1;
      std::vector<uint8_t> poolData;
      while (getMeta("amm_pool_" + std::to_string(poolId), poolData))
      {
        ++poolId;
      }

      AmmPool pool;
      pool.poolId = poolId;
      pool.creator = tx.from;
      pool.tokenIdA = tx.tokenId;
      pool.tokenIdB = tokenIdB;
      pool.reserveA = amountA;
      pool.reserveB = amountB;
      pool.totalLiquidity = static_cast<uint64_t>(std::sqrt(amountA * amountB));
      pool.feeBasisPoints = feeBasisPoints;
      pool.active = true;

      cn::BinaryArray ba = cn::toBinaryArray(pool);
      std::vector<uint8_t> vec(ba.begin(), ba.end());
      putMeta("amm_pool_" + std::to_string(poolId), vec);

      AmmPosition pos;
      pos.positionId = 1;
      pos.owner = tx.from;
      pos.poolId = poolId;
      pos.liquidity = pool.totalLiquidity;
      pos.lastFeeCheckpoint = 0;

      cn::BinaryArray posBa = cn::toBinaryArray(pos);
      std::vector<uint8_t> posVec(posBa.begin(), posBa.end());
      putMeta("amm_pos_1", posVec);

      std::cout << "applyTransaction AmmCreatePool: pool created id=" << poolId << std::endl;
      break;
    }

    case TransactionType::AmmAddLiquidity:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos)
      {
        std::cout << "applyTransaction AmmAddLiquidity: PARSE FAILED" << std::endl;
        return false;
      }

      uint64_t poolId = std::stoull(extraStr.substr(0, firstColon));
      uint64_t amountA = tx.amount;
      uint64_t amountB = std::stoull(extraStr.substr(firstColon + 1, secondColon - firstColon - 1));

      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!getMeta("amm_pool_" + std::to_string(poolId), poolData))
      {
        std::cout << "applyTransaction AmmAddLiquidity: pool not found" << std::endl;
        return false;
      }
      cn::BinaryArray poolBa(poolData.begin(), poolData.end());
      cn::fromBinaryArray(pool, poolBa);

      if (!pool.active)
      {
        std::cout << "applyTransaction AmmAddLiquidity: pool inactive" << std::endl;
        return false;
      }

      getBalance(tx.from, tx.tokenId, fromBalance);
      uint64_t balanceB = 0;
      getBalance(tx.from, pool.tokenIdB, balanceB);

      if (fromBalance < amountA || balanceB < amountB)
      {
        std::cout << "applyTransaction AmmAddLiquidity: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, tx.tokenId, fromBalance - amountA);
      setBalance(tx.from, pool.tokenIdB, balanceB - amountB);

      uint64_t liquidityMinted = 0;
      if (pool.totalLiquidity == 0)
        liquidityMinted = static_cast<uint64_t>(std::sqrt(amountA * amountB));
      else
      {
        uint64_t shareA = (amountA * pool.totalLiquidity) / pool.reserveA;
        uint64_t shareB = (amountB * pool.totalLiquidity) / pool.reserveB;
        liquidityMinted = std::min(shareA, shareB);
      }

      pool.reserveA += amountA;
      pool.reserveB += amountB;
      pool.totalLiquidity += liquidityMinted;

      cn::BinaryArray updatedBa = cn::toBinaryArray(pool);
      std::vector<uint8_t> updatedVec(updatedBa.begin(), updatedBa.end());
      putMeta("amm_pool_" + std::to_string(poolId), updatedVec);

      uint64_t posId = 1;
      std::vector<uint8_t> posData;
      AmmPosition pos;
      bool found = false;

      while (getMeta("amm_pos_" + std::to_string(posId), posData))
      {
        cn::BinaryArray posBa(posData.begin(), posData.end());
        cn::fromBinaryArray(pos, posBa);
        if (pos.poolId == poolId && pos.owner == tx.from)
        {
          pos.liquidity += liquidityMinted;
          found = true;
          break;
        }
        ++posId;
        posData.clear();
      }

      if (!found)
      {
        pos.positionId = posId;
        pos.owner = tx.from;
        pos.poolId = poolId;
        pos.liquidity = liquidityMinted;
        pos.lastFeeCheckpoint = 0;
      }

      cn::BinaryArray posBa = cn::toBinaryArray(pos);
      std::vector<uint8_t> posVec(posBa.begin(), posBa.end());
      putMeta("amm_pos_" + std::to_string(pos.positionId), posVec);

      std::cout << "applyTransaction AmmAddLiquidity: pool=" << poolId
                << " amountA=" << amountA << " amountB=" << amountB << std::endl;
      break;
    }

    case TransactionType::AmmRemoveLiquidity:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());
      uint64_t positionId = std::stoull(extraStr);

      AmmPosition pos;
      std::vector<uint8_t> posData;
      if (!getMeta("amm_pos_" + std::to_string(positionId), posData))
      {
        std::cout << "applyTransaction AmmRemoveLiquidity: position not found" << std::endl;
        return false;
      }
      cn::BinaryArray posBa(posData.begin(), posData.end());
      cn::fromBinaryArray(pos, posBa);

      if (pos.owner != tx.from)
      {
        std::cout << "applyTransaction AmmRemoveLiquidity: not position owner" << std::endl;
        return false;
      }

      if (pos.liquidity == 0)
      {
        std::cout << "applyTransaction AmmRemoveLiquidity: position empty" << std::endl;
        return false;
      }

      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!getMeta("amm_pool_" + std::to_string(pos.poolId), poolData))
      {
        std::cout << "applyTransaction AmmRemoveLiquidity: pool not found" << std::endl;
        return false;
      }
      cn::BinaryArray poolBa(poolData.begin(), poolData.end());
      cn::fromBinaryArray(pool, poolBa);

      uint64_t shareA = (pos.liquidity * pool.reserveA) / pool.totalLiquidity;
      uint64_t shareB = (pos.liquidity * pool.reserveB) / pool.totalLiquidity;

      pool.reserveA -= shareA;
      pool.reserveB -= shareB;
      pool.totalLiquidity -= pos.liquidity;

      cn::BinaryArray updatedBa = cn::toBinaryArray(pool);
      std::vector<uint8_t> updatedVec(updatedBa.begin(), updatedBa.end());
      putMeta("amm_pool_" + std::to_string(pos.poolId), updatedVec);

      getBalance(tx.from, pool.tokenIdA, fromBalance);
      uint64_t balanceB = 0;
      getBalance(tx.from, pool.tokenIdB, balanceB);
      setBalance(tx.from, pool.tokenIdA, fromBalance + shareA);
      setBalance(tx.from, pool.tokenIdB, balanceB + shareB);

      pos.liquidity = 0;
      cn::BinaryArray posBa2 = cn::toBinaryArray(pos);
      std::vector<uint8_t> posVec2(posBa2.begin(), posBa2.end());
      putMeta("amm_pos_" + std::to_string(positionId), posVec2);

      std::cout << "applyTransaction AmmRemoveLiquidity: position=" << positionId
                << " shareA=" << shareA << " shareB=" << shareB << std::endl;
      break;
    }

    case TransactionType::AmmSwap:
    {
      std::string extraStr(tx.extra.begin(), tx.extra.end());

      size_t firstColon = extraStr.find(':');
      size_t secondColon = extraStr.find(':', firstColon + 1);

      if (firstColon == std::string::npos || secondColon == std::string::npos)
      {
        std::cout << "applyTransaction AmmSwap: PARSE FAILED" << std::endl;
        return false;
      }

      uint64_t poolId = std::stoull(extraStr.substr(0, firstColon));
      uint64_t minAmountOut = std::stoull(extraStr.substr(firstColon + 1, secondColon - firstColon - 1));

      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!getMeta("amm_pool_" + std::to_string(poolId), poolData))
      {
        std::cout << "applyTransaction AmmSwap: pool not found" << std::endl;
        return false;
      }
      cn::BinaryArray poolBa(poolData.begin(), poolData.end());
      cn::fromBinaryArray(pool, poolBa);

      if (!pool.active)
      {
        std::cout << "applyTransaction AmmSwap: pool inactive" << std::endl;
        return false;
      }

      uint64_t tokenIdIn = tx.tokenId;
      uint64_t tokenIdOut;
      uint64_t reserveIn, reserveOut;

      if (tokenIdIn == pool.tokenIdA)
      {
        tokenIdOut = pool.tokenIdB;
        reserveIn = pool.reserveA;
        reserveOut = pool.reserveB;
      }
      else if (tokenIdIn == pool.tokenIdB)
      {
        tokenIdOut = pool.tokenIdA;
        reserveIn = pool.reserveB;
        reserveOut = pool.reserveA;
      }
      else
      {
        std::cout << "applyTransaction AmmSwap: token not in pool" << std::endl;
        return false;
      }

      uint64_t amountInAfterFee = tx.amount * (10000 - pool.feeBasisPoints) / 10000;
      uint64_t k = reserveIn * reserveOut;
      uint64_t newReserveIn = reserveIn + amountInAfterFee;
      uint64_t newReserveOut = k / newReserveIn;
      uint64_t amountOut = reserveOut - newReserveOut;

      if (amountOut < minAmountOut)
      {
        std::cout << "applyTransaction AmmSwap: slippage exceeded" << std::endl;
        return false;
      }

      getBalance(tx.from, tokenIdIn, fromBalance);
      if (fromBalance < tx.amount)
      {
        std::cout << "applyTransaction AmmSwap: insufficient balance" << std::endl;
        return false;
      }

      setBalance(tx.from, tokenIdIn, fromBalance - tx.amount);

      uint64_t outBalance = 0;
      getBalance(tx.to, tokenIdOut, outBalance);
      setBalance(tx.to, tokenIdOut, outBalance + amountOut);

      if (tokenIdIn == pool.tokenIdA)
      {
        pool.reserveA += tx.amount;
        pool.reserveB -= amountOut;
      }
      else
      {
        pool.reserveB += tx.amount;
        pool.reserveA -= amountOut;
      }

      cn::BinaryArray updatedBa = cn::toBinaryArray(pool);
      std::vector<uint8_t> updatedVec(updatedBa.begin(), updatedBa.end());
      putMeta("amm_pool_" + std::to_string(poolId), updatedVec);

      std::cout << "applyTransaction AmmSwap: pool=" << poolId
                << " in=" << tx.amount << " out=" << amountOut << std::endl;
      break;
    }

    default:
      break;
    }

    if (tx.fee > 0)
    {
      getBalance(tx.from, 0, fromBalance);
      if (fromBalance < tx.fee)
        return false;
      setBalance(tx.from, 0, fromBalance - tx.fee);
    }

    m_storage.flush();
    return true;
  }

  // ── Validator operations ───────────────────────────────────────────────

  bool SidechainStorage::addValidator(const ValidatorInfo &validator)
  {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    cn::BinaryArray ba = cn::toBinaryArray(validator);
    std::vector<uint8_t> vec(ba.begin(), ba.end());
    std::string key = "validator_" + std::to_string(validator.id);
    m_storage.put(key, vec);
    m_storage.flush();
    return true;
  }

  std::vector<ValidatorInfo> SidechainStorage::getActiveValidators() const
  {
    std::vector<ValidatorInfo> result;
    uint32_t id = 0;
    while (true)
    {
      std::string key = "validator_" + std::to_string(id);
      std::vector<uint8_t> value;
      if (!m_storage.get(key, value))
        break;
      cn::BinaryArray ba(value.begin(), value.end());
      ValidatorInfo validator;
      if (cn::fromBinaryArray(validator, ba) && validator.active)
        result.push_back(validator);
      ++id;
    }
    return result;
  }

  void SidechainStorage::flush()
  {
    m_storage.flush();
  }

} // namespace Sidechain