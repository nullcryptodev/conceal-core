// BoltAMM.cpp — Automated Market Maker engine
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltAMM.h"
#include "SidechainStorage.h"
#include "SidechainValidator.h"
#include "Common/StringTools.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include <algorithm>
#include <iostream>
#include <cmath>

namespace Sidechain
{
  namespace BoltAMM
  {
    Engine::Engine()
    {
    }

    void Engine::setStorage(SidechainStorage &storage)
    {
      m_storage = &storage;
    }

    void Engine::setValidator(SidechainValidator &validator)
    {
      m_validator = &validator;
    }

    // Constant product: reserveIn * reserveOut = k
    // After swap: (reserveIn + amountIn) * (reserveOut - amountOut) = k
    // amountOut = reserveOut - (k / (reserveIn + amountIn))
    uint64_t Engine::calculateSwapAmount(uint64_t reserveIn, uint64_t reserveOut,
                                         uint64_t amountIn, uint16_t feeBps) const
    {
      if (reserveIn == 0 || reserveOut == 0 || amountIn == 0)
        return 0;

      // Deduct fee from input
      uint64_t amountInAfterFee = amountIn * (10000 - feeBps) / 10000;

      // Constant product
      uint64_t k = reserveIn * reserveOut;

      // New reserve after adding input
      uint64_t newReserveIn = reserveIn + amountInAfterFee;

      // Calculate output
      uint64_t newReserveOut = k / newReserveIn;
      uint64_t amountOut = reserveOut - newReserveOut;

      return amountOut;
    }

    bool Engine::createPool(uint64_t tokenIdA, uint64_t tokenIdB,
                            uint64_t amountA, uint64_t amountB,
                            const crypto::PublicKey &creator,
                            uint16_t feeBasisPoints)
    {
      if (!m_storage)
        return false;

      if (tokenIdA == tokenIdB)
      {
        std::cerr << "AMM: cannot create pool with identical tokens" << std::endl;
        return false;
      }

      if (amountA == 0 || amountB == 0)
      {
        std::cerr << "AMM: must provide both tokens to seed pool" << std::endl;
        return false;
      }

      // Find next pool ID by scanning
      uint64_t poolId = 1;
      std::vector<uint8_t> poolData;
      while (m_storage->getMeta("amm_pool_" + std::to_string(poolId), poolData))
      {
        ++poolId;
      }

      // Verify creator has sufficient balances
      uint64_t balanceA = 0, balanceB = 0;
      m_storage->getBalance(creator, tokenIdA, balanceA);
      m_storage->getBalance(creator, tokenIdB, balanceB);

      if (balanceA < amountA || balanceB < amountB)
      {
        std::cerr << "AMM: insufficient balance to seed pool" << std::endl;
        return false;
      }

      // Deduct from creator
      m_storage->setBalance(creator, tokenIdA, balanceA - amountA);
      m_storage->setBalance(creator, tokenIdB, balanceB - amountB);

      // Create the pool
      AmmPool pool;
      pool.poolId = poolId;
      pool.creator = creator;
      pool.tokenIdA = tokenIdA;
      pool.tokenIdB = tokenIdB;
      pool.reserveA = amountA;
      pool.reserveB = amountB;
      pool.totalLiquidity = static_cast<uint64_t>(std::sqrt(amountA * amountB));
      pool.feeBasisPoints = feeBasisPoints;
      pool.active = true;

      // Store pool
      cn::BinaryArray ba = cn::toBinaryArray(pool);
      m_storage->putMeta("amm_pool_" + std::to_string(pool.poolId), ba);

      // Give LP tokens to creator as initial position
      AmmPosition pos;
      pos.positionId = 1;
      pos.owner = creator;
      pos.poolId = pool.poolId;
      pos.liquidity = pool.totalLiquidity;
      pos.lastFeeCheckpoint = 0;

      cn::BinaryArray posBa = cn::toBinaryArray(pos);
      m_storage->putMeta("amm_pos_" + std::to_string(pos.positionId), posBa);

      std::cout << "AMM: pool created id=" << pool.poolId
                << " tokenA=" << tokenIdA << " tokenB=" << tokenIdB
                << " reserveA=" << amountA << " reserveB=" << amountB
                << " liquidity=" << pool.totalLiquidity << std::endl;

      return true;
    }

