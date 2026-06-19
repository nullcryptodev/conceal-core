// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain/Blockchain.h"
#include "Blockchain/BlockchainFilter.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "../pow/pow_service.hpp"

#include "Common/PathHelpers.h"

#include <chrono>

#include "Logging/ILogger.h"

namespace cn
{

  // Constructor
  Blockchain::Blockchain(const Currency &currency, tx_memory_pool &tx_pool, logging::ILogger &logger, bool blockchainIndexesEnabled)
      : m_currency(currency),
        m_tx_pool(tx_pool),
        m_checkpoints(logger),
        m_upgradeDetectorV2(currency, this, BLOCK_MAJOR_VERSION_2, logger),
        m_upgradeDetectorV3(currency, this, BLOCK_MAJOR_VERSION_3, logger),
        m_upgradeDetectorV4(currency, this, BLOCK_MAJOR_VERSION_4, logger),
        m_upgradeDetectorV7(currency, this, BLOCK_MAJOR_VERSION_7, logger),
        m_upgradeDetectorV8(currency, this, BLOCK_MAJOR_VERSION_8, logger),
        m_blockchainIndexesEnabled(blockchainIndexesEnabled),
        m_sparseChainCacheValid(false),
        m_cachedSparseChainHeight(0),
        logger(logger, "Blockchain")
  {
    m_cachedEntries.reserve(MDBX_CACHE_SIZE);
    m_lastSparseChainUpdate = std::chrono::steady_clock::now();
  }

  // Block count / empty checks
  size_t Blockchain::blocksSize() const
  {
    return m_blockHashes.size();
  }

  bool Blockchain::blocksEmpty() const
  {
    return m_blockHashes.empty();
  }

  // Block access (with LRU cache)
  const Blockchain::BlockEntry &Blockchain::blocksAt(size_t i) const
  {
    // Check LRU cache first
    for (size_t c = 0; c < m_cachedEntries.size(); ++c)
      if (m_cachedEntries[c].height == i)
        return m_cachedEntries[c].entry;

    // Load from MDBX
    cn::BinaryArray ba;
    if (!m_mdbxStorage->getBlockEntry(static_cast<uint32_t>(i), ba))
      throw std::runtime_error("blocksAt: block not found at height " + std::to_string(i));

    BlockEntry entry;
    if (!cn::fromBinaryArray(entry, ba))
      throw std::runtime_error("blocksAt: failed to deserialise block at height " + std::to_string(i));

    // Insert into circular LRU cache
    if (m_cachedEntries.size() < MDBX_CACHE_SIZE)
    {
      m_cachedEntries.push_back({i, std::move(entry)});
      return m_cachedEntries.back().entry;
    }

    m_cachedEntries[m_cacheIndex] = {i, std::move(entry)};
    size_t insertedIndex = m_cacheIndex;
    m_cacheIndex = (m_cacheIndex + 1) % MDBX_CACHE_SIZE;
    return m_cachedEntries[insertedIndex].entry;
  }

  Blockchain::BlockEntry &Blockchain::blocksAt(size_t i)
  {
    return const_cast<BlockEntry &>(const_cast<const Blockchain *>(this)->blocksAt(i));
  }

  Blockchain::BlockEntry Blockchain::blocksBack() const
  {
    return blocksAt(blocksSize() - 1);
  }

  void Blockchain::blocksClear()
  {
    m_cachedEntries.clear();
    m_cachedEntries.reserve(MDBX_CACHE_SIZE);
    m_cacheIndex = 0;
    m_blockHashes.clear();
    m_hashToHeight.clear();
  }

  // Block header retrieval
  cn::BlockHeaderPOD Blockchain::getBlockHeader(uint32_t height) const
  {
    cn::BlockHeaderPOD hdr;
    if (m_mdbxStorage->getBlockHeader(height, hdr))
    {
      // Safety: if cumulative difficulty is zero but the block exists,
      // fall back to the full block entry for correct data
      if (hdr.cumulativeDifficulty == 0 && height < blocksSize())
        return headerFromBlockEntry(blocksAt(height));
      return hdr;
    }
    return headerFromBlockEntry(blocksAt(height));
  }

