// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"

#include "Common/Math.h"
#include "Common/StringTools.h"

#include "CryptoNoteCore/CryptoNoteTools.h"

#include "CryptoNoteProtocol/CryptoNoteProtocolDefinitions.h"

namespace cn
{
  // Sparse chain building
  std::vector<crypto::Hash> Blockchain::buildSparseChain()
  {
    uint32_t currentHeight;
    crypto::Hash tailId;
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
      assert(blocksSize() != 0);
      currentHeight = static_cast<uint32_t>(blocksSize());
      tailId = getTailId();
    }

    // Check if cached sparse chain is still fresh
    if (isSparseChainCacheValid(currentHeight))
    {
      std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);
      return m_cachedSparseChain;
    }

    std::vector<crypto::Hash> result = doBuildSparseChainUnlocked(tailId);
    updateSparseChainCache(result, currentHeight);
    return result;
  }

  std::vector<crypto::Hash> Blockchain::buildSparseChain(const crypto::Hash &startBlockId)
  {
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
      if (!haveBlock(startBlockId))
        return {};
    }
    return doBuildSparseChainUnlocked(startBlockId);
  }

  std::vector<crypto::Hash> Blockchain::doBuildSparseChain(const crypto::Hash &startBlockId) const
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return doBuildSparseChainMdbx(startBlockId);
  }

  std::vector<crypto::Hash> Blockchain::doBuildSparseChainUnlocked(const crypto::Hash &startBlockId) const
  {
    return doBuildSparseChain(startBlockId);
  }

  // Sparse chain — MDBX path
  std::vector<crypto::Hash> Blockchain::doBuildSparseChainMdbx(const crypto::Hash &startBlockId) const
  {
    if (m_blockHashes.empty())
      return {};

    // Check if startBlockId is in the main chain
    size_t height = 0;
    if (findHashHeight(startBlockId, height))
      return buildSparseFromHeightMdbx(height);

    // Not in main chain — must be in alternative chains
    assert(m_alternative_chains.count(startBlockId) > 0);

    // Walk up alternative chain to find the main chain ancestor
    std::vector<crypto::Hash> altPart;
    crypto::Hash ancestor = walkAlternativeChainToAncestor(startBlockId, altPart);

    // Find ancestor in main chain
    size_t ancestorHeight = 0;
    bool ancestorFound = findHashHeight(ancestor, ancestorHeight);
    assert(ancestorFound);

    // Build sparse chain from ancestor in main chain
    std::vector<crypto::Hash> mainPart = buildSparseFromHeightMdbx(ancestorHeight);

    // Combine: alternate chain (powers of 2) + main chain
    std::vector<crypto::Hash> result;
    result.reserve(32);
    for (size_t i = 0; i < altPart.size(); i *= 2)
      result.push_back(altPart[i]);
    if (result.back() != altPart.back())
      result.push_back(altPart.back());

    result.insert(result.end(), mainPart.begin(), mainPart.end());
    return result;
  }

  std::vector<crypto::Hash> Blockchain::buildSparseFromHeightMdbx(size_t height) const
  {
    std::vector<crypto::Hash> chain;
    chain.reserve(32);
    chain.push_back(m_blockHashes[height]); // top block

    if (height > 0)
    {
      uint32_t step = 1;
      while (step < height)
      {
        chain.push_back(m_blockHashes[height - step]);
        step *= 2;
      }
      chain.push_back(m_blockHashes[0]); // genesis last
    }
    return chain;
  }

  bool Blockchain::findHashHeight(const crypto::Hash &hash, size_t &height) const
  {
    for (size_t i = 0; i < m_blockHashes.size(); ++i)
    {
      if (m_blockHashes[i] == hash)
      {
        height = i;
        return true;
      }
    }
    return false;
  }

  crypto::Hash Blockchain::walkAlternativeChainToAncestor(const crypto::Hash &startId,
                                                          std::vector<crypto::Hash> &altPart) const
  {
    crypto::Hash currentId = startId;
    crypto::Hash ancestor;
    while (m_alternative_chains.count(currentId))
    {
      altPart.push_back(currentId);
      ancestor = m_alternative_chains.at(currentId).bl.previousBlockHash;
      currentId = ancestor;
    }
    return ancestor;
  }

  // Sparse chain cache management
  bool Blockchain::isSparseChainCacheValid(uint32_t currentHeight) const
  {
    std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       now - m_lastSparseChainUpdate)
                       .count();
    uint32_t heightDelta = currentHeight > m_cachedSparseChainHeight
                               ? currentHeight - m_cachedSparseChainHeight
                               : m_cachedSparseChainHeight - currentHeight;

    return m_sparseChainCacheValid &&
           elapsed < SPARSE_CHAIN_CACHE_DURATION_SECONDS &&
           heightDelta < SPARSE_CHAIN_CACHE_BLOCK_DELTA &&
           !m_cachedSparseChain.empty();
  }

  void Blockchain::updateSparseChainCache(const std::vector<crypto::Hash> &chain,
                                          uint32_t currentHeight)
  {
    std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);
    m_cachedSparseChain = chain;
    m_cachedSparseChainHeight = currentHeight;
    m_lastSparseChainUpdate = std::chrono::steady_clock::now();
    m_sparseChainCacheValid = true;
  }

  void Blockchain::invalidateSparseChainCache()
  {
    std::lock_guard<std::mutex> cacheLock(m_sparseChainCacheMutex);
    m_sparseChainCacheValid = false;
    m_cachedSparseChain.clear();
  }

  std::vector<crypto::Hash> Blockchain::getCachedSparseChain()
  {
    return buildSparseChain();
  }

  // Block retrieval for sync
  bool Blockchain::getBlocks(uint32_t start_offset, uint32_t count,
                             std::list<Block> &blocks, std::list<Transaction> &txs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (start_offset >= blocksSize())
      return false;

    for (size_t i = start_offset; i < start_offset + count && i < blocksSize(); i++)
    {
      blocks.push_back(blocksAt(i).bl);
      std::list<crypto::Hash> missed_ids;
      getTransactions(blocksAt(i).bl.transactionHashes, txs, missed_ids);
      if (!missed_ids.empty())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "have missed transactions in own block in main blockchain";
        return false;
      }
    }
    return true;
  }

  bool Blockchain::getBlocks(uint32_t start_offset, uint32_t count, std::list<Block> &blocks)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (start_offset >= blocksSize())
      return false;

    for (uint32_t i = start_offset; i < start_offset + count && i < blocksSize(); i++)
      blocks.push_back(blocksAt(i).bl);
    return true;
  }

  std::vector<crypto::Hash> Blockchain::getBlockIds(uint32_t startHeight, uint32_t maxCount)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);

    if (m_blockHashes.empty() || startHeight >= m_blockHashes.size())
      return {};

    uint32_t end = std::min(startHeight + maxCount,
                            static_cast<uint32_t>(m_blockHashes.size()));
    return std::vector<crypto::Hash>(m_blockHashes.begin() + startHeight,
                                     m_blockHashes.begin() + end);
  }

  // Chain supplement finding (sync divergence point)
  uint32_t Blockchain::findBlockchainSupplement(const std::vector<crypto::Hash> &qblock_ids)
  {
    assert(!qblock_ids.empty());
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return findBlockchainSupplementInternal(qblock_ids);
  }

  std::vector<crypto::Hash> Blockchain::findBlockchainSupplement(
      const std::vector<crypto::Hash> &remoteBlockIds, size_t maxCount,
      uint32_t &totalBlockCount, uint32_t &startBlockIndex)
  {
    assert(!remoteBlockIds.empty());

    uint32_t startIndex = 0, totalCount = 0;
    {
      std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
      totalCount = getCurrentBlockchainHeight();
      startIndex = findBlockchainSupplement(remoteBlockIds);
    }

    totalBlockCount = totalCount;
    startBlockIndex = startIndex;
    return getBlockIds(startIndex, static_cast<uint32_t>(maxCount));
  }

  uint32_t Blockchain::findBlockchainSupplementInternal(
      const std::vector<crypto::Hash> &qblock_ids) const
  {
    uint32_t currentHeight = static_cast<uint32_t>(blocksSize());
    uint32_t bestMatch = 0;

    for (const auto &blockId : qblock_ids)
    {
      auto it = m_hashToHeight.find(blockId);
      if (it != m_hashToHeight.end())
      {
        uint32_t height = it->second;
        if (height + 1 > bestMatch && height < currentHeight)
          bestMatch = height + 1;
      }
    }

    return bestMatch;
  }

  // Timestamp-based block lookup
  bool Blockchain::getLowerBound(uint64_t timestamp, uint64_t startOffset, uint32_t &height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    assert(startOffset < m_blockHashes.size());

    for (size_t i = startOffset; i < m_blockHashes.size(); ++i)
    {
      cn::BlockHeaderPOD hdr = getBlockHeader(static_cast<uint32_t>(i));
      if (hdr.timestamp >= timestamp - m_currency.blockFutureTimeLimit())
      {
        height = static_cast<uint32_t>(i);
        return true;
      }
    }
    return false;
  }

  // P2P getObjects handler
  bool Blockchain::handleGetObjects(NOTIFY_REQUEST_GET_OBJECTS::request &arg,
                                    NOTIFY_RESPONSE_GET_OBJECTS::request &rsp)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    rsp.current_blockchain_height = getCurrentBlockchainHeight();

    // Fetch requested blocks
    std::list<Block> blocks;
    getBlocks(arg.blocks, blocks, rsp.missed_ids);

    for (const auto &bl : blocks)
    {
      std::list<crypto::Hash> missed_tx_id;
      std::list<Transaction> txs;
      getTransactions(bl.transactionHashes, txs, missed_tx_id);

      if (!missed_tx_id.empty())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Internal error: missed "
                                                    << missed_tx_id.size() << " transactions for block "
                                                    << get_block_hash(bl);
        return false;
      }

      rsp.blocks.push_back(block_complete_entry());
      block_complete_entry &e = rsp.blocks.back();
      e.block = common::asString(toBinaryArray(bl));
      for (const Transaction &tx : txs)
        e.txs.push_back(common::asString(toBinaryArray(tx)));
    }

    // Fetch requested transactions
    std::list<Transaction> txs;
    getTransactions(arg.txs, txs, rsp.missed_ids);
    for (const auto &tx : txs)
      rsp.txs.push_back(common::asString(toBinaryArray(tx)));

    return true;
  }

  // Block addition (entry point from P2P / miner)
  bool Blockchain::addNewBlock(const Block &bl_, block_verification_context &bvc)
  {
    try
    {
      Block bl = bl_;
      crypto::Hash id;
      if (!get_block_hash(bl, id))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to get block hash";
        bvc.m_verification_failed = true;
        return false;
      }

      bool add_result;
      {
        std::lock_guard<decltype(m_tx_pool)> poolLock(m_tx_pool);
        std::lock_guard<decltype(m_blockchain_lock)> bcLock(m_blockchain_lock);

        if (haveBlock(id))
        {
          logger(logging::TRACE) << "block already exists";
          bvc.m_already_exists = true;
          return false;
        }

        if (bl.previousBlockHash != getTailId())
        {
          bvc.m_added_to_main_chain = false;
          add_result = handle_alternative_block(bl, id, bvc);
        }
        else
        {
          auto height = static_cast<uint32_t>(blocksSize());
          add_result = pushBlock(bl, id, bvc, height);

          if (add_result)
          {
            sendMessage(BlockchainMessage(NewBlockMessage(id)));
          }
        }
      }

      if (add_result && bvc.m_added_to_main_chain)
        m_observerManager.notify(&IBlockchainStorageObserver::blockchainUpdated);

      return add_result;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error adding new block to blockchain";
      bvc.m_verification_failed = true;
      return false;
    }
  }

} // namespace cn