    bool Engine::addLiquidity(uint64_t poolId,
                              uint64_t amountA, uint64_t amountB,
                              const crypto::PublicKey &provider)
    {
      if (!m_storage)
        return false;

      // Load pool
      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!m_storage->getMeta("amm_pool_" + std::to_string(poolId), poolData))
      {
        std::cerr << "AMM: pool not found" << std::endl;
        return false;
      }
      cn::fromBinaryArray(pool, poolData);

      if (!pool.active)
      {
        std::cerr << "AMM: pool is inactive" << std::endl;
        return false;
      }

      // Verify provider has sufficient balances
      uint64_t balanceA = 0, balanceB = 0;
      m_storage->getBalance(provider, pool.tokenIdA, balanceA);
      m_storage->getBalance(provider, pool.tokenIdB, balanceB);

      if (balanceA < amountA || balanceB < amountB)
      {
        std::cerr << "AMM: insufficient balance to add liquidity" << std::endl;
        return false;
      }

      // Deduct from provider
      m_storage->setBalance(provider, pool.tokenIdA, balanceA - amountA);
      m_storage->setBalance(provider, pool.tokenIdB, balanceB - amountB);

      // Calculate LP tokens to mint proportional to existing liquidity
      uint64_t liquidityMinted = 0;
      if (pool.totalLiquidity == 0)
      {
        liquidityMinted = static_cast<uint64_t>(std::sqrt(amountA * amountB));
      }
      else
      {
        uint64_t shareA = (amountA * pool.totalLiquidity) / pool.reserveA;
        uint64_t shareB = (amountB * pool.totalLiquidity) / pool.reserveB;
        liquidityMinted = std::min(shareA, shareB);
      }

      // Update pool reserves
      pool.reserveA += amountA;
      pool.reserveB += amountB;
      pool.totalLiquidity += liquidityMinted;

      // Store updated pool
      cn::BinaryArray updatedBa = cn::toBinaryArray(pool);
      m_storage->putMeta("amm_pool_" + std::to_string(poolId), updatedBa);

      // Find or create position for this provider
      AmmPosition pos;
      bool found = false;
      uint64_t posId = 1;
      std::vector<uint8_t> posData;
      while (m_storage->getMeta("amm_pos_" + std::to_string(posId), posData))
      {
        cn::fromBinaryArray(pos, posData);
        if (pos.poolId == poolId && pos.owner == provider)
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
        pos.owner = provider;
        pos.poolId = poolId;
        pos.liquidity = liquidityMinted;
        pos.lastFeeCheckpoint = 0;
      }

      cn::BinaryArray posBa = cn::toBinaryArray(pos);
      m_storage->putMeta("amm_pos_" + std::to_string(pos.positionId), posBa);

      std::cout << "AMM: liquidity added pool=" << poolId
                << " amountA=" << amountA << " amountB=" << amountB
                << " minted=" << liquidityMinted << std::endl;

      return true;
    }

    bool Engine::removeLiquidity(uint64_t positionId,
                                 const crypto::PublicKey &owner)
    {
      if (!m_storage)
        return false;

      // Load position
      AmmPosition pos;
      std::vector<uint8_t> posData;
      if (!m_storage->getMeta("amm_pos_" + std::to_string(positionId), posData))
      {
        std::cerr << "AMM: position not found" << std::endl;
        return false;
      }
      cn::fromBinaryArray(pos, posData);

      if (pos.owner != owner)
      {
        std::cerr << "AMM: not the position owner" << std::endl;
        return false;
      }

      if (pos.liquidity == 0)
      {
        std::cerr << "AMM: position already empty" << std::endl;
        return false;
      }

      // Load pool
      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!m_storage->getMeta("amm_pool_" + std::to_string(pos.poolId), poolData))
      {
        std::cerr << "AMM: pool not found" << std::endl;
        return false;
      }
      cn::fromBinaryArray(pool, poolData);

