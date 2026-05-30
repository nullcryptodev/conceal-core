// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"

#include "Common/ShuffleGenerator.h"

#include "CryptoNoteCore/CryptoNoteTools.h"

#include "Rpc/CoreRpcServerCommandsDefinitions.h"

namespace cn
{
  // Chain state queries (height, tail, block lookup)
  uint32_t Blockchain::getCurrentBlockchainHeight()
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return static_cast<uint32_t>(m_blockHashes.size());
  }

  crypto::Hash Blockchain::getTailId()
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return m_blockHashes.empty() ? NULL_HASH : m_blockHashes.back();
  }

  crypto::Hash Blockchain::getTailId(uint32_t &height)
  {
    assert(!blocksEmpty());
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    height = getCurrentBlockchainHeight() - 1;
    return getTailId();
  }

  crypto::Hash Blockchain::getBlockIdByHeight(uint32_t height)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    assert(height < m_blockHashes.size());
    return m_blockHashes[height];
  }

  bool Blockchain::getBlockByHash(const crypto::Hash &blockHash, Block &b)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    // Try main chain
    auto it = m_hashToHeight.find(blockHash);
    if (it != m_hashToHeight.end())
    {
      b = blocksAt(it->second).bl;
      return true;
    }

    // Try alternative chains
    auto altIt = m_alternative_chains.find(blockHash);
    if (altIt != m_alternative_chains.end())
    {
      b = altIt->second.bl;
      return true;
    }
    return false;
  }

  bool Blockchain::getBlockHeight(const crypto::Hash &blockId, uint32_t &blockHeight)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lock(m_blockchain_lock);
    auto it = m_hashToHeight.find(blockId);
    if (it != m_hashToHeight.end())
    {
      blockHeight = it->second;
      return true;
    }
    return false;
  }

  bool Blockchain::haveBlock(const crypto::Hash &id)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    if (m_hashToHeight.count(id))
      return true;
    return m_alternative_chains.count(id) > 0;
  }

  bool Blockchain::isBlockInMainChain(const crypto::Hash &blockId) const
  {
    return m_hashToHeight.count(blockId) > 0;
  }

  // Block metadata queries (timestamp, coins, difficulty at height)
  uint64_t Blockchain::getBlockTimestamp(uint32_t height)
  {
    assert(height < blocksSize());
    return getBlockHeader(height).timestamp;
  }

  uint64_t Blockchain::getCoinsInCirculation()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (blocksEmpty())
      return 0;
    return getBlockHeader(blocksSize() - 1).alreadyGeneratedCoins;
  }

  uint64_t Blockchain::coinsEmittedAtHeight(uint64_t height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    cn::BinaryArray ba;
    if (!m_mdbxStorage->getBlockEntry(static_cast<uint32_t>(height), ba))
      return 0;
    BlockEntry entry;
    if (!cn::fromBinaryArray(entry, ba))
      return 0;
    return entry.already_generated_coins;
  }

  uint8_t Blockchain::get_block_major_version_for_height(uint64_t height) const
  {
    if (height > m_upgradeDetectorV8.upgradeHeight())
      return m_upgradeDetectorV8.targetVersion();
    if (height > m_upgradeDetectorV7.upgradeHeight())
      return m_upgradeDetectorV7.targetVersion();
    if (height > m_upgradeDetectorV4.upgradeHeight())
      return m_upgradeDetectorV4.targetVersion();
    if (height > m_upgradeDetectorV3.upgradeHeight())
      return m_upgradeDetectorV3.targetVersion();
    if (height > m_upgradeDetectorV2.upgradeHeight())
      return m_upgradeDetectorV2.targetVersion();
    return BLOCK_MAJOR_VERSION_1;
  }

  // Transaction queries
  bool Blockchain::haveTransaction(const crypto::Hash &id)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return m_transactionMap.find(id) != m_transactionMap.end();
  }

  size_t Blockchain::getTotalTransactions()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_transactionMap.size();
  }

  bool Blockchain::getBlockContainingTransaction(const crypto::Hash &txId, crypto::Hash &blockId, uint32_t &blockHeight)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto it = m_transactionMap.find(txId);
    if (it == m_transactionMap.end())
      return false;
    blockHeight = blocksAt(it->second.block).height;
    blockId = getBlockIdByHeight(blockHeight);
    return true;
  }

  bool Blockchain::getTransactionOutputGlobalIndexes(const crypto::Hash &tx_id, std::vector<uint32_t> &indexs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto it = m_transactionMap.find(tx_id);
    if (it == m_transactionMap.end())
    {
      logger(logging::WARNING, logging::YELLOW) << "warning: get_tx_outputs_gindexs failed to find transaction with id = " << tx_id;
      return false;
    }
    const TransactionEntry &tx = transactionByIndex(it->second);
    if (tx.m_global_output_indexes.empty())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "internal error: global indexes for transaction " << tx_id << " is empty";
      return false;
    }
    indexs.assign(tx.m_global_output_indexes.begin(), tx.m_global_output_indexes.end());
    return true;
  }

  bool Blockchain::getTransactionsWithOutputGlobalIndexes(
      const std::vector<crypto::Hash> &txs_ids,
      std::list<crypto::Hash> &missed_txs,
      std::vector<std::pair<Transaction, std::vector<uint32_t>>> &txs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    for (const auto &tx_id : txs_ids)
    {
      auto it = m_transactionMap.find(tx_id);
      if (it == m_transactionMap.end())
      {
        missed_txs.push_back(tx_id);
        continue;
      }
      const TransactionEntry &tx = transactionByIndex(it->second);
      if (tx.m_global_output_indexes.empty())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Internal error: global indexes for transaction " << tx_id << " is empty";
        return false;
      }
      txs.emplace_back(tx.tx, tx.m_global_output_indexes);
    }
    return true;
  }

  // Spent key image queries
  bool Blockchain::have_tx_keyimg_as_spent(const crypto::KeyImage &key_im)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    return m_spent_keys.find(key_im) != m_spent_keys.end();
  }

  // Multisignature output queries
  bool Blockchain::get_out_by_msig_gindex(uint64_t amount, uint64_t gindex, MultisignatureOutput &out)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto it = m_multisignatureOutputs.find(amount);
    if (it == m_multisignatureOutputs.end() || it->second.size() <= gindex)
      return false;

    const auto &msigUsage = it->second[gindex];
    const auto &targetOut = transactionByIndex(msigUsage.transactionIndex).tx.outputs[msigUsage.outputIndex].target;
    if (targetOut.type() != typeid(MultisignatureOutput))
      return false;

    out = boost::get<MultisignatureOutput>(targetOut);
    return true;
  }

  bool Blockchain::getMultisigOutputReference(const MultisignatureInput &txInMultisig,
                                              std::pair<crypto::Hash, size_t> &outputReference)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    auto amountIter = m_multisignatureOutputs.find(txInMultisig.amount);
    if (amountIter == m_multisignatureOutputs.end() || amountIter->second.size() <= txInMultisig.outputIndex)
      return false;

    const auto &outputIndex = amountIter->second[txInMultisig.outputIndex];
    const Transaction &outputTransaction =
        blocksAt(outputIndex.transactionIndex.block).transactions[outputIndex.transactionIndex.transaction].tx;
    outputReference.first = getObjectHash(outputTransaction);
    outputReference.second = outputIndex.outputIndex;
    return true;
  }

  // Block metadata by hash (coins generated, block size)
  bool Blockchain::getAlreadyGeneratedCoins(const crypto::Hash &hash, uint64_t &generatedCoins)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    auto it = m_hashToHeight.find(hash);
    if (it != m_hashToHeight.end())
    {
      generatedCoins = getBlockHeader(it->second).alreadyGeneratedCoins;
      return true;
    }

    // Try alternative chains
    auto altIt = m_alternative_chains.find(hash);
    if (altIt != m_alternative_chains.end())
    {
      generatedCoins = altIt->second.already_generated_coins;
      return true;
    }
    return false;
  }

  bool Blockchain::getBlockSize(const crypto::Hash &hash, size_t &size)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    auto it = m_hashToHeight.find(hash);
    if (it != m_hashToHeight.end())
    {
      size = getBlockHeader(it->second).blockCumulativeSize;
      return true;
    }

    auto altIt = m_alternative_chains.find(hash);
    if (altIt != m_alternative_chains.end())
    {
      size = altIt->second.block_cumulative_size;
      return true;
    }
    return false;
  }

  // Index queries (payment ID, timestamp, generated transactions)
  bool Blockchain::getGeneratedTransactionsNumber(uint32_t height, uint64_t &generatedTransactions)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_generatedTransactionsIndex.find(height, generatedTransactions);
  }

  bool Blockchain::getBlockIdsByTimestamp(uint64_t timestampBegin, uint64_t timestampEnd,
                                          uint32_t blocksNumberLimit, std::vector<crypto::Hash> &hashes,
                                          uint32_t &blocksNumberWithinTimestamps)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_timestampIndex.find(timestampBegin, timestampEnd, blocksNumberLimit, hashes, blocksNumberWithinTimestamps);
  }

  bool Blockchain::getTransactionIdsByPaymentId(const crypto::Hash &paymentId,
                                                std::vector<crypto::Hash> &transactionHashes)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_paymentIdIndex.find(paymentId, transactionHashes);
  }

  // Random output selection (ring signature mixins)
  size_t Blockchain::find_end_of_allowed_index(
      const std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    if (amount_outs.empty())
      return 0;

    size_t i = amount_outs.size();
    do
    {
      --i;
      if (amount_outs[i].first.block + m_currency.minedMoneyUnlockWindow() <= getCurrentBlockchainHeight())
        return i + 1;
    } while (i != 0);
    return 0;
  }

  bool Blockchain::add_out_to_get_random_outs(
      std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs,
      COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount &result_outs,
      uint64_t amount, size_t i)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    const Transaction &tx = transactionByIndex(amount_outs[i].first).tx;

    if (!(tx.outputs.size() > amount_outs[i].second))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "internal error: transaction out index=" << amount_outs[i].second
                                                  << " exceeds outputs size=" << tx.outputs.size();
      return false;
    }
    if (tx.outputs[amount_outs[i].second].target.type() != typeid(KeyOutput))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "unknown tx out type";
      return false;
    }
    if (!is_tx_spendtime_unlocked(tx.unlockTime))
      return false;

    COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry &oen =
        *result_outs.outs.insert(result_outs.outs.end(),
                                 COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry());
    oen.global_amount_index = static_cast<uint32_t>(i);
    oen.out_key = boost::get<KeyOutput>(tx.outputs[amount_outs[i].second].target).key;
    return true;
  }

  bool Blockchain::getRandomOutsByAmount(
      const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request &req,
      COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response &res)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    for (uint64_t amount : req.amounts)
    {
      COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount &result_outs =
          *res.outs.insert(res.outs.end(),
                           COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount());
      result_outs.amount = amount;

      auto it = m_outputs.find(amount);
      if (it == m_outputs.end())
        continue; // No outputs of this amount exist

      auto &amount_outs = it->second;
      size_t up_index_limit = find_end_of_allowed_index(amount_outs);

      if (up_index_limit > amount_outs.size())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "internal error: up_index_limit=" << up_index_limit
                                                    << " > amount_outs.size()=" << amount_outs.size();
        return false;
      }

      if (up_index_limit > 0)
      {
        ShuffleGenerator<size_t, crypto::random_engine<size_t>> generator(up_index_limit);
        for (uint64_t j = 0; j < up_index_limit && result_outs.outs.size() < req.outs_count; ++j)
          add_out_to_get_random_outs(amount_outs, result_outs, amount, generator());
      }
    }
    return true;
  }

  // Checkpoint zone
  bool Blockchain::isInCheckpointZone(const uint32_t height) const
  {
    return m_checkpoints.is_in_checkpoint_zone(height);
  }

} // namespace cn