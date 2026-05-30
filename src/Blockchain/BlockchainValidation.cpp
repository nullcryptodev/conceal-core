// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"
#include "CheckTxOutputsVisitor.h"

#include "Common/Math.h"

#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"

namespace cn
{
  //  Transaction input validation (public interface)
  bool Blockchain::checkTransactionInputs(const Transaction &tx, BlockInfo &maxUsedBlock)
  {
    return checkTransactionInputs(tx, maxUsedBlock.height, maxUsedBlock.id) &&
           check_tx_outputs(tx, maxUsedBlock.height);
  }

  bool Blockchain::checkTransactionInputs(const Transaction &tx, BlockInfo &maxUsedBlock,
                                          BlockInfo &lastFailed)
  {
    BlockInfo tail;

    // If maxUsedBlock is empty, this is a fresh check
    if (maxUsedBlock.empty())
    {
      // Skip if we already know this tx fails at the current height
      if (!lastFailed.empty() &&
          getCurrentBlockchainHeight() > lastFailed.height &&
          getBlockIdByHeight(lastFailed.height) == lastFailed.id)
        return false;

      if (!checkTransactionInputs(tx, maxUsedBlock.height, maxUsedBlock.id, &tail))
      {
        lastFailed = tail;
        return false;
      }
      return true;
    }

    // maxUsedBlock is set — validate it's still relevant
    if (maxUsedBlock.height >= getCurrentBlockchainHeight())
      return false;

    if (getBlockIdByHeight(maxUsedBlock.height) != maxUsedBlock.id)
    {
      // Chain has changed — check if we already know this fails
      if (lastFailed.id == getBlockIdByHeight(lastFailed.height))
        return false;

      // Re-validate (small chance tx became valid again)
      if (!checkTransactionInputs(tx, maxUsedBlock.height, maxUsedBlock.id, &tail))
      {
        lastFailed = tail;
        return false;
      }
    }

    return true;
  }

  bool Blockchain::checkTransactionInputs(const Transaction &tx,
                                          uint32_t &max_used_block_height,
                                          crypto::Hash &max_used_block_id,
                                          BlockInfo *tail)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    if (tail)
      tail->id = getTailId(tail->height);

    if (!checkTransactionInputs(tx, &max_used_block_height))
      return false;