      // Calculate share of reserves
      uint64_t shareA = (pos.liquidity * pool.reserveA) / pool.totalLiquidity;
      uint64_t shareB = (pos.liquidity * pool.reserveB) / pool.totalLiquidity;

      // Update pool
      pool.reserveA -= shareA;
      pool.reserveB -= shareB;
      pool.totalLiquidity -= pos.liquidity;

      cn::BinaryArray updatedBa = cn::toBinaryArray(pool);
      m_storage->putMeta("amm_pool_" + std::to_string(pos.poolId), updatedBa);

      // Return tokens to owner
      uint64_t balanceA = 0, balanceB = 0;
      m_storage->getBalance(owner, pool.tokenIdA, balanceA);
      m_storage->getBalance(owner, pool.tokenIdB, balanceB);
      m_storage->setBalance(owner, pool.tokenIdA, balanceA + shareA);
      m_storage->setBalance(owner, pool.tokenIdB, balanceB + shareB);

      // Zero out position
      pos.liquidity = 0;
      cn::BinaryArray posBa = cn::toBinaryArray(pos);
      m_storage->putMeta("amm_pos_" + std::to_string(positionId), posBa);

      std::cout << "AMM: liquidity removed pool=" << pos.poolId
                << " shareA=" << shareA << " shareB=" << shareB << std::endl;

