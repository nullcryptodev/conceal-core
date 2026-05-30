// BoltAMM.h — Automated Market Maker engine
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include <vector>
#include <mutex>
#include <string>
#include <functional>

namespace Sidechain
{
  class SidechainStorage;
  class SidechainValidator;

  namespace BoltAMM
  {
    struct SwapResult
    {
      bool success = false;
      uint64_t amountIn = 0;
      uint64_t amountOut = 0;
      uint64_t fee = 0;
      uint64_t poolId = 0;
      std::string error;
    };

    class Engine
    {
    public:
      Engine();

      void setStorage(SidechainStorage &storage);
      void setValidator(SidechainValidator &validator);

      // Pool management
      bool createPool(uint64_t tokenIdA, uint64_t tokenIdB,
                      uint64_t amountA, uint64_t amountB,
                      const crypto::PublicKey &creator,
                      uint16_t feeBasisPoints = 30);

      bool addLiquidity(uint64_t poolId,
                        uint64_t amountA, uint64_t amountB,
                        const crypto::PublicKey &provider);

      bool removeLiquidity(uint64_t positionId,
                           const crypto::PublicKey &owner);

      // Swap tokenA for tokenB using the pool
      SwapResult swap(uint64_t poolId,
                      uint64_t tokenIdIn, uint64_t amountIn,
                      uint64_t minAmountOut,
                      const crypto::PublicKey &swapper);

      // Read-only queries
      uint64_t getAmountOut(uint64_t poolId,
                            uint64_t tokenIdIn, uint64_t amountIn) const;
      uint64_t getAmountIn(uint64_t poolId,
                           uint64_t tokenIdIn, uint64_t amountOut) const;

      std::vector<AmmPool> getPools() const;
      std::vector<AmmPool> getPoolsByToken(uint64_t tokenId) const;
      std::vector<AmmPosition> getPositions(const crypto::PublicKey &owner) const;

      void processBlock(const Block &block);

    private:
      SidechainStorage *m_storage = nullptr;
      SidechainValidator *m_validator = nullptr;
      mutable std::mutex m_mutex;

      // Constant product calculation
      uint64_t calculateSwapAmount(uint64_t reserveIn, uint64_t reserveOut,
                                   uint64_t amountIn, uint16_t feeBps) const;
    };
  }
}