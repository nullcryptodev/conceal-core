// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"
#include "Common/Math.h"

#include "CryptoNoteCore/CryptoNoteTools.h"

#include <boost/foreach.hpp>

namespace cn
{
  //  Difficulty for next main-chain block
  difficulty_type Blockchain::getDifficultyForNextBlock()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    std::vector<uint64_t> timestamps;
    std::vector<difficulty_type> cumulative_difficulties;

    size_t sz = blocksSize();
    uint8_t blockMajorVersion = get_block_major_version_for_height(static_cast<uint32_t>(sz));
    size_t window = m_currency.difficultyBlocksCountByBlockVersion(blockMajorVersion);
    size_t offset = sz - std::min(sz, window);
    if (offset == 0)
      ++offset;

    for (; offset < sz; ++offset)
    {
      cn::BlockHeaderPOD hdr = getBlockHeader(offset);
      timestamps.push_back(hdr.timestamp);
      cumulative_difficulties.push_back(hdr.cumulativeDifficulty);
    }

    uint64_t block_index = sz;
    uint8_t block_major_version = get_block_major_version_for_height(block_index + 1);

    difficulty_type currentDifficulty = calculateDifficulty(
        block_major_version, block_index, timestamps, cumulative_difficulties);

    return currentDifficulty == 0 ? 1 : currentDifficulty;
  }

  // Difficulty for an alternative chain
  difficulty_type Blockchain::get_next_difficulty_for_alternative_chain(
      const std::list<crypto::Hash> &alt_chain, const BlockEntry &bei)
  {
    std::vector<uint64_t> timestamps;
    std::vector<difficulty_type> cumulative_difficulties;

    size_t sz = blocksSize();
    uint8_t blockMajorVersion = get_block_major_version_for_height(static_cast<uint32_t>(sz));
    size_t window = m_currency.difficultyBlocksCountByBlockVersion(blockMajorVersion);

    if (alt_chain.size() < window)
    {
      collectDifficultyDataMainChain(alt_chain, bei, window, timestamps, cumulative_difficulties);
    }
    else
    {
      collectDifficultyDataAltChain(alt_chain, window, timestamps, cumulative_difficulties);
    }

    uint64_t block_index = sz;
    uint8_t block_major_version = get_block_major_version_for_height(block_index + 1);

    return calculateDifficulty(block_major_version, block_index,
                               timestamps, cumulative_difficulties);
  }

  // Difficulty data collection helpers
  void Blockchain::collectDifficultyDataMainChain(
      const std::list<crypto::Hash> &alt_chain, const BlockEntry &bei,
      size_t window, std::vector<uint64_t> &timestamps,
      std::vector<difficulty_type> &cumulative_difficulties)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    size_t main_chain_stop_offset = alt_chain.empty()
                                        ? bei.height
                                        : m_alternative_chains[alt_chain.front()].height;
    size_t main_chain_count = window - std::min(window, alt_chain.size());
    main_chain_count = std::min(main_chain_count, main_chain_stop_offset);
    size_t main_chain_start_offset = main_chain_stop_offset - main_chain_count;
    if (main_chain_start_offset == 0)
      ++main_chain_start_offset;

    for (; main_chain_start_offset < main_chain_stop_offset; ++main_chain_start_offset)
    {
      cn::BlockHeaderPOD hdr = getBlockHeader(main_chain_start_offset);
      timestamps.push_back(hdr.timestamp);
      cumulative_difficulties.push_back(hdr.cumulativeDifficulty);
    }

    for (const auto &it : alt_chain)
    {
      const BlockEntry &blockEntry = m_alternative_chains[it];
      timestamps.push_back(blockEntry.bl.timestamp);
      cumulative_difficulties.push_back(blockEntry.cumulative_difficulty);
    }
  }

  void Blockchain::collectDifficultyDataAltChain(
      const std::list<crypto::Hash> &alt_chain, size_t window,
      std::vector<uint64_t> &timestamps,
      std::vector<difficulty_type> &cumulative_difficulties)
  {
    size_t count = std::min(alt_chain.size(), window);
    timestamps.resize(count);
    cumulative_difficulties.resize(count);

    size_t idx = count - 1;
    size_t collected = 0;

    BOOST_REVERSE_FOREACH(auto it, alt_chain)
    {
      const BlockEntry &blockEntry = m_alternative_chains[it];
      timestamps[idx - collected] = blockEntry.bl.timestamp;
      cumulative_difficulties[idx - collected] = blockEntry.cumulative_difficulty;
      ++collected;
      if (collected >= window)
        break;
    }
  }

  // Difficulty calculation dispatch
  difficulty_type Blockchain::calculateDifficulty(
      uint8_t majorVersion, uint64_t blockIndex,
      const std::vector<uint64_t> &timestamps,
      const std::vector<difficulty_type> &cumulativeDifficulties)
  {
    if (majorVersion >= 8)
      return m_currency.nextDifficultyLWMA1(timestamps, cumulativeDifficulties, blockIndex);
    if (majorVersion >= 4)
      return m_currency.nextDifficultyLWMA3(timestamps, cumulativeDifficulties);
    return m_currency.nextDifficulty(majorVersion, blockIndex, timestamps, cumulativeDifficulties);
  }

  // Single block difficulty
  uint64_t Blockchain::blockDifficulty(size_t i)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    if (i >= blocksSize())
    {
      logger(logging::ERROR, logging::BRIGHT_RED)
          << "wrong block index i = " << i << " at Blockchain::block_difficulty()";
      return 0;
    }

    uint64_t diff_i = getBlockHeader(i).cumulativeDifficulty;
    if (i == 0)
      return diff_i;
    return diff_i - getBlockHeader(i - 1).cumulativeDifficulty;
  }

  difficulty_type Blockchain::difficultyAtHeight(uint64_t height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    BlockEntry entry, entry_prev;
    if (!loadBlockEntry(height, entry))
      return 0;
    if (height < 1)
      return entry.cumulative_difficulty;
    if (!loadBlockEntry(height - 1, entry_prev))
      return entry.cumulative_difficulty;
    return entry.cumulative_difficulty - entry_prev.cumulative_difficulty;
  }

  // MDBX block entry loader helper
  bool Blockchain::loadBlockEntry(uint64_t height, BlockEntry &entry)
  {
    cn::BinaryArray ba;
    if (!m_mdbxStorage->getBlockEntry(static_cast<uint32_t>(height), ba))
      return false;
    return fromBinaryArray(entry, ba);
  }

  // Cumulative block size limit
  uint64_t Blockchain::getCurrentCumulativeBlocksizeLimit() const
  {
    return m_current_block_cumul_sz_limit;
  }

  bool Blockchain::update_next_comulative_size_limit()
  {
    std::vector<size_t> sz;
    get_last_n_blocks_sizes(sz, m_currency.rewardBlocksWindow());

    uint64_t median = common::medianValue(sz);
    if (median <= m_currency.blockGrantedFullRewardZone())
      median = m_currency.blockGrantedFullRewardZone();

    m_current_block_cumul_sz_limit = median * 2;
    return true;
  }

  // Block size helpers
  bool Blockchain::getBackwardBlocksSize(size_t from_height, std::vector<size_t> &sz,
                                         size_t count)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    if (from_height >= blocksSize())
    {
      logger(logging::ERROR, logging::BRIGHT_RED)
          << "Internal error: get_backward_blocks_sizes called with from_height="
          << from_height << ", blockchain height = " << blocksSize();
      return false;
    }

    size_t start_offset = (from_height + 1) - std::min(from_height + 1, count);
    for (size_t i = start_offset; i <= from_height; ++i)
      sz.push_back(blocksAt(i).block_cumulative_size);

    return true;
  }

  bool Blockchain::get_last_n_blocks_sizes(std::vector<size_t> &sz, size_t count)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (blocksSize() == 0)
      return true;
    return getBackwardBlocksSize(blocksSize() - 1, sz, count);
  }

} // namespace cn