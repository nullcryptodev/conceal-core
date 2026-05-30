// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain/Blockchain.h"

#include "Common/Math.h"
#include "Common/PathHelpers.h"
#include "Common/StringTools.h"

#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"

#include "Serialization/SerializationTools.h"

#include <mdbx.h>
#include <iomanip>
#include <sstream>

namespace cn
{

  // ─── MDBX Storage Initialisation ───────────────────────────────────────

  bool Blockchain::initMdbxStorage(const std::string &config_folder)
  {
    logger(logging::INFO) << "Initializing MDBX storage backend...";
    m_mdbxStorage.reset(new CryptoNote::MDBXBlockchainStorage(
        PathHelpers::appendPath(config_folder, "mdbx_blocks")));

    m_cacheIndex = 0;
    m_cachedEntries.clear();
    m_cachedEntries.reserve(MDBX_CACHE_SIZE);

    return true;
  }

  // ─── Full Index Rebuild ───────────────────────────────────────────────

  void Blockchain::rebuildMdbxIndex()
  {
    logger(logging::INFO) << "Rebuilding in-memory index from MDBX...";

    m_blockHashes.clear();
    m_hashToHeight.clear();
    m_transactionMap.clear();
    m_spent_keys.clear();
    m_outputs.clear();
    m_multisignatureOutputs.clear();
    m_depositIndex = DepositIndex();
    m_timestampIndex.clear();
    m_generatedTransactionsIndex.clear();
    m_paymentIdIndex.clear();

    uint32_t topHeight = m_mdbxStorage->topBlockHeight();

    for (uint32_t h = 0; h <= topHeight; ++h)
    {
      cn::BinaryArray ba;
      if (!m_mdbxStorage->getBlockEntry(h, ba))
        continue;

      if (h % 10000 == 0)
        logger(logging::INFO, logging::BRIGHT_WHITE)
            << "Rebuilding MDBX index for Height " << h << " of " << topHeight;

      BlockEntry entry;
      if (!cn::fromBinaryArray(entry, ba))
        continue;

      crypto::Hash blockHash = get_block_hash(entry.bl);

      // Chain index
      m_blockHashes.push_back(blockHash);
      m_hashToHeight[blockHash] = h;

      // Timestamp index
      m_timestampIndex.add(entry.bl.timestamp, blockHash);

      // Generated transactions index
      m_generatedTransactionsIndex.add(entry.bl);

      // Process transactions
      for (uint32_t t = 0; t < entry.transactions.size(); ++t)
      {
        const auto &tx_entry = entry.transactions[t];
        crypto::Hash txHash = getObjectHash(tx_entry.tx);
        TransactionIndex txIdx = {h, static_cast<uint16_t>(t)};
        m_transactionMap.insert(std::make_pair(txHash, txIdx));

        // Payment ID index
        m_paymentIdIndex.add(tx_entry.tx);

        // Spent key images
        for (const auto &input : tx_entry.tx.inputs)
        {
          if (input.type() == typeid(KeyInput))
          {
            const auto &ki = boost::get<KeyInput>(input);
            m_spent_keys.insert(std::make_pair(ki.keyImage, h));
          }
          else if (input.type() == typeid(MultisignatureInput))
          {
            const auto &msInput = boost::get<MultisignatureInput>(input);
            m_multisignatureOutputs[msInput.amount][msInput.outputIndex].isUsed = true;
          }
        }

        // Outputs index
        for (uint32_t o = 0; o < tx_entry.tx.outputs.size(); ++o)
        {
          const auto &out = tx_entry.tx.outputs[o];
          if (out.target.type() == typeid(KeyOutput))
          {
            m_outputs[out.amount].push_back(std::make_pair<>(txIdx, o));
          }
          else if (out.target.type() == typeid(MultisignatureOutput))
          {
            MultisignatureOutputUsage usage = {txIdx, static_cast<uint16_t>(o), false};
            m_multisignatureOutputs[out.amount].push_back(usage);
          }
        }
      }

      // Deposit index
      uint64_t interest = 0;
      for (const auto &tx : entry.transactions)
        interest += m_currency.calculateTotalTransactionInterest(tx.tx, h);
      pushToDepositIndex(entry, interest);
    }

    logger(logging::INFO) << "Loaded " << m_blockHashes.size() << " blocks from MDBX (full rebuild)";
  }

  // ─── MDBX Init ────────────────────────────────────────────────────────

