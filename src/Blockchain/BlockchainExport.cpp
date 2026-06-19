// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"
#include "Blockchain/BlockchainIndicesSerializer.h"
#include "Common/PathHelpers.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Serialization/BinarySerializationTools.h"

namespace cn
{
  //  Debug printing
  void Blockchain::print_blockchain(uint64_t start_index, uint64_t end_index)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    if (start_index >= blocksSize())
    {
      logger(logging::INFO, logging::BRIGHT_WHITE)
          << "Wrong start index: " << start_index
          << ", max index: " << blocksSize() - 1;
      return;
    }

    std::stringstream ss;
    for (size_t i = start_index; i < blocksSize() && i != end_index; ++i)
    {
      cn::BlockHeaderPOD hdr = getBlockHeader(i);
      crypto::Hash blockHash = m_blockHashes[i];

      ss << "height " << i
         << ", timestamp " << hdr.timestamp
         << ", cumul_dif " << hdr.cumulativeDifficulty
         << ", cumul_size " << hdr.blockCumulativeSize
         << "\nid\t\t" << common::podToHex(blockHash)
         << "\ndifficulty\t\t" << blockDifficulty(i)
         << ", nonce " << hdr.nonce
         << ", tx_count " << blocksAt(i).bl.transactionHashes.size()
         << ENDL;
    }

    logger(logging::INFO) << "Blockchain:\n"
                          << ss.str();
  }

  void Blockchain::print_blockchain_index(bool print_all)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    if (!print_all)
    {
      uint32_t height = getCurrentBlockchainHeight() - 1;
      crypto::Hash id = m_blockHashes[height];
      logger(logging::INFO) << "Current blockchain index: id: " << common::podToHex(id)
                            << " height: " << height;
      return;
    }

    logger(logging::INFO) << "Blockchain indexes:";
    size_t height = 0;
    for (const auto &id : m_blockHashes)
      logger(logging::INFO) << "id: " << common::podToHex(id) << " height: " << height++;
  }

  void Blockchain::print_blockchain_outs(const std::string &file)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    std::stringstream ss;
    for (const auto &entry : m_outputs)
    {
      const uint64_t amount = entry.first;
      const auto &outputs = entry.second;
      if (outputs.empty())
        continue;

      ss << "amount: " << amount << ENDL;
      for (size_t i = 0; i < outputs.size(); ++i)
        ss << "\t" << getObjectHash(transactionByIndex(outputs[i].first).tx)
           << ": " << outputs[i].second << ENDL;
    }

    if (common::saveStringToFile(file, ss.str()))
      logger(logging::INFO, logging::BRIGHT_WHITE)
          << "Outputs index written to file: " << file;
    else
      logger(logging::WARNING, logging::BRIGHT_YELLOW)
          << "Failed to write outputs index to file: " << file;
  }

  // Blockchain explorer indices
  bool Blockchain::storeBlockchainIndices()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(logging::INFO, logging::BRIGHT_WHITE) << "Saving blockchain indices";

    BlockchainIndicesSerializer ser(*this, getTailId(), logger.getLogger());

    try
    {
      if (!storeToBinaryFile(ser, PathHelpers::appendPath(
                                      m_config_folder,
                                      m_currency.blockchinIndicesFileName())))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to save blockchain indices";
        return false;
      }
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to save blockchain indices";
      return false;
    }

    return true;
  }

  bool Blockchain::loadBlockchainIndices()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(logging::INFO, logging::BRIGHT_WHITE) << "Loading blockchain indices";

    BlockchainIndicesSerializer loader(*this, get_block_hash(blocksBack().bl),
                                       logger.getLogger());

    bool needsRebuild = false;
    try
    {
      loadFromBinaryFile(loader, PathHelpers::appendPath(
                                     m_config_folder,
                                     m_currency.blockchinIndicesFileName()));
      needsRebuild = !loader.loaded();
    }
    catch (const std::exception &)
    {
      needsRebuild = true;
    }

    if (needsRebuild)
      rebuildBlockchainIndices();

    return true;
  }

  void Blockchain::rebuildBlockchainIndices()
  {
    logger(logging::WARNING, logging::BRIGHT_YELLOW)
        << "No valid blockchain indices found, rebuilding...";

    auto timePoint = std::chrono::steady_clock::now();

    m_paymentIdIndex.clear();
    m_timestampIndex.clear();
    m_generatedTransactionsIndex.clear();

    for (uint32_t b = 0; b < blocksSize(); ++b)
    {
      if (b % 1000 == 0)
        logger(logging::INFO, logging::BRIGHT_WHITE)
            << "Rebuilding indices for Height " << b << " of " << blocksSize();

      cn::BlockHeaderPOD hdr = getBlockHeader(b);
      crypto::Hash hash = m_blockHashes[b];

      m_timestampIndex.add(hdr.timestamp, hash);

      const BlockEntry &block = blocksAt(b);
      m_generatedTransactionsIndex.add(block.bl);

      for (const auto &txEntry : block.transactions)
        m_paymentIdIndex.add(txEntry.tx);
    }

    auto duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - timePoint);
    logger(logging::INFO, logging::BRIGHT_WHITE)
        << "Rebuilding blockchain indices took: " << duration.count() << "s";
  }

} // namespace cn