  cn::BlockHeaderPOD Blockchain::headerFromBlockEntry(const BlockEntry &entry)
  {
    cn::BlockHeaderPOD hdr;
    hdr.majorVersion = entry.bl.majorVersion;
    hdr.minorVersion = entry.bl.minorVersion;
    hdr.timestamp = entry.bl.timestamp;
    hdr.previousBlockHash = entry.bl.previousBlockHash;
    hdr.nonce = entry.bl.nonce;
    hdr.blockCumulativeSize = entry.block_cumulative_size;
    hdr.cumulativeDifficulty = entry.cumulative_difficulty;
    hdr.alreadyGeneratedCoins = entry.already_generated_coins;
    hdr.height = entry.height;
    return hdr;
  }

  // Transaction index resolution
  const Blockchain::TransactionEntry &Blockchain::transactionByIndex(TransactionIndex index)
  {
    return blocksAt(index.block).transactions[index.transaction];
  }

  // Block duplication check
  bool Blockchain::blockExistsInMainChain(const crypto::Hash &blockHash) const
  {
    return m_hashToHeight.count(blockHash) > 0;
  }

  // pushBlock — full validation path (block + transactions)
  bool Blockchain::pushBlock(const Block &blockData, const crypto::Hash &id,
                             block_verification_context &bvc, uint32_t height)
  {
    try
    {
      std::vector<Transaction> transactions;
      if (!loadTransactions(blockData, transactions, height))
      {
        bvc.m_verification_failed = true;
        return false;
      }

      if (!pushBlock(blockData, transactions, id, bvc))
      {
        saveTransactions(transactions, height);
        return false;
      }

      return true;
    }
    catch (const std::exception &e)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Exception in pushBlock: " << e.what();
      bvc.m_verification_failed = true;
      return false;
    }
  }

  bool Blockchain::pushBlock(const Block &blockData,
                             const std::vector<Transaction> &transactions,
                             const crypto::Hash &id,
                             block_verification_context &bvc)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    crypto::Hash blockHash = id;

    // Duplicate check
    if (blockExistsInMainChain(blockHash))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Block " << blockHash << " already exists";
      bvc.m_verification_failed = true;
      return false;
    }

    // Version check
    if (!checkBlockVersion(blockData, blockHash))
    {
      bvc.m_verification_failed = true;
      return false;
    }

    // Chain continuity
    if (blockData.previousBlockHash != getTailId())
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Wrong previousBlockHash";
      bvc.m_verification_failed = true;
      return false;
    }

    if (!check_block_timestamp_main(blockData))
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Invalid timestamp";
      bvc.m_verification_failed = true;
      return false;
    }

    // Difficulty & Proof of Work
    difficulty_type currentDifficulty = getDifficultyForNextBlock();
    if (!currentDifficulty)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Difficulty overhead";
      return false;
    }

    crypto::Hash proof_of_work = NULL_HASH;
    const auto powStart = std::chrono::steady_clock::now();
    if (m_checkpoints.is_in_checkpoint_zone(getCurrentBlockchainHeight()))
    {
      if (!m_checkpoints.check_block(getCurrentBlockchainHeight(), blockHash))
      {
        bvc.m_verification_failed = true;
        return false;
      }
    }
    else if (!m_currency.checkProofOfWork(m_cn_context, blockData, currentDifficulty, proof_of_work,
                                          static_cast<uint32_t>(getCurrentBlockchainHeight())))
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Block " << blockHash << " has too weak proof of work";
      bvc.m_verification_failed = true;
      return false;
    }
    const auto powEnd = std::chrono::steady_clock::now();
    const uint64_t powNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(powEnd - powStart).count());

    // Miner transaction pre-validation
    if (!prevalidate_miner_transaction(blockData, static_cast<uint32_t>(blocksSize())))
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Block " << blockHash << " failed prevalidation";
      bvc.m_verification_failed = true;
      return false;
    }

    // Build and validate the BlockEntry
    BlockEntry block;
    block.bl = blockData;
    block.height = static_cast<uint32_t>(blocksSize());

    crypto::Hash minerTransactionHash = getObjectHash(blockData.baseTransaction);
    block.transactions.resize(1 + transactions.size());
    block.transactions[0].tx = blockData.baseTransaction;

    TransactionIndex transactionIndex = {block.height, 0};
    pushTransaction(block, minerTransactionHash, transactionIndex);

    const auto txStart = std::chrono::steady_clock::now();

    // Process non-base transactions
    size_t cumulative_block_size = getObjectBinarySize(blockData.baseTransaction);
    uint64_t fee_summary = 0, interestSummary = 0;

    for (size_t i = 0; i < transactions.size(); ++i)
    {
      const crypto::Hash &tx_id = blockData.transactionHashes[i];
      block.transactions[1 + i].tx = transactions[i];

      if (!validateAndPushTransaction(block, transactions[i], tx_id, i,
                                      transactionIndex, cumulative_block_size,
                                      fee_summary, interestSummary, blockHash, bvc))
      {
        popTransaction(blockData.baseTransaction, minerTransactionHash);
        return false;
      }
    }

    // Cumulative size check
    if (!checkCumulativeBlockSize(blockHash, cumulative_block_size, block.height))
    {
      bvc.m_verification_failed = true;
      popTransactions(block, minerTransactionHash);
      return false;
    }

    // Miner transaction amount validation
    int64_t emissionChange = 0;
    uint64_t reward = 0;
    uint64_t already_generated_coins =
        (block.height == 0) ? 0 : getBlockHeader(block.height - 1).alreadyGeneratedCoins;

    if (!validate_miner_transaction(blockData, block.height, cumulative_block_size,
                                    already_generated_coins, fee_summary, reward, emissionChange))
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Block " << blockHash << " has invalid miner transaction";
      bvc.m_verification_failed = true;
      popTransactions(block, minerTransactionHash);
      return false;
    }

    const auto txEnd = std::chrono::steady_clock::now();
    const uint64_t txNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(txEnd - txStart).count());

    // Finalize and push
    block.block_cumulative_size = cumulative_block_size;
    block.cumulative_difficulty = currentDifficulty;
    block.already_generated_coins = already_generated_coins + emissionChange + interestSummary;
    if (block.height > 0)
      block.cumulative_difficulty += getBlockHeader(block.height - 1).cumulativeDifficulty;

    const auto dbStart = std::chrono::steady_clock::now();
    pushBlock(block);
    pushToDepositIndex(block, interestSummary);
    const auto dbEnd = std::chrono::steady_clock::now();
    const uint64_t dbNs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(dbEnd - dbStart).count());
    PowService::instance().onBlockValidated(block.height);
    PowService::instance().recordPushBlockTiming(powNs, txNs, dbNs);

    if (blockData.majorVersion >= 8 && (block.height % 256) == 0 &&
        PowService::instance().gpuPrefetchEnabled())
    {
      const PowVerifyMetrics& pm = PowService::instance().metrics();
      logger(logging::INFO) << "CN-GPU PoW metrics @ height " << block.height << ": gpuHits=" << pm.gpuHits
                            << " gpuMismatch=" << pm.gpuCacheMismatch << " cpuPowUsed=" << pm.cpuPowUsed << " jobsSubmitted=" << pm.jobsSubmitted
                            << " batches=" << pm.batchesExecuted << " avgBatch=" << pm.avgBatchSize
                            << " cpuFallback=" << pm.cpuFallbackCount;
    }

    logger(logging::DEBUGGING) << "+++++ Block added id:\t" << blockHash
                               << " PoW:\t" << proof_of_work
                               << " HEIGHT " << block.height
                               << " difficulty:\t" << currentDifficulty;

    bvc.m_added_to_main_chain = true;
    notifyUpgradeDetectorsBlockPushed();
    update_next_comulative_size_limit();
    return true;
  }

  // Single transaction validation during block push
  bool Blockchain::validateAndPushTransaction(
      BlockEntry &block, const Transaction &tx, const crypto::Hash &tx_id,
      size_t txIndex, TransactionIndex &transactionIndex,
      size_t &cumulative_block_size, uint64_t &fee_summary,
      uint64_t &interestSummary, const crypto::Hash &blockHash,
      block_verification_context &bvc)
  {
    size_t blob_size = toBinaryArray(tx).size();
    uint64_t fee = m_currency.getTransactionFee(tx, block.height);

    bool valid = true;
    if (block.bl.majorVersion == BLOCK_MAJOR_VERSION_1 && tx.version > TRANSACTION_VERSION_1)
    {
      valid = false;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "invalid version";
    }
    if (!checkTransactionInputs(tx, nullptr))
    {
      valid = false;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "wrong inputs";
    }
    if (!check_tx_outputs(tx, block.height))
    {
      valid = false;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "invalid output";
    }
    if (!checkDomainRegistrationFee(tx, block.height))
    {
      valid = false;
      logger(logging::INFO, logging::BRIGHT_WHITE) << "invalid domain registration fee";
    }

    if (!valid)
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Block " << blockHash << " has invalid transaction: " << tx_id;
      bvc.m_verification_failed = true;
      return false;
    }

    ++transactionIndex.transaction;
    pushTransaction(block, tx_id, transactionIndex);
    cumulative_block_size += blob_size;
    fee_summary += fee;
    interestSummary += m_currency.calculateTotalTransactionInterest(tx, block.height);
    return true;
  }

  // pushBlock — final BlockEntry push (MDBX)
  bool Blockchain::pushBlock(const BlockEntry &block)
  {
    return pushBlockMdbx(block);
  }

  bool Blockchain::pushBlockMdbx(const BlockEntry &block)
  {
    if (!m_mdbxStorage)
      return false;

    crypto::Hash blockHash = get_block_hash(block.bl);
    cn::BinaryArray ba = cn::toBinaryArray(block);
    uint32_t height = block.height;
    cn::BlockHeaderPOD hdr = headerFromBlockEntry(block);

    // Atomic write — single transaction, immediate commit
    m_mdbxStorage->pushCompleteBlock(height, blockHash, ba, hdr);

    // Build and store filter record for this block (pre-fork only)
    if (block.height < parameters::UPGRADE_HEIGHT_V9)
    {
      try
      {
        std::vector<Transaction> allTxs;
        for (uint32_t i = 1; i < block.transactions.size(); ++i)
          allTxs.push_back(block.transactions[i].tx);

        BlockFilterRecord filterRecord = buildBlockFilterRecord(
            block.bl, block.height, allTxs);
        m_mdbxStorage->storeBlockFilterRecord(block.height, filterRecord);
      }
      catch (const std::exception &e)
      {
        logger(logging::WARNING, logging::BRIGHT_YELLOW)
            << "Failed to build filter record for block " << block.height
            << ": " << e.what();
      }
    }

    // Update domain index for post-fork blocks
    if (block.height >= parameters::UPGRADE_HEIGHT_V9)
    {
      m_domainIndex.processBlock(block.bl, block.height);
      // Also process transactions for domain registrations/deletions
      for (uint32_t i = 1; i < block.transactions.size(); ++i)
      {
        m_domainIndex.extractFromTransaction(block.transactions[i].tx, block.height, i);
      }
    }

    // Update in-memory index
    if (height >= m_blockHashes.size())
    {
      m_blockHashes.push_back(blockHash);
      m_hashToHeight[blockHash] = height;
    }

    m_timestampIndex.add(block.bl.timestamp, blockHash);
    m_generatedTransactionsIndex.add(block.bl);

    // Update LRU cache if this block was already loaded during validation
    for (size_t c = 0; c < m_cachedEntries.size(); ++c)
    {
      if (m_cachedEntries[c].height == block.height)
      {
        m_cachedEntries[c].entry = block;
        break;
      }
    }
    return true;
  }

  // Block removal (pop)
  void Blockchain::popBlock(const crypto::Hash &blockHash)
  {
    if (blocksEmpty())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Attempt to pop block from empty blockchain.";
      return;
    }

    BlockEntry entry = blocksBack();
    uint32_t height = static_cast<uint32_t>(blocksSize());

    // Save transactions back to the pool
    std::vector<Transaction> transactions(entry.transactions.size() - 1);
    for (size_t i = 0; i < entry.transactions.size() - 1; ++i)
      transactions[i] = entry.transactions[1 + i].tx;

    saveTransactions(transactions, height);
    popTransactions(entry, getObjectHash(entry.bl.baseTransaction));

    // Remove from indices
    m_timestampIndex.remove(entry.bl.timestamp, blockHash);
    m_generatedTransactionsIndex.remove(entry.bl);
    m_depositIndex.popBlock();

    popBlockMdbx(blockHash);

    notifyUpgradeDetectorsBlockPopped();
  }

  void Blockchain::popBlockMdbx(const crypto::Hash &blockHash)
  {
    if (!m_mdbxStorage)
      return;

    if (blocksSize() == 0)
      return;

    uint32_t top = static_cast<uint32_t>(blocksSize() - 1);

    // removeCompleteBlock also removes the filter record
    m_mdbxStorage->removeCompleteBlock(top, blockHash);

    m_blockHashes.pop_back();
    m_hashToHeight.erase(blockHash);
  }

  bool Blockchain::removeLastBlock()
  {
    if (blocksSize() == 0)
      return false;

    uint32_t top = static_cast<uint32_t>(blocksSize() - 1);

    cn::BinaryArray ba;
    if (!m_mdbxStorage->getBlockEntry(top, ba))
      return false;

    BlockEntry entry;
    if (!cn::fromBinaryArray(entry, ba))
      return false;

    popTransactions(entry, getObjectHash(entry.bl.baseTransaction));
    crypto::Hash blockHash = get_block_hash(entry.bl);
    m_timestampIndex.remove(entry.bl.timestamp, blockHash);
    m_generatedTransactionsIndex.remove(entry.bl);

    popBlockMdbx(blockHash);
    return true;
  }

  // Transaction push / pop (indices, outputs, spent keys)
  bool Blockchain::pushTransaction(BlockEntry &block, const crypto::Hash &transactionHash,
                                   TransactionIndex transactionIndex)
  {
    auto result = m_transactionMap.insert(std::make_pair(transactionHash, transactionIndex));
    if (!result.second)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Duplicate transaction";
      return false;
    }

    TransactionEntry &transaction = block.transactions[transactionIndex.transaction];

    if (!checkMultisignatureInputsDiff(transaction.tx))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Double spending transaction";
      m_transactionMap.erase(transactionHash);
      return false;
    }

    // Mark key images as spent (with rollback on failure)
    if (!markKeyImagesSpent(transaction.tx, block.height))
    {
      m_transactionMap.erase(transactionHash);
      return false;
    }

    // Mark multisig outputs as spent
    markMultisigInputsSpent(transaction.tx);

    // Add new outputs to global output index
    transaction.m_global_output_indexes.resize(transaction.tx.outputs.size());
    for (uint32_t output = 0; output < transaction.tx.outputs.size(); ++output)
    {
      if (transaction.tx.outputs[output].target.type() == typeid(KeyOutput))
      {
        auto &amountOutputs = m_outputs[transaction.tx.outputs[output].amount];
        transaction.m_global_output_indexes[output] = static_cast<uint32_t>(amountOutputs.size());
        amountOutputs.push_back(std::make_pair<>(transactionIndex, output));
      }
      else if (transaction.tx.outputs[output].target.type() == typeid(MultisignatureOutput))
      {
        auto &amountOutputs = m_multisignatureOutputs[transaction.tx.outputs[output].amount];
        transaction.m_global_output_indexes[output] = static_cast<uint32_t>(amountOutputs.size());
        amountOutputs.push_back({transactionIndex, static_cast<uint16_t>(output), false});
      }
      else if (transaction.tx.outputs[output].target.type() == typeid(StandardPaymentOutput))
      {
        auto &amountOutputs = m_outputs[transaction.tx.outputs[output].amount];
        transaction.m_global_output_indexes[output] = static_cast<uint32_t>(amountOutputs.size());
        amountOutputs.push_back(std::make_pair<>(transactionIndex, output));
      }
      else if (transaction.tx.outputs[output].target.type() == typeid(MultisigPaymentOutput))
      {
        auto &amountOutputs = m_multisignatureOutputs[transaction.tx.outputs[output].amount];
        transaction.m_global_output_indexes[output] = static_cast<uint32_t>(amountOutputs.size());
        amountOutputs.push_back({transactionIndex, static_cast<uint16_t>(output), false});
      }
      // DomainRegistrationOutput and DomainDeletionOutput are not spendable,
      // so they don't need global output indexing
    }

    m_paymentIdIndex.add(transaction.tx);
    return true;
  }

  void Blockchain::popTransaction(const Transaction &transaction, const crypto::Hash &transactionHash)
  {
    TransactionIndex transactionIndex = m_transactionMap.at(transactionHash);

    // Remove outputs from global index
    for (size_t outputIndex = 0; outputIndex < transaction.outputs.size(); ++outputIndex)
    {
      const TransactionOutput &output = transaction.outputs[transaction.outputs.size() - 1 - outputIndex];
      if (output.target.type() == typeid(KeyOutput))
        popKeyOutput(output.amount, transactionIndex, transaction.outputs.size() - 1 - outputIndex);
      else if (output.target.type() == typeid(MultisignatureOutput))
        popMultisigOutput(output.amount, transactionIndex, transaction.outputs.size() - 1 - outputIndex);
      else if (output.target.type() == typeid(StandardPaymentOutput))
        popKeyOutput(output.amount, transactionIndex, transaction.outputs.size() - 1 - outputIndex);
      else if (output.target.type() == typeid(MultisigPaymentOutput))
        popMultisigOutput(output.amount, transactionIndex, transaction.outputs.size() - 1 - outputIndex);
      // Domain outputs don't have global output entries to pop
    }

    // Unmark key images
    for (auto &input : transaction.inputs)
    {
      if (input.type() == typeid(KeyInput))
        m_spent_keys.erase(::boost::get<KeyInput>(input).keyImage);
      else if (input.type() == typeid(MultisignatureInput))
      {
        const MultisignatureInput &in = ::boost::get<MultisignatureInput>(input);
        m_multisignatureOutputs[in.amount][in.outputIndex].isUsed = false;
      }
    }

    m_paymentIdIndex.remove(transaction);
    m_transactionMap.erase(transactionHash);
  }

  void Blockchain::popTransactions(const BlockEntry &block, const crypto::Hash &minerTransactionHash)
  {
    for (size_t i = 0; i < block.transactions.size() - 1; ++i)
      popTransaction(block.transactions[block.transactions.size() - 1 - i].tx,
                     block.bl.transactionHashes[block.transactions.size() - 2 - i]);
    popTransaction(block.bl.baseTransaction, minerTransactionHash);
  }

  // Key image / multisig helpers
  bool Blockchain::markKeyImagesSpent(const Transaction &tx, uint32_t blockHeight)
  {
    for (size_t i = 0; i < tx.inputs.size(); ++i)
    {
      if (tx.inputs[i].type() != typeid(KeyInput))
        continue;

      const auto &keyImage = ::boost::get<KeyInput>(tx.inputs[i]).keyImage;
      if (!m_spent_keys.insert(std::make_pair(keyImage, blockHeight)).second)
      {
        // Rollback previously inserted key images
        for (size_t j = 0; j < i; ++j)
          if (tx.inputs[i - 1 - j].type() == typeid(KeyInput))
            m_spent_keys.erase(::boost::get<KeyInput>(tx.inputs[i - 1 - j]).keyImage);
        return false;
      }
    }
    return true;
  }

  void Blockchain::markMultisigInputsSpent(const Transaction &tx)
  {
    for (const auto &inv : tx.inputs)
    {
      if (inv.type() == typeid(MultisignatureInput))
      {
        const MultisignatureInput &in = ::boost::get<MultisignatureInput>(inv);
        m_multisignatureOutputs[in.amount][in.outputIndex].isUsed = true;
      }
    }
  }

  void Blockchain::popKeyOutput(uint64_t amount, const TransactionIndex &txIndex, size_t outputIndex)
  {
    auto it = m_outputs.find(amount);
    if (it == m_outputs.end() || it->second.empty())
      return;
    if (it->second.back().first.block != txIndex.block ||
        it->second.back().first.transaction != txIndex.transaction)
      return;
    if (it->second.back().second != outputIndex)
      return;
    it->second.pop_back();
    if (it->second.empty())
      m_outputs.erase(it);
  }

  void Blockchain::popMultisigOutput(uint64_t amount, const TransactionIndex &txIndex, size_t outputIndex)
  {
    auto it = m_multisignatureOutputs.find(amount);
    if (it == m_multisignatureOutputs.end() || it->second.empty())
      return;
    if (it->second.back().isUsed)
      return;
    if (it->second.back().transactionIndex.block != txIndex.block ||
        it->second.back().transactionIndex.transaction != txIndex.transaction)
      return;
    if (it->second.back().outputIndex != outputIndex)
      return;
    it->second.pop_back();
    if (it->second.empty())
      m_multisignatureOutputs.erase(it);
  }

  // Upgrade detector notification helpers
  void Blockchain::notifyUpgradeDetectorsBlockPushed()
  {
    m_upgradeDetectorV2.blockPushed();
    m_upgradeDetectorV3.blockPushed();
    m_upgradeDetectorV4.blockPushed();
    m_upgradeDetectorV7.blockPushed();
    m_upgradeDetectorV8.blockPushed();
  }

  void Blockchain::notifyUpgradeDetectorsBlockPopped()
  {
    m_upgradeDetectorV2.blockPopped();
    m_upgradeDetectorV3.blockPopped();
    m_upgradeDetectorV4.blockPopped();
    m_upgradeDetectorV7.blockPopped();
    m_upgradeDetectorV8.blockPopped();
  }

  // Transaction pool I/O
  bool Blockchain::loadTransactions(const Block &block, std::vector<Transaction> &transactions,
                                    uint32_t height)
  {
    transactions.resize(block.transactionHashes.size());
    size_t transactionSize;
    uint64_t fee;

    for (size_t i = 0; i < block.transactionHashes.size(); ++i)
    {
      if (!m_tx_pool.take_tx(block.transactionHashes[i], transactions[i], transactionSize, fee))
      {
        // Rollback: return already-taken transactions to the pool
        tx_verification_context context;
        for (size_t j = 0; j < i; ++j)
          if (!m_tx_pool.add_tx(transactions[i - 1 - j], context, true, height))
            throw std::runtime_error("failed to return transaction to pool during rollback");
        return false;
      }
    }
    return true;
  }

  void Blockchain::saveTransactions(const std::vector<Transaction> &transactions, uint32_t height)
  {
    tx_verification_context context;
    for (size_t i = 0; i < transactions.size(); ++i)
      if (!m_tx_pool.add_tx(transactions[transactions.size() - 1 - i], context, true, height))
        throw std::runtime_error("failed to add transaction to pool");
  }

} // namespace cn