  bool Blockchain::initMdbx(bool load_existing)
  {
    auto mdbxStart = std::chrono::steady_clock::now();

    if (load_existing && m_mdbxStorage->topBlockHeight() > 0)
    {
      logger(logging::INFO) << "Loading in-memory structures from MDBX...";

      // Clear all in-memory caches
      m_blockHashes.clear();
      m_hashToHeight.clear();
      m_transactionMap.clear();
      m_spent_keys.clear();
      m_outputs.clear();
      m_multisignatureOutputs.clear();
      m_depositIndex = DepositIndex();
      m_timestampIndex.clear();
      m_generatedTransactionsIndex.clear();
      m_paymentIdIndex.clear();

      // Always rebuild from MDBX — single source of truth
      rebuildMdbxIndex();
    }

    auto mdbxEnd = std::chrono::steady_clock::now();
    auto mdbxMs = std::chrono::duration_cast<std::chrono::milliseconds>(mdbxEnd - mdbxStart).count();
    logger(logging::INFO) << "MDBX initialization took " << mdbxMs << " ms";
    return true;
  }

  // ─── Genesis & Validation Helpers ─────────────────────────────────────

  bool Blockchain::ensureGenesisBlock()
  {
    if (!blocksEmpty())
      return true;

    logger(logging::INFO, logging::BRIGHT_WHITE) << "Blockchain not loaded, generating genesis block.";

    try
    {
      block_verification_context bvc = boost::value_initialized<block_verification_context>();
      pushBlock(m_currency.genesisBlock(), get_block_hash(m_currency.genesisBlock()), bvc, 0);
      if (bvc.m_verification_failed)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to add genesis block to blockchain";
        return false;
      }
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error creating genesis block";
      return false;
    }

