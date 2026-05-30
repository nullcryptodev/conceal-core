// SidechainTypes.h — all data structures for blocks, transactions, tokens
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "crypto/crypto.h"
#include "Serialization/ISerializer.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"

namespace Sidechain
{
  // Tokens
  enum class TokenBackingModel : uint8_t
  {
    Unbacked = 0,
    Backed = 1,
    Hybrid = 2
  };

  struct TokenInfo
  {
    uint64_t id = 0;
    std::string fingerprint; // hash(sourceChain:sourceAsset:bridgeOperatorPubKey)
    std::string name;
    std::string symbol;
    uint64_t totalSupply = 0;
    uint64_t maxSupply = 0;
    uint8_t decimals = 6;
    TokenBackingModel backingModel = TokenBackingModel::Unbacked;
    uint64_t backingRatio = 0;
    crypto::Hash backingTxHash;
    uint64_t lockedCCXAmount = 0;
    uint16_t royaltyBasisPoints = 0; // e.g., 250 = 2.5%
    crypto::PublicKey creator;       // Who receives royalties
    bool royaltiesEnabled = false;   // Toggle for royalty collection

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      s(fingerprint, "fingerprint");
      s(name, "name");
      s(symbol, "symbol");
      s(totalSupply, "totalSupply");
      s(maxSupply, "maxSupply");
      s(decimals, "decimals");
      uint8_t model = static_cast<uint8_t>(backingModel);
      s(model, "backingModel");
      backingModel = static_cast<TokenBackingModel>(model);
      s(backingRatio, "backingRatio");
      s(backingTxHash, "backingTxHash");
      s(lockedCCXAmount, "lockedCCXAmount");
      s(royaltyBasisPoints, "royaltyBasisPoints");
      s(creator, "creator");
      s(royaltiesEnabled, "royaltiesEnabled");
    }
  };

  // Transactions
  enum class TransactionType : uint8_t
  {
    Transfer = 0,
    Mint = 1,
    Burn = 2,
    CreateToken = 3,
    Stake = 4,
    Unstake = 5,
    ClaimReward = 6,
    CreateVesting = 7,
    RevokeVesting = 8,
    CreateRewardPool = 9,
    FundRewardPool = 10,
    AmmCreatePool = 11,
    AmmAddLiquidity = 12,
    AmmRemoveLiquidity = 13,
    AmmSwap = 14
  };

  struct Transaction
  {
    uint64_t id = 0;
    TransactionType type = TransactionType::Transfer;
    crypto::PublicKey from;
    crypto::PublicKey to;
    uint64_t amount = 0;
    uint64_t fee = 0;
    uint64_t tokenId = 0;
    uint64_t feeTokenId = 0;
    uint64_t timestamp = 0;
    crypto::Hash txHash;
    std::vector<uint8_t> extra;
    crypto::Signature signature;

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      uint8_t typeVal = static_cast<uint8_t>(type);
      s(typeVal, "type");
      type = static_cast<TransactionType>(typeVal);
      s(from, "from");
      s(to, "to");
      s(amount, "amount");
      s(fee, "fee");
      s(tokenId, "tokenId");
      s(feeTokenId, "feeTokenId");
      s(timestamp, "timestamp");
      s(extra, "extra");
      s(txHash, "txHash");
      s(signature, "signature");
    }
  };

  // Blocks
  struct BlockHeader
  {
    uint64_t height = 0;
    crypto::Hash previousBlockHash;
    crypto::Hash blockHash;
    uint64_t timestamp = 0;
    uint32_t validatorId = 0;
    crypto::Signature validatorSignature;

    void serialize(cn::ISerializer &s)
    {
      s(height, "height");
      s(previousBlockHash, "previousBlockHash");
      s(blockHash, "blockHash");
      s(timestamp, "timestamp");
      s(validatorId, "validatorId");
      s(validatorSignature, "validatorSignature");
    }
  };

  struct Block
  {
    BlockHeader header;
    std::vector<Transaction> transactions;

    void serialize(cn::ISerializer &s)
    {
      s(header, "header");
      s(transactions, "transactions");
    }
  };

  // Validator
  struct ValidatorInfo
  {
    uint32_t id = 0;
    crypto::PublicKey publicKey;
    crypto::SecretKey secretKey;
    std::string host;
    uint16_t port = 0;
    uint64_t stake = 0;
    bool active = true;

    void serialize(cn::ISerializer &s)
    {
      s(id, "id");
      s(publicKey, "publicKey");
      s(host, "host");
      s(port, "port");
      s(stake, "stake");
      s(active, "active");
    }
  };

  // State
  struct AccountState
  {
    crypto::PublicKey address;
    std::unordered_map<uint64_t, uint64_t> balances;
  };

  // Vesting
  enum class VestingStatus : uint8_t
  {
    Active = 0,
    Completed = 1,
    Revoked = 2
  };

  struct VestingSchedule
  {
    uint64_t scheduleId = 0;
    crypto::PublicKey creator;     // Who created the vesting
    crypto::PublicKey beneficiary; // Who receives tokens
    uint64_t tokenId = 0;          // Which token is vested
    uint64_t totalAllocated = 0;   // Total tokens in schedule
    uint64_t releasedAmount = 0;   // How much has been released so far
    uint64_t cliffTimestamp = 0;   // Unix timestamp when cliff ends
    uint64_t vestingEndTimestamp = 0; // Unix timestamp when fully vested
    uint64_t createdAt = 0;           // Unix timestamp when created
    bool revocable = false;        // Can creator cancel remaining?
    VestingStatus status = VestingStatus::Active;

    void serialize(cn::ISerializer &s)
    {
      s(scheduleId, "scheduleId");
      s(creator, "creator");
      s(beneficiary, "beneficiary");
      s(tokenId, "tokenId");
      s(totalAllocated, "totalAllocated");
      s(releasedAmount, "releasedAmount");
      s(cliffTimestamp, "cliffTimestamp");
      s(vestingEndTimestamp, "vestingEndTimestamp");
      s(createdAt, "createdAt");
      s(revocable, "revocable");
      uint8_t statusVal = static_cast<uint8_t>(status);
      s(statusVal, "status");
      status = static_cast<VestingStatus>(statusVal);
    }
  };

  // Reward pool for incentivized staking
  struct RewardPool
  {
    uint64_t poolId = 0;
    crypto::PublicKey creator;          // Who funded the pool
    uint64_t tokenId = 0;               // Token being rewarded (same as staked token)
    uint64_t totalRewards = 0;          // Total rewards deposited
    uint64_t remainingRewards = 0;      // Rewards not yet distributed
    uint64_t rewardRateBasisPoints = 0; // Annual rate in basis points (e.g., 500 = 5%)
    uint64_t lastAccrualTimestamp = 0;
    uint64_t totalStaked = 0;           // Total tokens currently staked in this pool
    uint64_t startBlock = 0;            // When rewards begin accruing
    uint64_t endBlock = 0;              // When rewards stop (0 = indefinite)
    bool active = true;

    void serialize(cn::ISerializer &s)
    {
      s(poolId, "poolId");
      s(creator, "creator");
      s(tokenId, "tokenId");
      s(totalRewards, "totalRewards");
      s(remainingRewards, "remainingRewards");
      s(rewardRateBasisPoints, "rewardRateBasisPoints");
      s(lastAccrualTimestamp, "lastAccrualTimestamp");
      s(totalStaked, "totalStaked");
      s(startBlock, "startBlock");
      s(endBlock, "endBlock");
      s(active, "active");
    }
  };

  // Stake entry tracking a user's position in a reward pool
  struct StakeEntry
  {
    uint64_t entryId = 0;
    crypto::PublicKey owner;
    uint64_t poolId = 0;
    uint64_t tokenId = 0;
    uint64_t amount = 0;
    uint64_t startBlock = 0;
    uint64_t lastClaimBlock = 0;
    uint64_t pendingRewards = 0;

    void serialize(cn::ISerializer &s)
    {
      s(entryId, "entryId");
      s(owner, "owner");
      s(poolId, "poolId");
      s(tokenId, "tokenId");
      s(amount, "amount");
      s(startBlock, "startBlock");
      s(lastClaimBlock, "lastClaimBlock");
      s(pendingRewards, "pendingRewards");
    }
  };

  // AMM liquidity pool
  struct AmmPool
  {
    uint64_t poolId = 0;
    crypto::PublicKey creator;    // Who created the pool
    uint64_t tokenIdA = 0;        // First token in the pair
    uint64_t tokenIdB = 0;        // Second token in the pair
    uint64_t reserveA = 0;        // Current reserve of token A
    uint64_t reserveB = 0;        // Current reserve of token B
    uint64_t totalLiquidity = 0;  // Total LP tokens minted
    uint16_t feeBasisPoints = 30; // Fee in basis points (30 = 0.3%)
    bool active = true;

    void serialize(cn::ISerializer &s)
    {
      s(poolId, "poolId");
      s(creator, "creator");
      s(tokenIdA, "tokenIdA");
      s(tokenIdB, "tokenIdB");
      s(reserveA, "reserveA");
      s(reserveB, "reserveB");
      s(totalLiquidity, "totalLiquidity");
      s(feeBasisPoints, "feeBasisPoints");
      s(active, "active");
    }
  };

  // AMM liquidity position tracking a user's share of a pool
  struct AmmPosition
  {
    uint64_t positionId = 0;
    crypto::PublicKey owner;
    uint64_t poolId = 0;
    uint64_t liquidity = 0;         // LP tokens owned
    uint64_t lastFeeCheckpoint = 0; // Block height of last fee accrual

    void serialize(cn::ISerializer &s)
    {
      s(positionId, "positionId");
      s(owner, "owner");
      s(poolId, "poolId");
      s(liquidity, "liquidity");
      s(lastFeeCheckpoint, "lastFeeCheckpoint");
    }
  };
}