    if (max_used_block_height >= blocksSize())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "internal error: max used block index="
                                                  << max_used_block_height
                                                  << " >= blockchain size=" << blocksSize();
      return false;
    }

    get_block_hash(blocksAt(max_used_block_height).bl, max_used_block_id);
    return true;
  }

  //  Transaction input validation (internal)
  bool Blockchain::checkTransactionInputs(const Transaction &tx,
                                          uint32_t *pmax_used_block_height)
  {
    crypto::Hash tx_prefix_hash = getObjectHash(*static_cast<const TransactionPrefix *>(&tx));
    return checkTransactionInputs(tx, tx_prefix_hash, pmax_used_block_height);
  }

  bool Blockchain::checkTransactionInputs(const Transaction &tx,
                                          const crypto::Hash &tx_prefix_hash,
                                          uint32_t *pmax_used_block_height)
  {
    if (pmax_used_block_height)
      *pmax_used_block_height = 0;

    crypto::Hash transactionHash = getObjectHash(tx);
    size_t inputIndex = 0;

    for (const auto &txin : tx.inputs)
    {
      assert(inputIndex < tx.signatures.size());

      if (txin.type() == typeid(KeyInput))
      {
        if (!validateKeyInput(boost::get<KeyInput>(txin), tx_prefix_hash,
                              tx.signatures[inputIndex], transactionHash,
                              pmax_used_block_height))
          return false;
        ++inputIndex;
      }
      else if (txin.type() == typeid(MultisignatureInput))
      {
        if (!isInCheckpointZone(getCurrentBlockchainHeight()))
        {
          if (!validateInput(boost::get<MultisignatureInput>(txin),
                             transactionHash, tx_prefix_hash,
                             tx.signatures[inputIndex]))
            return false;
        }
        ++inputIndex;
      }
      else
      {
        logger(logging::INFO, logging::BRIGHT_WHITE) << "Transaction " << transactionHash
                                                     << " contains input of unsupported type.";
        return false;
      }
    }

    return true;
  }

  //  Key input validation
  bool Blockchain::validateKeyInput(const KeyInput &in_to_key,
                                    const crypto::Hash &tx_prefix_hash,
                                    const std::vector<crypto::Signature> &sig,
                                    const crypto::Hash &transactionHash,
                                    uint32_t *pmax_used_block_height)
  {
    if (in_to_key.outputIndexes.empty())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "empty outputIndexes in transaction " << transactionHash;
      return false;
    }

    if (have_tx_keyimg_as_spent(in_to_key.keyImage))
    {
      logger(logging::DEBUGGING) << "Key image already spent: "
                                 << common::podToHex(in_to_key.keyImage);
      return false;
    }

    if (!isInCheckpointZone(getCurrentBlockchainHeight()))
    {
      if (!check_tx_input(in_to_key, tx_prefix_hash, sig, pmax_used_block_height))
      {
        logger(logging::INFO, logging::BRIGHT_WHITE) << "Failed to check input in transaction "
                                                     << transactionHash;
        return false;
      }
    }

    return true;
  }

  bool Blockchain::check_tx_input(const KeyInput &txin,
                                  const crypto::Hash &tx_prefix_hash,
                                  const std::vector<crypto::Signature> &sig,
                                  uint32_t *pmax_related_block_height)
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);

    // Build the output key list via the visitor pattern
    struct outputs_visitor
    {
      std::vector<const crypto::PublicKey *> &m_results_collector;
      Blockchain &m_bch;
      logging::LoggerRef logger;
      outputs_visitor(std::vector<const crypto::PublicKey *> &results_collector,
                      Blockchain &bch, logging::ILogger &logger)
          : m_results_collector(results_collector), m_bch(bch),
            logger(logger, "outputs_visitor") {}

      bool handle_output(const Transaction &tx, const TransactionOutput &out,
                         size_t transactionOutputIndex)
      {
        if (!m_bch.is_tx_spendtime_unlocked(tx.unlockTime))
        {
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Output has wrong unlockTime=" << tx.unlockTime;
          return false;
        }
        if (out.target.type() != typeid(KeyOutput))
        {
          logger(logging::INFO, logging::BRIGHT_WHITE) << "Output has wrong type, which="
                                                       << out.target.which();
          return false;
        }
        m_results_collector.push_back(&boost::get<KeyOutput>(out.target).key);
        return true;
      }
    };

    std::vector<const crypto::PublicKey *> output_keys;
    outputs_visitor vi(output_keys, *this, logger.getLogger());

    if (!scanOutputKeysForIndexes(txin, vi, pmax_related_block_height))
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Failed to get output keys for amount "
                                                   << m_currency.formatAmount(txin.amount)
                                                   << " with " << txin.outputIndexes.size() << " indexes";
      return false;
    }

    return verifyRingSignature(txin, output_keys, sig, tx_prefix_hash);
  }

  //  Ring signature verification

  bool Blockchain::verifyRingSignature(const KeyInput &txin,
                                       const std::vector<const crypto::PublicKey *> &output_keys,
                                       const std::vector<crypto::Signature> &sig,
                                       const crypto::Hash &tx_prefix_hash)
  {
    if (txin.outputIndexes.size() != output_keys.size())
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Output keys mismatch";
      return false;
    }

    // Ring size enforcement (V4→V5 transition)
    if (getCurrentBlockchainHeight() > cn::parameters::UPGRADE_HEIGHT_V4 &&
        getCurrentBlockchainHeight() < cn::parameters::UPGRADE_HEIGHT_V5 &&
        txin.outputIndexes.size() < 3)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "ring size too small";
      return false;
    }

    if (sig.size() != output_keys.size())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "sig count mismatch";
      return false;
    }

    // Skip full verification in checkpoint zone
    if (isInCheckpointZone(getCurrentBlockchainHeight()))
      return true;

    // Key image validity check (must be in the prime-order subgroup)
    static const crypto::KeyImage I = {
        {0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
    static const crypto::KeyImage L = {
        {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
         0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
         0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10}};

    if (!(scalarmultKey(txin.keyImage, L) == I))
      return false;

    return crypto::check_ring_signature(tx_prefix_hash, txin.keyImage,
                                        output_keys, sig.data());
  }

  //  Multisignature input validation
  bool Blockchain::validateInput(const MultisignatureInput &input,
                                 const crypto::Hash &transactionHash,
                                 const crypto::Hash &transactionPrefixHash,
                                 const std::vector<crypto::Signature> &transactionSignatures)
  {
    assert(input.signatureCount == transactionSignatures.size());

    auto amountOutputs = m_multisignatureOutputs.find(input.amount);
    if (amountOutputs == m_multisignatureOutputs.end() ||
        input.outputIndex >= amountOutputs->second.size())
      return false;

    const MultisignatureOutputUsage &outputIndex = amountOutputs->second[input.outputIndex];
    if (outputIndex.isUsed)
      return false;

    // Verify the output transaction is unlocked
    const Transaction &outputTransaction =
        blocksAt(outputIndex.transactionIndex.block)
            .transactions[outputIndex.transactionIndex.transaction]
            .tx;

    if (!is_tx_spendtime_unlocked(outputTransaction.unlockTime))
      return false;

    // Verify the output is a valid MultisignatureOutput
    assert(outputTransaction.outputs[outputIndex.outputIndex].target.type() ==
           typeid(MultisignatureOutput));

    const MultisignatureOutput &output =
        boost::get<MultisignatureOutput>(
            outputTransaction.outputs[outputIndex.outputIndex].target);

    // Validate term and signature count match
    if (input.signatureCount != output.requiredSignatureCount ||
        input.term != output.term)
      return false;

    // Check if deposit term has expired
    if (output.term != 0 &&
        outputIndex.transactionIndex.block + output.term > getCurrentBlockchainHeight())
      return false;

    // Verify each signature against the output keys
    return verifyMultisigSignatures(output.keys, transactionSignatures,
                                    input.signatureCount, transactionPrefixHash);
  }

  bool Blockchain::verifyMultisigSignatures(
      const std::vector<crypto::PublicKey> &outputKeys,
      const std::vector<crypto::Signature> &signatures,
      size_t requiredCount,
      const crypto::Hash &txPrefixHash)
  {
    size_t inputSignatureIndex = 0;
    size_t outputKeyIndex = 0;

    while (inputSignatureIndex < requiredCount)
    {
      if (outputKeyIndex == outputKeys.size())
        return false;

      if (crypto::check_signature(txPrefixHash, outputKeys[outputKeyIndex],
                                  signatures[inputSignatureIndex]))
        ++inputSignatureIndex;

      ++outputKeyIndex;
    }

    return true;
  }

  //  Output validation
  bool Blockchain::check_tx_outputs(const Transaction &tx, uint32_t height) const
  {
    std::string error;
    for (const TransactionOutput &out : tx.outputs)
    {
      if (!boost::apply_visitor(
              check_tx_outputs_visitor(tx, height, out.amount, m_currency, error),
              out.target))
      {
        logger(logging::ERROR, logging::BRIGHT_WHITE) << getObjectHash(tx) << ": " << error;
        return false;
      }
    }
    return true;
  }

  //  Spent key image checks
  bool Blockchain::haveSpentKeyImages(const Transaction &tx)
  {
    return haveTransactionKeyImagesAsSpent(tx);
  }

  bool Blockchain::haveTransactionKeyImagesAsSpent(const Transaction &tx)
  {
    for (const auto &in : tx.inputs)
      if (in.type() == typeid(KeyInput) &&
          have_tx_keyimg_as_spent(boost::get<KeyInput>(in).keyImage))
        return true;
    return false;
  }

  //  Transaction size check
  bool Blockchain::checkTransactionSize(size_t blobSize)
  {
    size_t maxSize = getCurrentCumulativeBlocksizeLimit() -
                     m_currency.minerTxBlobReservedSize();

    if (blobSize >= maxSize)
    {
      logger(logging::ERROR) << "transaction is too big " << blobSize
                             << ", maximum allowed size is " << maxSize;
      return false;
    }

    return true;
  }

  //  Unlock time check
  bool Blockchain::is_tx_spendtime_unlocked(uint64_t unlock_time)
  {
    if (unlock_time < m_currency.maxBlockHeight())
      return getCurrentBlockchainHeight() - 1 +
                 m_currency.lockedTxAllowedDeltaBlocks() >=
             unlock_time;

    auto current_time = static_cast<uint64_t>(time(nullptr));
    return current_time + m_currency.lockedTxAllowedDeltaSeconds() >= unlock_time;
  }

  //  Miner transaction validation
  bool Blockchain::prevalidate_miner_transaction(const Block &b, uint32_t height) const
  {
    const Transaction &tx = b.baseTransaction;

    if (tx.inputs.size() != 1)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "coinbase transaction has no inputs";
      return false;
    }

    if (tx.signatures.size() > 1)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "coinbase transaction has " << tx.signatures.size()
                                                  << " signatures, expected 1";
      return false;
    }

    if (tx.inputs[0].type() != typeid(BaseInput))
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "coinbase transaction has wrong input type";
      return false;
    }

    if (boost::get<BaseInput>(tx.inputs[0]).blockIndex != height)
    {
      logger(logging::INFO, logging::BRIGHT_RED) << "miner transaction has invalid height: "
                                                 << boost::get<BaseInput>(tx.inputs[0]).blockIndex
                                                 << ", expected: " << height;
      return false;
    }

    if (tx.unlockTime != height + m_currency.minedMoneyUnlockWindow())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "coinbase transaction has wrong unlock time="
                                                  << tx.unlockTime << ", expected "
                                                  << height + m_currency.minedMoneyUnlockWindow();
      return false;
    }

    if (!check_outs_valid(tx))
    {
      logger(logging::INFO, logging::BRIGHT_RED) << "miner transaction has invalid outputs";
      return false;
    }

    if (!check_outs_overflow(tx))
    {
      logger(logging::INFO, logging::BRIGHT_RED) << "miner transaction has money overflow in block "
                                                 << get_block_hash(b);
      return false;
    }

    return true;
  }

  bool Blockchain::validate_miner_transaction(const Block &b, uint32_t height,
                                              size_t cumulativeBlockSize,
                                              uint64_t alreadyGeneratedCoins,
                                              uint64_t fee, uint64_t &reward,
                                              int64_t &emissionChange)
  {
    uint64_t minerReward = 0;
    for (const auto &o : b.baseTransaction.outputs)
      minerReward += o.amount;

    std::vector<size_t> lastBlocksSizes;
    get_last_n_blocks_sizes(lastBlocksSizes, m_currency.rewardBlocksWindow());
    size_t blocksSizeMedian = common::medianValue(lastBlocksSizes);

    if (!m_currency.getBlockReward(blocksSizeMedian, cumulativeBlockSize,
                                   alreadyGeneratedCoins, fee, height,
                                   reward, emissionChange))
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "block size " << cumulativeBlockSize
                                                   << " exceeds allowed limit";
      return false;
    }

    if (minerReward > reward && (minerReward - reward) > 10)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Coinbase transaction spends too much: "
                                                  << m_currency.formatAmount(minerReward)
                                                  << ", block reward is " << m_currency.formatAmount(reward);
      return false;
    }

    if (minerReward < reward)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Coinbase transaction doesn't claim full reward: spent "
                                                  << m_currency.formatAmount(minerReward)
                                                  << ", block reward is " << m_currency.formatAmount(reward)
                                                  << ", fee is " << fee;
      return false;
    }

    return true;
  }

  //  Block-level validation
  bool Blockchain::checkBlockVersion(const Block &b, const crypto::Hash &blockHash)
  {
    uint64_t height = get_block_height(b);
    const uint8_t expected = get_block_major_version_for_height(height);

    if (b.majorVersion != expected)
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Block " << blockHash
                                                   << " has wrong major version";
      return false;
    }
    return true;
  }

  bool Blockchain::checkCumulativeBlockSize(const crypto::Hash &blockId,
                                            size_t cumulativeBlockSize,
                                            uint64_t height)
  {
    size_t maxSize = m_currency.maxBlockCumulativeSize(height);
    if (cumulativeBlockSize > maxSize)
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Block " << blockId << " is too big";
      return false;
    }
    return true;
  }

  bool Blockchain::getBlockCumulativeSize(const Block &block, size_t &cumulativeSize)
  {
    try
    {
      std::vector<Transaction> blockTxs;
      std::vector<crypto::Hash> missedTxs;
      getTransactions(block.transactionHashes, blockTxs, missedTxs, true);

      cumulativeSize = getObjectBinarySize(block.baseTransaction);
      for (const Transaction &tx : blockTxs)
        cumulativeSize += getObjectBinarySize(tx);

      if (!missedTxs.empty())
        logger(logging::DEBUGGING) << "Some transactions missing for cumulative size calculation";

      return missedTxs.empty();
    }
    catch (const std::exception &)
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "Error calculating cumulative block size";
      return false;
    }
  }

  //  Timestamp validation
  uint64_t Blockchain::get_adjusted_time() const
  {
    return time(nullptr);
  }

  bool Blockchain::check_block_timestamp_main(const Block &b)
  {
    if (b.timestamp > get_adjusted_time() + m_currency.blockFutureTimeLimit())
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Timestamp of block " << get_block_hash(b)
                                                   << " too far in future";
      return false;
    }

    std::vector<uint64_t> timestamps;
    size_t sz = blocksSize();
    size_t offset = sz <= m_currency.timestampCheckWindow()
                        ? 0
                        : sz - m_currency.timestampCheckWindow();

    for (; offset != sz; ++offset)
      timestamps.push_back(getBlockHeader(offset).timestamp);

    return check_block_timestamp(std::move(timestamps), b);
  }

  bool Blockchain::check_block_timestamp(std::vector<uint64_t> timestamps,
                                         const Block &b)
  {
    if (timestamps.size() < m_currency.timestampCheckWindow())
      return true;

    uint64_t median_ts = common::medianValue(timestamps);
    if (b.timestamp < median_ts)
    {
      logger(logging::INFO, logging::BRIGHT_WHITE) << "Timestamp of block " << get_block_hash(b)
                                                   << " less than median";
      return false;
    }
    return true;
  }

  bool Blockchain::complete_timestamps_vector(uint64_t start_top_height,
                                              std::vector<uint64_t> &timestamps)
  {
    if (timestamps.size() >= m_currency.timestampCheckWindow())
      return true;

    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    size_t need_elements = m_currency.timestampCheckWindow() - timestamps.size();

    if (start_top_height >= blocksSize())
    {
      logger(logging::ERROR, logging::BRIGHT_RED) << "internal error: start_height=" << start_top_height
                                                  << " >= blocksSize()=" << blocksSize();
      return false;
    }

    size_t stop_offset = start_top_height > need_elements
                             ? start_top_height - need_elements
                             : 0;

    do
    {
      timestamps.push_back(blocksAt(start_top_height).bl.timestamp);
      if (start_top_height == 0)
        break;
      --start_top_height;
    } while (start_top_height != stop_offset);

    return true;
  }

  //  Checkpoint validation
  bool Blockchain::checkCheckpoints(uint32_t &lastValidCheckpointHeight)
  {
    std::vector<uint32_t> checkpointHeights = m_checkpoints.getCheckpointHeights();
    for (const auto &checkpointHeight : checkpointHeights)
    {
      if (blocksSize() <= checkpointHeight)
        return true;

      if (m_checkpoints.check_block(checkpointHeight,
                                    getBlockIdByHeight(checkpointHeight)))
        lastValidCheckpointHeight = checkpointHeight;
      else
        return false;
    }

    logger(logging::INFO, logging::BRIGHT_WHITE) << "Checkpoints passed";
    return true;
  }

  bool Blockchain::checkDomainRegistrationFee(const Transaction &tx, uint32_t height) const
  {
    // Only enforce post-fork
    if (height < parameters::UPGRADE_HEIGHT_V9)
      return true;

    bool hasDomainRegistration = false;
    bool hasFeePayment = false;

    for (const auto &output : tx.outputs)
    {
      if (output.target.type() == typeid(DomainRegistrationOutput))
      {
        hasDomainRegistration = true;
      }
      else if (output.target.type() == typeid(StandardPaymentOutput) &&
               output.amount == cn::DOMAIN_REGISTRATION_FEE)
      {
        // Verify the fee is going to the team address
        // We need the txPubKey to derive the output key
        // For now, just check the amount matches
        hasFeePayment = true;
      }
    }

    // If there's a domain registration, there must be a matching fee payment
    if (hasDomainRegistration && !hasFeePayment)
    {
      logger(logging::ERROR, logging::BRIGHT_RED)
          << "Transaction contains domain registration without required "
          << cn::DOMAIN_REGISTRATION_FEE / parameters::COIN << " CCX fee";
      return false;
    }

    return true;
  }
} // namespace cn