    return true;
  }

  bool Blockchain::validateGenesisBlock()
  {
    return true;
  }

  bool Blockchain::initUpgradeDetectors()
  {
    try
    {
      if (!m_upgradeDetectorV2.init() || !m_upgradeDetectorV3.init() ||
          !m_upgradeDetectorV4.init() || !m_upgradeDetectorV7.init() ||
          !m_upgradeDetectorV8.init())
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to initialize one or more upgrade detectors";
        return false;
      }
    }
    catch (const std::exception &e)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error initializing upgrade detectors: " << e.what();
      return false;
    }
    return true;
  }

  void Blockchain::logInitSummary()
  {
    logger(logging::INFO, logging::BRIGHT_GREEN)
        << "Blockchain initialized. Local Height: " << blocksSize() - 1 << " [MDBX backend]";
  }

  // ─── Main Init ────────────────────────────────────────────────────────

  bool Blockchain::init(const std::string &config_folder, bool load_existing, bool testnet)
  {
    try
    {
      m_testnet = testnet;
      m_checkpoints.set_testnet(testnet);
      std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

      if (!config_folder.empty() && !tools::create_directories_if_necessary(config_folder))
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Failed to create data directory: " << m_config_folder;
        return false;
      }

      m_config_folder = config_folder;

      // ── Storage backend ────────────────────────────────────────────
      if (!initMdbxStorage(config_folder))
        return false;
      if (!initMdbx(load_existing))
        return false;

      // ── Genesis ────────────────────────────────────────────────────
      if (!ensureGenesisBlock())
        return false;

      if (!validateGenesisBlock())
        return false;

      // ── Checkpoints ────────────────────────────────────────────────
      try
      {
        uint32_t lastValidCheckpointHeight = 0;
        if (!checkCheckpoints(lastValidCheckpointHeight))
        {
          logger(logging::WARNING, logging::BRIGHT_YELLOW)
              << "Invalid checkpoint. Rollback blockchain to last valid checkpoint at height "
              << lastValidCheckpointHeight;
          rollbackBlockchainTo(lastValidCheckpointHeight);
        }
      }
      catch (const std::exception &)
      {
        logger(logging::ERROR, logging::BRIGHT_RED) << "Error checking/rolling back checkpoints";
        return false;
      }

      // ── Upgrade detectors ──────────────────────────────────────────
      if (!initUpgradeDetectors())
        return false;

      // ── Final setup ────────────────────────────────────────────────
      update_next_comulative_size_limit();
      logInitSummary();

      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error initializing blockchain";
      return false;
    }
  }

  // ─── Deinit ───────────────────────────────────────────────────────────

  bool Blockchain::deinit()
  {
    bool cacheStored = false, indicesStored = true;

    try
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Saving blockchain state before shutdown...";

      cacheStored = storeMdbxCache();

      if (m_blockchainIndexesEnabled)
      {
        logger(logging::INFO) << "Saving blockchain indices...";
        indicesStored = storeBlockchainIndices();
      }

      if (m_mdbxStorage)
      {
        logger(logging::INFO) << "Flushing MDBX...";
        m_mdbxStorage->flush();
      }

      logger(logging::INFO, logging::BRIGHT_GREEN) << "Blockchain state saved.";
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error occurred during blockchain deinit";
    }

    assert(m_messageQueueList.empty());
    return cacheStored && indicesStored;
  }

  // ─── Cache Rebuild ────────────────────────────────────────────────────

  bool Blockchain::rebuildCache()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding cache";

    std::chrono::steady_clock::time_point timePoint = std::chrono::steady_clock::now();
    try
    {
      m_blockHashes.clear();
      m_hashToHeight.clear();
      m_transactionMap.clear();
      m_spent_keys.clear();
      m_outputs.clear();
      m_multisignatureOutputs.clear();

      for (uint32_t b = 0; b < blocksSize(); ++b)
      {
        if (b % 1000 == 0)
          logger(logging::INFO, logging::BRIGHT_WHITE)
              << "Rebuilding Cache for Height " << b << " of " << blocksSize();

        const BlockEntry &block = blocksAt(b);
        crypto::Hash blockHash = get_block_hash(block.bl);

        m_blockHashes.push_back(blockHash);
        m_hashToHeight[blockHash] = b;

        uint64_t interest = 0;
        for (uint32_t t = 0; t < block.transactions.size(); ++t)
        {
          const TransactionEntry &transaction = block.transactions[t];
          crypto::Hash transactionHash = getObjectHash(transaction.tx);
          TransactionIndex transactionIndex = {b, static_cast<uint16_t>(t)};
          m_transactionMap.insert(std::make_pair(transactionHash, transactionIndex));

          for (auto &i : transaction.tx.inputs)
          {
            if (i.type() == typeid(KeyInput))
              m_spent_keys.insert(std::make_pair(boost::get<KeyInput>(i).keyImage, b));
            else if (i.type() == typeid(MultisignatureInput))
            {
              const auto &out = boost::get<MultisignatureInput>(i);
              m_multisignatureOutputs[out.amount][out.outputIndex].isUsed = true;
            }
          }

          for (uint32_t o = 0; o < transaction.tx.outputs.size(); ++o)
          {
            const auto &out = transaction.tx.outputs[o];
            if (out.target.type() == typeid(KeyOutput))
              m_outputs[out.amount].push_back(std::make_pair<>(transactionIndex, o));
            else if (out.target.type() == typeid(MultisignatureOutput))
            {
              MultisignatureOutputUsage usage = {transactionIndex, static_cast<uint16_t>(o), false};
              m_multisignatureOutputs[out.amount].push_back(usage);
            }
          }

          interest += m_currency.calculateTotalTransactionInterest(transaction.tx, b);
        }

        pushToDepositIndex(block, interest);
      }

      std::chrono::duration<double> duration = std::chrono::steady_clock::now() - timePoint;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Rebuilding internal structures took: " << duration.count();
      logger(logging::INFO, logging::BRIGHT_GREEN) << "Cache rebuilt successfully";
      return true;
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error rebuilding cache";
      return false;
    }
    catch (...)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Unknown error rebuilding cache";
      return false;
    }
  }

  // ─── Store Cache ──────────────────────────────────────────────────────

  bool Blockchain::storeCache()
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return storeMdbxCache();
  }

  bool Blockchain::storeMdbxCache()
  {
    if (!m_mdbxStorage)
      return false;

    m_mdbxStorage->flush();
    return true;
  }

  // ─── Filter database access ───────────────────────────────────────────

  bool Blockchain::getBlockFilterRecord(uint32_t height, BlockFilterRecord &record) const
  {
    if (!m_mdbxStorage)
      return false;
    return m_mdbxStorage->getBlockFilterRecord(height, record);
  }

  bool Blockchain::hasBlockFilterRecord(uint32_t height) const
  {
    if (!m_mdbxStorage)
      return false;
    return m_mdbxStorage->hasBlockFilterRecord(height);
  }

  // ─── Reset ────────────────────────────────────────────────────────────

  bool Blockchain::resetAndSetGenesisBlock(const Block &b)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    blocksClear();
    m_transactionMap.clear();
    m_spent_keys.clear();
    m_alternative_chains.clear();
    m_outputs.clear();
    m_paymentIdIndex.clear();
    m_timestampIndex.clear();
    m_generatedTransactionsIndex.clear();
    m_orthanBlocksIndex.clear();

    m_blockHashes.clear();
    m_hashToHeight.clear();
    m_cachedEntries.clear();
    m_cacheIndex = 0;

    block_verification_context bvc = boost::value_initialized<block_verification_context>();
    addNewBlock(b, bvc);
    return bvc.m_added_to_main_chain && !bvc.m_verification_failed;
  }

} // namespace cn