      return true;
    }

    SwapResult Engine::swap(uint64_t poolId,
                            uint64_t tokenIdIn, uint64_t amountIn,
                            uint64_t minAmountOut,
                            const crypto::PublicKey &swapper)
    {
      SwapResult result;
      result.success = false;

      if (!m_storage)
      {
        result.error = "Storage not configured";
        return result;
      }

      // Load pool
      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!m_storage->getMeta("amm_pool_" + std::to_string(poolId), poolData))
      {
        result.error = "Pool not found";
        return result;
      }
      cn::fromBinaryArray(pool, poolData);

      if (!pool.active)
      {
        result.error = "Pool is inactive";
        return result;
      }

      // Determine reserves based on which token is being swapped in
      uint64_t reserveIn, reserveOut;
      uint64_t tokenIdOut;

      if (tokenIdIn == pool.tokenIdA)
      {
        reserveIn = pool.reserveA;
        reserveOut = pool.reserveB;
        tokenIdOut = pool.tokenIdB;
      }
      else if (tokenIdIn == pool.tokenIdB)
      {
        reserveIn = pool.reserveB;
        reserveOut = pool.reserveA;
        tokenIdOut = pool.tokenIdA;
      }
      else
      {
        result.error = "Token not in pool";
        return result;
      }

      // Calculate output amount
      uint64_t amountOut = calculateSwapAmount(reserveIn, reserveOut, amountIn, pool.feeBasisPoints);

      if (amountOut < minAmountOut)
      {
        result.error = "Slippage exceeded. Expected " + std::to_string(amountOut) +
                       ", minimum " + std::to_string(minAmountOut);
        return result;
      }

      // Verify swapper has sufficient balance
      uint64_t balance = 0;
      m_storage->getBalance(swapper, tokenIdIn, balance);
      if (balance < amountIn)
      {
        result.error = "Insufficient balance";
        return result;
      }

      // Deduct input from swapper
      m_storage->setBalance(swapper, tokenIdIn, balance - amountIn);

      // Credit output to swapper
      uint64_t outBalance = 0;
      m_storage->getBalance(swapper, tokenIdOut, outBalance);
      m_storage->setBalance(swapper, tokenIdOut, outBalance + amountOut);

      // Update pool reserves
      uint64_t fee = amountIn - (amountIn * (10000 - pool.feeBasisPoints) / 10000);

      if (tokenIdIn == pool.tokenIdA)
      {
        pool.reserveA += amountIn;
        pool.reserveB -= amountOut;
      }
      else
      {
        pool.reserveB += amountIn;
        pool.reserveA -= amountOut;
      }

      cn::BinaryArray updatedBa = cn::toBinaryArray(pool);
      m_storage->putMeta("amm_pool_" + std::to_string(poolId), updatedBa);

      result.success = true;
      result.amountIn = amountIn;
      result.amountOut = amountOut;
      result.fee = fee;
      result.poolId = poolId;

      std::cout << "AMM: swap pool=" << poolId
                << " in=" << amountIn << " tokenIdIn=" << tokenIdIn
                << " out=" << amountOut << " tokenIdOut=" << tokenIdOut
                << " fee=" << fee << std::endl;

      return result;
    }

    uint64_t Engine::getAmountOut(uint64_t poolId,
                                  uint64_t tokenIdIn, uint64_t amountIn) const
    {
      if (!m_storage)
        return 0;

      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!m_storage->getMeta("amm_pool_" + std::to_string(poolId), poolData))
        return 0;
      cn::fromBinaryArray(pool, poolData);

      uint64_t reserveIn, reserveOut;

      if (tokenIdIn == pool.tokenIdA)
      {
        reserveIn = pool.reserveA;
        reserveOut = pool.reserveB;
      }
      else if (tokenIdIn == pool.tokenIdB)
      {
        reserveIn = pool.reserveB;
        reserveOut = pool.reserveA;
      }
      else
      {
        return 0;
      }

      return calculateSwapAmount(reserveIn, reserveOut, amountIn, pool.feeBasisPoints);
    }

    uint64_t Engine::getAmountIn(uint64_t poolId,
                                 uint64_t tokenIdIn, uint64_t amountOut) const
    {
      if (!m_storage)
        return 0;

      AmmPool pool;
      std::vector<uint8_t> poolData;
      if (!m_storage->getMeta("amm_pool_" + std::to_string(poolId), poolData))
        return 0;
      cn::fromBinaryArray(pool, poolData);

      uint64_t reserveIn, reserveOut;

      if (tokenIdIn == pool.tokenIdA)
      {
        reserveIn = pool.reserveA;
        reserveOut = pool.reserveB;
      }
      else if (tokenIdIn == pool.tokenIdB)
      {
        reserveIn = pool.reserveB;
        reserveOut = pool.reserveA;
      }
      else
      {
        return 0;
      }

      if (reserveIn == 0 || reserveOut == 0 || amountOut >= reserveOut)
        return 0;

      // Reverse of the constant product formula
      uint64_t k = reserveIn * reserveOut;
      uint64_t newReserveOut = reserveOut - amountOut;
      uint64_t newReserveIn = k / newReserveOut;
      uint64_t amountInBeforeFee = newReserveIn - reserveIn;

      // Add fee back
      uint64_t amountIn = amountInBeforeFee * 10000 / (10000 - pool.feeBasisPoints);

      return amountIn;
    }

    std::vector<AmmPool> Engine::getPools() const
    {
      std::vector<AmmPool> result;
      if (!m_storage)
        return result;

      uint64_t id = 1;
      while (true)
      {
        AmmPool pool;
        std::vector<uint8_t> data;
        if (!m_storage->getMeta("amm_pool_" + std::to_string(id), data))
          break;
        if (cn::fromBinaryArray(pool, data) && pool.active)
          result.push_back(pool);
        ++id;
      }
      return result;
    }

    std::vector<AmmPool> Engine::getPoolsByToken(uint64_t tokenId) const
    {
      std::vector<AmmPool> result;
      auto all = getPools();
      for (const auto &pool : all)
      {
        if (pool.tokenIdA == tokenId || pool.tokenIdB == tokenId)
          result.push_back(pool);
      }
      return result;
    }

    std::vector<AmmPosition> Engine::getPositions(const crypto::PublicKey &owner) const
    {
      std::vector<AmmPosition> result;
      if (!m_storage)
        return result;

      uint64_t id = 1;
      while (true)
      {
        AmmPosition pos;
        std::vector<uint8_t> data;
        if (!m_storage->getMeta("amm_pos_" + std::to_string(id), data))
          break;
        if (cn::fromBinaryArray(pos, data) && pos.owner == owner && pos.liquidity > 0)
          result.push_back(pos);
        ++id;
      }
      return result;
    }

    void Engine::processBlock(const Block &block)
    {
      // Process AMM-related transactions from committed blocks
      // The actual swaps and liquidity changes are handled when
      // transactions are submitted and validated
    }
  }
}