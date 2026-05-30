// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"

namespace cn
{
  //  Alternative chain queries
  bool Blockchain::getAlternativeBlocks(std::list<Block> &blocks)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    for (const auto &alt_bl : m_alternative_chains)
      blocks.push_back(alt_bl.second.bl);
    return true;
  }

  uint32_t Blockchain::getAlternativeBlocksCount()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return static_cast<uint32_t>(m_alternative_chains.size());
  }

  bool Blockchain::getOrphanBlockIdsByHeight(uint32_t height, std::vector<crypto::Hash> &blockHashes)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_orthanBlocksIndex.find(height, blockHashes);
  }

  //  Rollback helpers
  bool Blockchain::rollbackBlockchainTo(uint32_t height)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
      logger(logging::INFO, logging::BRIGHT_YELLOW) << "Rolling back blockchain to height " << height;

      if (height >= blocksSize())
      {
        logger(logging::WARNING, logging::BRIGHT_YELLOW) << "Requested rollback to height " << height
                                                         << " >= current height " << blocksSize();
        return true;
      }

      while (blocksSize() > height + 1)
      {
        if (!removeLastBlock())
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to remove last block";
          return false;
        }
      }

      logger(logging::INFO, logging::BRIGHT_GREEN) << "Blockchain successfully rolled back to height: " << height
                                                   << " Synchronization will resume";
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error rolling back blockchain";
      return false;
    }
  }

  bool Blockchain::rollback_blockchain_switching(const std::list<Block> &original_chain,
                                                 size_t rollback_height)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      // Pop blocks down to the rollback point
      for (size_t i = blocksSize() - 1; i >= rollback_height; i--)
        popBlock(get_block_hash(blocksBack().bl));

      // Re-push the original chain blocks
      auto height = static_cast<uint32_t>(rollback_height - 1);
      for (const auto &bl : original_chain)
      {
        block_verification_context bvc = boost::value_initialized<block_verification_context>();
        if (!pushBlock(bl, get_block_hash(bl), bvc, ++height) || !bvc.m_added_to_main_chain)
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "PANIC!!! failed to re-add block during chain switch rollback!";
          return false;
        }
      }

      logger(logging::INFO, logging::BRIGHT_WHITE) << "Rollback success.";
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error during blockchain rollback";
      return false;
    }
  }

  //  Previous block lookup (shared by alternative block handling)
  bool Blockchain::findPreviousBlockHeight(const crypto::Hash &prevHash,
                                           uint32_t &height, bool &inMainChain)
  {
    auto it = m_hashToHeight.find(prevHash);
    if (it != m_hashToHeight.end())
    {
      height = it->second;
      inMainChain = true;
      return true;
    }
    inMainChain = false;
    return false;
  }

  //  Alternative block handling
  bool Blockchain::handle_alternative_block(const Block &b, const crypto::Hash &id,
                                            block_verification_context &bvc,
                                            bool sendNewAlternativeBlockMessage)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      // ── Basic validation ──────────────────────────────────────────────
      auto block_height = get_block_height(b);
      if (block_height == 0)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Block with id: " << common::podToHex(id)
                                                    << " (as alternative) has wrong miner transaction";
        bvc.m_verification_failed = true;
        return false;
      }

      m_checkpoints.load_checkpoints_from_dns();

      if (!m_checkpoints.is_alternative_block_allowed(getCurrentBlockchainHeight(), block_height))
      {
        logger(logging::DEBUGGING) << "Block with id: " << id
                                   << " can't be accepted for alternative chain, block height: "
                                   << block_height << " blockchain height: " << getCurrentBlockchainHeight();
        bvc.m_verification_failed = true;
        return false;
      }

      if (!checkBlockVersion(b, id))
      {
        bvc.m_verification_failed = true;
        return false;
      }

      size_t cumulativeSize;
      if (!getBlockCumulativeSize(b, cumulativeSize))
        logger(logging::DEBUGGING) << "Block with id: " << id << " has unknown transactions, cumulative size imprecise";

      if (!checkCumulativeBlockSize(id, cumulativeSize, block_height))
      {
        bvc.m_verification_failed = true;
        return false;
      }

      // ── Find where this block connects ────────────────────────────────
      uint32_t mainPrevHeight = 0;
      bool mainPrev = false;
      findPreviousBlockHeight(b.previousBlockHash, mainPrevHeight, mainPrev);
      const auto it_prev = m_alternative_chains.find(b.previousBlockHash);

      if (it_prev == m_alternative_chains.end() && !mainPrev)
      {
        bvc.m_marked_as_orphaned = true;
        logger(logging::INFO, logging::BRIGHT_RED) << "Block recognized as orphaned and rejected, id = " << id;
        return true;
      }

      // ── Build the alternative chain and timestamp vector ──────────────
      blocks_ext_by_hash::iterator alt_it = it_prev;
      std::list<crypto::Hash> alt_chain;
      std::vector<uint64_t> timestamps;

      while (alt_it != m_alternative_chains.end())
      {
        alt_chain.push_front(alt_it->first);
        timestamps.push_back(alt_it->second.bl.timestamp);
        alt_it = m_alternative_chains.find(alt_it->second.bl.previousBlockHash);
      }

      if (!alt_chain.empty())
      {
        const BlockEntry &bei = m_alternative_chains[alt_chain.front()];
        if (!(blocksSize() > bei.height))
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "main blockchain wrong height";
          return false;
        }

        crypto::Hash h = NULL_HASH;
        get_block_hash(blocksAt(bei.height - 1).bl, h);
        if (!(h == bei.bl.previousBlockHash))
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "alternative chain has wrong connection to main chain";
          return false;
        }

        complete_timestamps_vector(bei.height - 1, timestamps);
      }
      else
      {
        if (!mainPrev)
        {
          logger(logging::ERROR, logging::BRIGHT_RED) << "internal error: no main chain predecessor found";
          return false;
        }
        complete_timestamps_vector(mainPrevHeight, timestamps);
      }

      // ── Timestamp validation ──────────────────────────────────────────
      if (!check_block_timestamp(timestamps, b))
      {
        logger(logging::INFO, logging::BRIGHT_RED) << "Block with id: " << id
                                                   << " for alternative chain has invalid timestamp: " << b.timestamp;
        bvc.m_verification_failed = true;
        return false;
      }

      // ── Build the BlockEntry ──────────────────────────────────────────
      BlockEntry bei = boost::value_initialized<BlockEntry>();
      bei.bl = b;
      bei.height = alt_chain.empty() ? mainPrevHeight + 1 : it_prev->second.height + 1;

      bool is_a_checkpoint;
      if (!m_checkpoints.check_block(bei.height, id, is_a_checkpoint))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Checkpoint validation failure";
        bvc.m_verification_failed = true;
        return false;
      }

      // ── Difficulty & PoW ──────────────────────────────────────────────
      m_is_in_checkpoint_zone = false;
      difficulty_type current_diff = get_next_difficulty_for_alternative_chain(alt_chain, bei);
      if (!current_diff)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "DIFFICULTY OVERHEAD";
        return false;
      }

      crypto::Hash proof_of_work = NULL_HASH;
      if (!m_currency.checkProofOfWork(m_cn_context, bei.bl, current_diff, proof_of_work, bei.height))
      {
        logger(logging::INFO, logging::BRIGHT_RED) << "Block with id: " << id
                                                   << " has insufficient proof of work: " << proof_of_work
                                                   << " expected difficulty: " << current_diff;
        bvc.m_verification_failed = true;
        return false;
      }

      // ── Miner transaction validation ──────────────────────────────────
      if (!prevalidate_miner_transaction(b, bei.height))
      {
        logger(logging::INFO, logging::BRIGHT_RED) << "Block with id: " << common::podToHex(id)
                                                   << " (as alternative) has wrong miner transaction.";
        bvc.m_verification_failed = true;
        return false;
      }

      // ── Finalize and insert ───────────────────────────────────────────
      bei.cumulative_difficulty = alt_chain.empty()
                                      ? blocksAt(mainPrevHeight).cumulative_difficulty
                                      : it_prev->second.cumulative_difficulty;
      bei.cumulative_difficulty += current_diff;

      auto i_res = m_alternative_chains.insert(blocks_ext_by_hash::value_type(id, bei));
      if (!i_res.second)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Insertion of new alternative block failed — already exists";
        return false;
      }

      m_orthanBlocksIndex.add(bei.bl);
      alt_chain.push_back(i_res.first->first);

      // ── Decide: switch to this chain, store as alternative, or reject ─
      if (is_a_checkpoint)
      {
        logger(logging::INFO, logging::BRIGHT_GREEN) << "###### REORGANIZE on height: "
                                                     << m_alternative_chains[alt_chain.front()].height
                                                     << " of " << blocksSize() - 1
                                                     << ", checkpoint found in alternative chain on height " << bei.height;

        bool r = switch_to_alternative_blockchain(alt_chain, true);
        if (r)
        {
          bvc.m_added_to_main_chain = true;
          bvc.m_switched_to_alt_chain = true;
        }
        else
          bvc.m_verification_failed = true;
        return r;
      }

      if (blocksBack().cumulative_difficulty < bei.cumulative_difficulty)
      {
        logger(logging::INFO, logging::BRIGHT_GREEN) << "###### REORGANIZE on height: "
                                                     << m_alternative_chains[alt_chain.front()].height
                                                     << " of " << blocksSize() - 1
                                                     << " with cum_difficulty " << blocksBack().cumulative_difficulty
                                                     << " alternative blockchain size: " << alt_chain.size()
                                                     << " with cum_difficulty " << bei.cumulative_difficulty;

        bool r = switch_to_alternative_blockchain(alt_chain, false);
        if (r)
        {
          bvc.m_added_to_main_chain = true;
          bvc.m_switched_to_alt_chain = true;
        }
        else
          bvc.m_verification_failed = true;
        return r;
      }

      // Store as alternative
      logger(logging::INFO, logging::BRIGHT_BLUE) << "----- BLOCK ADDED AS ALTERNATIVE ON HEIGHT " << bei.height
                                                  << " id:\t" << id
                                                  << " PoW:\t" << proof_of_work
                                                  << " difficulty:\t" << current_diff;

      if (sendNewAlternativeBlockMessage)
        sendMessage(BlockchainMessage(NewAlternativeBlockMessage(id)));
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error handling alternative block";
      bvc.m_verification_failed = true;
      return false;
    }
  }

  //  Chain switching (reorganisation)
  bool Blockchain::switch_to_alternative_blockchain(const std::list<crypto::Hash> &alt_chain,
                                                    bool discard_disconnected_chain)
  {
    try
    {
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      if (alt_chain.empty())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "switch_to_alternative_blockchain: empty chain passed";
        return false;
      }

      uint32_t split_height = m_alternative_chains[alt_chain.front()].height;
      if (!(blocksSize() > split_height))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "switch_to_alternative_blockchain: blockchain size "
                                                    << blocksSize() << " <= split height " << split_height;
        return false;
      }

      // ── Verify transaction coverage ────────────────────────────────────
      if (!verifyAlternativeChainTransactions(alt_chain, split_height))
        return false;

      // ── Verify block versions ──────────────────────────────────────────
      for (const auto &hash : alt_chain)
      {
        const Block &b = m_alternative_chains[hash].bl;
        if (!checkBlockVersion(b, get_block_hash(b)))
          return false;
      }

      // ── Pop main chain blocks down to split point ─────────────────────
      std::list<Block> disconnected_chain;
      for (size_t i = blocksSize() - 1; i >= split_height; i--)
      {
        Block b = blocksAt(i).bl;
        popBlock(get_block_hash(b));
        disconnected_chain.push_front(b);
      }

      // ── Push alternative chain blocks ─────────────────────────────────
      uint32_t height = split_height - 1;
      for (auto alt_ch_iter = alt_chain.begin(); alt_ch_iter != alt_chain.end(); ++alt_ch_iter)
      {
        const auto &ch_ent = *alt_ch_iter;
        block_verification_context bvc = boost::value_initialized<block_verification_context>();
        const Block &b = m_alternative_chains[ch_ent].bl;

        if (!pushBlock(b, get_block_hash(b), bvc, ++height) || !bvc.m_added_to_main_chain)
        {
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Failed to switch to alternative blockchain";
          rollback_blockchain_switching(disconnected_chain, split_height);

          // Clean up remaining alternative blocks
          m_orthanBlocksIndex.remove(b);
          m_alternative_chains.erase(ch_ent);
          for (auto it = ++alt_ch_iter; it != alt_chain.end(); ++it)
          {
            const Block &bl = m_alternative_chains[*it].bl;
            m_orthanBlocksIndex.remove(bl);
            m_alternative_chains.erase(*it);
          }
          return false;
        }
      }

      // ── Optionally preserve the disconnected chain ────────────────────
      if (!discard_disconnected_chain)
      {
        for (const auto &old_ch_ent : disconnected_chain)
        {
          block_verification_context bvc = boost::value_initialized<block_verification_context>();
          if (!handle_alternative_block(old_ch_ent, get_block_hash(old_ch_ent), bvc, false))
          {
            logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to push ex-main chain blocks to alternative chain";
            rollback_blockchain_switching(disconnected_chain, split_height);
            return false;
          }
        }
      }

      // ── Build chain switch message ────────────────────────────────────
      std::vector<crypto::Hash> blocksFromCommonRoot;
      blocksFromCommonRoot.reserve(alt_chain.size() + 1);

      const Block &firstAltBlock = m_alternative_chains[alt_chain.front()].bl;
      blocksFromCommonRoot.push_back(firstAltBlock.previousBlockHash);

      for (const auto &ch_ent : alt_chain)
      {
        const Block &bl = m_alternative_chains[ch_ent].bl;
        blocksFromCommonRoot.push_back(get_block_hash(bl));
        m_orthanBlocksIndex.remove(bl);
        m_alternative_chains.erase(ch_ent);
      }

      sendMessage(BlockchainMessage(ChainSwitchMessage(std::move(blocksFromCommonRoot))));

      logger(logging::INFO, logging::BRIGHT_GREEN) << "Successfully reorganized on height: " << split_height
                                                   << ", new blockchain size: " << blocksSize();
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error during blockchain switching";
      return false;
    }
  }

  //  Transaction verification for chain switching
  bool Blockchain::verifyAlternativeChainTransactions(
      const std::list<crypto::Hash> &alt_chain, uint32_t split_height)
  {
    // Collect transaction hashes from the main chain segment being replaced
    std::unordered_set<crypto::Hash> mainChainTxHashes;
    for (size_t i = blocksSize() - 1; i >= split_height; i--)
    {
      const Block &b = blocksAt(i).bl;
      mainChainTxHashes.insert(b.transactionHashes.begin(), b.transactionHashes.end());
    }

    // Check every main chain transaction exists in the alternative chain
    for (const auto &hash : alt_chain)
    {
      const Block &b = m_alternative_chains[hash].bl;
      for (const auto &tx_hash : b.transactionHashes)
        mainChainTxHashes.erase(tx_hash);
    }

    if (!mainChainTxHashes.empty())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Alternative chain lacks transaction "
                                                  << common::podToHex(*mainChainTxHashes.begin())
                                                  << " from main chain, rejected";
      return false;
    }

    return true;
  }

} // namespace cn