// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2020 Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

#include <parallel_hashmap/phmap.h>

#include "Blockchain/BlockchainFilter.h"
#include "Blockchain/BlockchainIndices.h"
#include "Blockchain/BlockchainMessages.h"
#include "Blockchain/Checkpoints.h"
#include "Blockchain/DepositIndex.h"
#include "Blockchain/IBlockchainStorageObserver.h"
#include "Blockchain/ITransactionValidator.h"
#include "Blockchain/UpgradeDetector.h"
#include "Common/ObserverManager.h"
#include "Common/Util.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/IntrusiveLinkedList.h"
#include "CryptoNoteCore/MessageQueue.h"
#include "CryptoNoteCore/TransactionPool.h"
#include "Logging/LoggerRef.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "CryptoNoteCore/DomainIndex.h"

#define CURRENT_BLOCKCACHE_STORAGE_ARCHIVE_VER 5
#define CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER 1

#undef ERROR

namespace cn
{
  // Forward declarations
  struct NOTIFY_REQUEST_GET_OBJECTS_request;
  struct NOTIFY_RESPONSE_GET_OBJECTS_request;
  struct COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_request;
  struct COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_response;
  struct COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_outs_for_amount;

  class BlockchainIndicesSerializer;

  using cn::BlockInfo;
  using phmap::parallel_flat_hash_map;

  class Blockchain : public cn::ITransactionValidator
  {
  public:
    // Nested types
    struct TransactionIndex
    {
      uint32_t block;
      uint16_t transaction;

      void serialize(ISerializer &s)
      {
        s(block, "block");
        s(transaction, "tx");
      }
    };

    struct TransactionEntry
    {
      Transaction tx;
      std::vector<uint32_t> m_global_output_indexes;

      void serialize(ISerializer &s)
      {
        s(tx, "tx");
        s(m_global_output_indexes, "indexes");
      }
    };

    struct BlockEntry
    {
      Block bl;
      uint32_t height;
      uint64_t block_cumulative_size;
      difficulty_type cumulative_difficulty;
      uint64_t already_generated_coins;
      std::vector<TransactionEntry> transactions;

      void serialize(ISerializer &s)
      {
        s(bl, "block");
        s(height, "height");
        s(block_cumulative_size, "block_cumulative_size");
        s(cumulative_difficulty, "cumulative_difficulty");
        s(already_generated_coins, "already_generated_coins");
        s(transactions, "transactions");
      }
    };

    using CheckpointGeneratedCallback = std::function<void(uint32_t height, const crypto::Hash &hash)>;

    // Construction / destruction
    Blockchain(const Currency &currency, tx_memory_pool &tx_pool,
               logging::ILogger &logger, bool blockchainIndexesEnabled);

    // Lifecycle
    bool init() { return init(tools::getDefaultDataDirectory(), true, m_testnet); }
    bool init(const std::string &config_folder, bool load_existing, bool testnet);
    bool deinit();
    bool resetAndSetGenesisBlock(const Block &b);

    // Cache management
    bool rebuildCache();
    bool storeCache();

    // Observer pattern
    bool addObserver(IBlockchainStorageObserver *observer);
    bool removeObserver(IBlockchainStorageObserver *observer);

    // ITransactionValidator interface
    bool checkTransactionInputs(const Transaction &tx, BlockInfo &maxUsedBlock) override;
    bool checkTransactionInputs(const Transaction &tx, BlockInfo &maxUsedBlock,
                                BlockInfo &lastFailed) override;
    bool haveSpentKeyImages(const Transaction &tx) override;
    bool checkTransactionSize(size_t blobSize) override;

    // Block storage access
    size_t blocksSize() const;
    bool blocksEmpty() const;
    const BlockEntry &blocksAt(size_t i) const;
    BlockEntry &blocksAt(size_t i);
    BlockEntry blocksBack() const;
    void blocksClear();
    BlockHeaderPOD getBlockHeader(uint32_t height) const;

    // Chain state queries
    uint32_t getCurrentBlockchainHeight();
    crypto::Hash getTailId();
    crypto::Hash getTailId(uint32_t &height);
    crypto::Hash getBlockIdByHeight(uint32_t height);
    bool getBlockByHash(const crypto::Hash &h, Block &blk);
    bool getBlockHeight(const crypto::Hash &blockId, uint32_t &blockHeight);
    bool haveBlock(const crypto::Hash &id);
    bool haveTransaction(const crypto::Hash &id);
    bool isBlockInMainChain(const crypto::Hash &blockId) const;
    bool isInCheckpointZone(const uint32_t height) const;
    size_t getTotalTransactions();
    uint64_t getBlockTimestamp(uint32_t height);
    uint64_t getCoinsInCirculation();
    uint64_t coinsEmittedAtHeight(uint64_t height);
    uint8_t get_block_major_version_for_height(uint64_t height) const;

    // Transaction queries
    bool getBlockContainingTransaction(const crypto::Hash &txId,
                                       crypto::Hash &blockId, uint32_t &blockHeight);
    bool getTransactionOutputGlobalIndexes(const crypto::Hash &tx_id,
                                           std::vector<uint32_t> &indexs);
    bool getTransactionsWithOutputGlobalIndexes(
        const std::vector<crypto::Hash> &txs_ids,
        std::list<crypto::Hash> &missed_txs,
        std::vector<std::pair<Transaction, std::vector<uint32_t>>> &txs);
    bool getTransactionIdsByPaymentId(const crypto::Hash &paymentId,
                                      std::vector<crypto::Hash> &transactionHashes);
    bool have_tx_keyimg_as_spent(const crypto::KeyImage &key_im);
    bool haveTransactionKeyImagesAsSpent(const Transaction &tx);

    // Multisignature queries
    bool get_out_by_msig_gindex(uint64_t amount, uint64_t gindex,
                                MultisignatureOutput &out);
    bool getMultisigOutputReference(const MultisignatureInput &txInMultisig,
                                    std::pair<crypto::Hash, size_t> &outputReference);

    // Block metadata queries
    bool getAlreadyGeneratedCoins(const crypto::Hash &hash, uint64_t &generatedCoins);
    bool getBlockSize(const crypto::Hash &hash, size_t &size);
    uint64_t blockDifficulty(size_t i);
    difficulty_type difficultyAtHeight(uint64_t height);
    uint64_t getCurrentCumulativeBlocksizeLimit() const;

    // Difficulty
    difficulty_type getDifficultyForNextBlock();
    difficulty_type calculateDifficulty(uint8_t majorVersion, uint64_t blockIndex,
                                        const std::vector<uint64_t> &timestamps,
                                        const std::vector<difficulty_type> &cumulativeDifficulties);
    void collectDifficultyDataMainChain(const std::list<crypto::Hash> &alt_chain,
                                        const BlockEntry &bei, size_t window,
                                        std::vector<uint64_t> &timestamps,
                                        std::vector<difficulty_type> &cumulativeDifficulties);
    void collectDifficultyDataAltChain(const std::list<crypto::Hash> &alt_chain,
                                       size_t window,
                                       std::vector<uint64_t> &timestamps,
                                       std::vector<difficulty_type> &cumulativeDifficulties);
    bool loadBlockEntry(uint64_t height, BlockEntry &entry);

    // Index queries
    bool getGeneratedTransactionsNumber(uint32_t height, uint64_t &generatedTransactions);
    bool getBlockIdsByTimestamp(uint64_t timestampBegin, uint64_t timestampEnd,
                                uint32_t blocksNumberLimit,
                                std::vector<crypto::Hash> &hashes,
                                uint32_t &blocksNumberWithinTimestamps);
    bool getOrphanBlockIdsByHeight(uint32_t height,
                                   std::vector<crypto::Hash> &blockHashes);

    // Random output selection (ring signature mixins)
    bool getRandomOutsByAmount(
        const COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_request &req,
        COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_response &res);

    // Deposit tracking
    uint64_t fullDepositAmount() const;
    uint64_t depositAmountAtHeight(size_t height) const;
    uint64_t depositInterestAtHeight(size_t height) const;

    // Block addition / reorg
    bool addNewBlock(const Block &bl_, block_verification_context &bvc);
    bool rollbackBlockchainTo(uint32_t height);
    bool getAlternativeBlocks(std::list<Block> &blocks);
    uint32_t getAlternativeBlocksCount();

    // Sync helpers
    std::vector<crypto::Hash> buildSparseChain();
    std::vector<crypto::Hash> buildSparseChain(const crypto::Hash &startBlockId);
    uint32_t findBlockchainSupplement(const std::vector<crypto::Hash> &qblock_ids);
    std::vector<crypto::Hash> findBlockchainSupplement(
        const std::vector<crypto::Hash> &remoteBlockIds, size_t maxCount,
        uint32_t &totalBlockCount, uint32_t &startBlockIndex);
    bool getLowerBound(uint64_t timestamp, uint64_t startOffset, uint32_t &height);
    std::vector<crypto::Hash> getBlockIds(uint32_t startHeight, uint32_t maxCount);
    bool getBlocks(uint32_t start_offset, uint32_t count,
                   std::list<Block> &blocks, std::list<Transaction> &txs);
    bool getBlocks(uint32_t start_offset, uint32_t count, std::list<Block> &blocks);
    bool getBackwardBlocksSize(size_t from_height, std::vector<size_t> &sz, size_t count);
    void invalidateSparseChainCache();
    std::vector<crypto::Hash> getCachedSparseChain();

    // P2P handlers
    bool handleGetObjects(NOTIFY_REQUEST_GET_OBJECTS_request &arg,
                          NOTIFY_RESPONSE_GET_OBJECTS_request &rsp);

    // Checkpoints
    void setCheckpoints(Checkpoints &&chk_pts) { m_checkpoints = std::move(chk_pts); }
    CheckpointList getCheckpointList(uint32_t startHeight, uint32_t endHeight) const
    {
      return m_checkpoints.getCheckpointList(startHeight, endHeight);
    }
    bool addCheckpoint(uint32_t height, const std::string &hash)
    {
      return m_checkpoints.add_checkpoint(height, hash);
    }
    void setCheckpointGeneratedCallback(CheckpointGeneratedCallback callback);

    // Message queue
    bool addMessageQueue(MessageQueue<BlockchainMessage> &messageQueue);
    bool removeMessageQueue(MessageQueue<BlockchainMessage> &messageQueue);

    // Validation (public)
    bool check_tx_outputs(const Transaction &tx, uint32_t height) const;
    bool checkTransactionInputs(const Transaction &tx,
                                uint32_t &pmax_used_block_height,
                                crypto::Hash &max_used_block_id,
                                BlockInfo *tail = nullptr);
    bool checkDomainRegistrationFee(const Transaction &tx, uint32_t height) const;

    // Filter database access
    bool getBlockFilterRecord(uint32_t height, BlockFilterRecord &record) const;
    bool hasBlockFilterRecord(uint32_t height) const;

    const DomainIndex &getDomainIndex() const { return m_domainIndex; }
    DomainIndex &getDomainIndex() { return m_domainIndex; }

    // Debug
    void print_blockchain(uint64_t start_index, uint64_t end_index);
    void print_blockchain_index(bool print_all);
    void print_blockchain_outs(const std::string &file);
    std::string printDatabaseStats() const;

    // Serialisation
    template <class archive_t>
    void serialize(archive_t &ar, const unsigned int version);

    // Template helpers (public, used by P2P / RPC)
    template <class visitor_t>
    bool scanOutputKeysForIndexes(const KeyInput &tx_in_to_key, visitor_t &vis,
                                  uint32_t *pmax_related_block_height = nullptr);

    template <class t_ids_container, class t_blocks_container, class t_missed_container>
    bool getBlocks(const t_ids_container &block_ids, t_blocks_container &blocks,
                   t_missed_container &missed_bs);

    template <class t_ids_container, class t_tx_container, class t_missed_container>
    void getBlockchainTransactions(const t_ids_container &txs_ids,
                                   t_tx_container &txs,
                                   t_missed_container &missed_txs);

    template <class t_ids_container, class t_tx_container, class t_missed_container>
    void getTransactions(const t_ids_container &txs_ids, t_tx_container &txs,
                         t_missed_container &missed_txs, bool checkTxPool = false);

    CryptoNote::MDBXBlockchainStorage *getMdbxStorage() const { return m_mdbxStorage.get(); }

    // Legacy public member
    uint8_t blockMajorVersion;

  private:
    // Private nested types
    struct MultisignatureOutputUsage
    {
      TransactionIndex transactionIndex;
      uint16_t outputIndex;
      bool isUsed;

      void serialize(ISerializer &s)
      {
        s(transactionIndex, "txindex");
        s(outputIndex, "outindex");
        s(isUsed, "used");
      }
    };

    struct CachedEntry
    {
      size_t height;
      BlockEntry entry;
    };

    // Type aliases
    using key_images_container = parallel_flat_hash_map<crypto::KeyImage, uint32_t>;
    using blocks_ext_by_hash = parallel_flat_hash_map<crypto::Hash, BlockEntry>;
    using outputs_container = parallel_flat_hash_map<uint64_t,
                                                     std::vector<std::pair<TransactionIndex, uint16_t>>>;
    using MultisignatureOutputsContainer = parallel_flat_hash_map<uint64_t,
                                                                  std::vector<MultisignatureOutputUsage>>;
    using BlockMap = parallel_flat_hash_map<crypto::Hash, uint32_t>;
    using TransactionMap = parallel_flat_hash_map<crypto::Hash, TransactionIndex>;

    // Constants
    static constexpr size_t MDBX_CACHE_SIZE = 256;
    static constexpr uint32_t SPARSE_CHAIN_CACHE_DURATION_SECONDS = 10;
    static constexpr uint32_t SPARSE_CHAIN_CACHE_BLOCK_DELTA = 100;

    // Friends
    friend class BlockchainIndicesSerializer;
    friend class LockedBlockchainStorage;

    // Core members
    bool m_testnet = false;
    const Currency &m_currency;
    tx_memory_pool &m_tx_pool;
    mutable std::recursive_mutex m_blockchain_lock;
    crypto::cn_context m_cn_context;
    tools::ObserverManager<IBlockchainStorageObserver> m_observerManager;
    std::string m_config_folder;
    Checkpoints m_checkpoints;
    std::atomic<bool> m_is_in_checkpoint_zone;
    logging::LoggerRef logger;

    // Storage backend
    std::unique_ptr<CryptoNote::MDBXBlockchainStorage> m_mdbxStorage;

    // In-memory chain index
    std::vector<crypto::Hash> m_blockHashes;
    parallel_flat_hash_map<crypto::Hash, uint32_t> m_hashToHeight;
    mutable std::vector<CachedEntry> m_cachedEntries;
    mutable size_t m_cacheIndex = 0;

    // In-memory caches
    key_images_container m_spent_keys;
    size_t m_current_block_cumul_sz_limit = 0;
    blocks_ext_by_hash m_alternative_chains;
    outputs_container m_outputs;
    TransactionMap m_transactionMap;
    MultisignatureOutputsContainer m_multisignatureOutputs;

    // Deposit tracking
    cn::DepositIndex m_depositIndex;

    // Domain name system
    DomainIndex m_domainIndex;

    // Upgrade detectors
    BasicUpgradeDetector m_upgradeDetectorV2;
    BasicUpgradeDetector m_upgradeDetectorV3;
    BasicUpgradeDetector m_upgradeDetectorV4;
    BasicUpgradeDetector m_upgradeDetectorV7;
    BasicUpgradeDetector m_upgradeDetectorV8;

    // Optional blockchain indexes
    bool m_blockchainIndexesEnabled;
    PaymentIdIndex m_paymentIdIndex;
    TimestampBlocksIndex m_timestampIndex;
    GeneratedTransactionsIndex m_generatedTransactionsIndex;
    OrphanBlocksIndex m_orthanBlocksIndex;

    // Message queue
    IntrusiveLinkedList<MessageQueue<BlockchainMessage>> m_messageQueueList;

    // Sparse chain cache
    mutable std::mutex m_sparseChainCacheMutex;
    mutable std::vector<crypto::Hash> m_cachedSparseChain;
    mutable uint32_t m_cachedSparseChainHeight;
    mutable std::chrono::steady_clock::time_point m_lastSparseChainUpdate;
    mutable bool m_sparseChainCacheValid;

    // Checkpoint callback
    CheckpointGeneratedCallback m_checkpointGeneratedCallback;

    //  Private methods — Init
    bool initMdbxStorage(const std::string &config_folder);
    bool initMdbx(bool load_existing);
    void rebuildMdbxIndex();
    bool ensureGenesisBlock();
    bool validateGenesisBlock();
    bool initUpgradeDetectors();
    void logInitSummary();

    //  Private methods — Storage
    bool blockExistsInMainChain(const crypto::Hash &blockHash) const;
    const TransactionEntry &transactionByIndex(TransactionIndex index);
    static cn::BlockHeaderPOD headerFromBlockEntry(const BlockEntry &entry);

    bool pushBlock(const Block &blockData, const crypto::Hash &id,
                   block_verification_context &bvc, uint32_t height);
    bool pushBlock(const Block &blockData, const std::vector<Transaction> &transactions,
                   const crypto::Hash &id, block_verification_context &bvc);
    bool pushBlock(const BlockEntry &block);
    bool pushBlockMdbx(const BlockEntry &block);

    void popBlock(const crypto::Hash &blockHash);
    void popBlockMdbx(const crypto::Hash &blockHash);
    bool removeLastBlock();

    bool pushTransaction(BlockEntry &block, const crypto::Hash &transactionHash,
                         TransactionIndex transactionIndex);
    void popTransaction(const Transaction &transaction, const crypto::Hash &transactionHash);
    void popTransactions(const BlockEntry &block, const crypto::Hash &minerTransactionHash);

    bool validateAndPushTransaction(BlockEntry &block, const Transaction &tx,
                                    const crypto::Hash &tx_id, size_t txIndex,
                                    TransactionIndex &transactionIndex,
                                    size_t &cumulative_block_size,
                                    uint64_t &fee_summary, uint64_t &interestSummary,
                                    const crypto::Hash &blockHash,
                                    block_verification_context &bvc);
    bool markKeyImagesSpent(const Transaction &tx, uint32_t blockHeight);
    void markMultisigInputsSpent(const Transaction &tx);
    void popKeyOutput(uint64_t amount, const TransactionIndex &txIndex, size_t outputIndex);
    void popMultisigOutput(uint64_t amount, const TransactionIndex &txIndex, size_t outputIndex);
    void notifyUpgradeDetectorsBlockPushed();
    void notifyUpgradeDetectorsBlockPopped();

    bool loadTransactions(const Block &block, std::vector<Transaction> &transactions,
                          uint32_t height);
    void saveTransactions(const std::vector<Transaction> &transactions, uint32_t height);

    bool storeMdbxCache();

    //  Private methods — Validation
    bool checkTransactionInputs(const Transaction &tx, uint32_t *pmax_used_block_height);
    bool checkTransactionInputs(const Transaction &tx, const crypto::Hash &tx_prefix_hash,
                                uint32_t *pmax_used_block_height);
    bool validateKeyInput(const KeyInput &in_to_key, const crypto::Hash &tx_prefix_hash,
                          const std::vector<crypto::Signature> &sig,
                          const crypto::Hash &transactionHash,
                          uint32_t *pmax_used_block_height);
    bool check_tx_input(const KeyInput &txin, const crypto::Hash &tx_prefix_hash,
                        const std::vector<crypto::Signature> &sig,
                        uint32_t *pmax_related_block_height = nullptr);
    bool verifyRingSignature(const KeyInput &txin,
                             const std::vector<const crypto::PublicKey *> &output_keys,
                             const std::vector<crypto::Signature> &sig,
                             const crypto::Hash &tx_prefix_hash);
    bool validateInput(const MultisignatureInput &input, const crypto::Hash &transactionHash,
                       const crypto::Hash &transactionPrefixHash,
                       const std::vector<crypto::Signature> &transactionSignatures);
    bool verifyMultisigSignatures(const std::vector<crypto::PublicKey> &outputKeys,
                                  const std::vector<crypto::Signature> &signatures,
                                  size_t requiredCount, const crypto::Hash &txPrefixHash);

    bool prevalidate_miner_transaction(const Block &b, uint32_t height) const;
    bool validate_miner_transaction(const Block &b, uint32_t height,
                                    size_t cumulativeBlockSize,
                                    uint64_t alreadyGeneratedCoins, uint64_t fee,
                                    uint64_t &reward, int64_t &emissionChange);
    bool checkBlockVersion(const Block &b, const crypto::Hash &blockHash);
    bool checkCumulativeBlockSize(const crypto::Hash &blockId,
                                  size_t cumulativeBlockSize, uint64_t height);
    bool getBlockCumulativeSize(const Block &block, size_t &cumulativeSize);
    bool is_tx_spendtime_unlocked(uint64_t unlock_time);
    uint64_t get_adjusted_time() const;

    bool check_block_timestamp_main(const Block &b);
    bool check_block_timestamp(std::vector<uint64_t> timestamps, const Block &b);
    bool complete_timestamps_vector(uint64_t start_height,
                                    std::vector<uint64_t> &timestamps);

    bool checkCheckpoints(uint32_t &lastValidCheckpointHeight);

    //  Private methods — Reorg
    bool switch_to_alternative_blockchain(const std::list<crypto::Hash> &alt_chain,
                                          bool discard_disconnected_chain);
    bool handle_alternative_block(const Block &b, const crypto::Hash &id,
                                  block_verification_context &bvc,
                                  bool sendNewAlternativeBlockMessage = true);
    bool rollback_blockchain_switching(const std::list<Block> &original_chain,
                                       size_t rollback_height);
    bool findPreviousBlockHeight(const crypto::Hash &prevHash,
                                 uint32_t &height, bool &inMainChain);
    bool verifyAlternativeChainTransactions(const std::list<crypto::Hash> &alt_chain,
                                            uint32_t split_height);

    //  Private methods — Sync
    std::vector<crypto::Hash> doBuildSparseChain(const crypto::Hash &startBlockId) const;
    std::vector<crypto::Hash> doBuildSparseChainUnlocked(const crypto::Hash &startBlockId) const;
    std::vector<crypto::Hash> doBuildSparseChainMdbx(const crypto::Hash &startBlockId) const;
    std::vector<crypto::Hash> buildSparseFromHeightMdbx(size_t height) const;
    bool findHashHeight(const crypto::Hash &hash, size_t &height) const;
    crypto::Hash walkAlternativeChainToAncestor(const crypto::Hash &startId,
                                                std::vector<crypto::Hash> &altPart) const;
    bool isSparseChainCacheValid(uint32_t currentHeight) const;
    void updateSparseChainCache(const std::vector<crypto::Hash> &chain,
                                uint32_t currentHeight);
    uint32_t findBlockchainSupplementInternal(
        const std::vector<crypto::Hash> &qblock_ids) const;

    //  Private methods — Difficulty
    difficulty_type get_next_difficulty_for_alternative_chain(
        const std::list<crypto::Hash> &alt_chain, const BlockEntry &bei);
    bool get_last_n_blocks_sizes(std::vector<size_t> &sz, size_t count);
    bool update_next_comulative_size_limit();

    //  Private methods — Deposit
    void pushToDepositIndex(const BlockEntry &block, uint64_t interest);

    //  Private methods — Random outputs
    bool add_out_to_get_random_outs(
        std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs,
        COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_outs_for_amount &result_outs,
        uint64_t amount, size_t i);
    size_t find_end_of_allowed_index(
        const std::vector<std::pair<TransactionIndex, uint16_t>> &amount_outs);

    //  Private methods — Indices
    bool storeBlockchainIndices();
    bool loadBlockchainIndices();
    void rebuildBlockchainIndices();

    //  Private methods — Messaging
    void sendMessage(const BlockchainMessage &message);
  };

  //  LockedBlockchainStorage — RAII mutex guard
  class LockedBlockchainStorage : private boost::noncopyable
  {
  public:
    explicit LockedBlockchainStorage(Blockchain &bc)
        : m_bc(bc), m_lock(bc.m_blockchain_lock) {}

    Blockchain *operator->() { return &m_bc; }

  private:
    Blockchain &m_bc;
    std::lock_guard<std::recursive_mutex> m_lock;
  };

  //  Template method implementations
  template <class visitor_t>
  bool Blockchain::scanOutputKeysForIndexes(const KeyInput &tx_in_to_key,
                                            visitor_t &vis,
                                            uint32_t *pmax_related_block_height)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);
    auto it = m_outputs.find(tx_in_to_key.amount);
    if (it == m_outputs.end() || tx_in_to_key.outputIndexes.empty())
      return false;

    std::vector<uint32_t> absolute_offsets =
        relative_output_offsets_to_absolute(tx_in_to_key.outputIndexes);
    auto &amount_outs_vec = it->second;
    size_t count = 0;

    for (uint64_t i : absolute_offsets)
    {
      if (i >= amount_outs_vec.size())
      {
        logger(logging::INFO) << "Wrong index in transaction inputs: " << i
                              << ", expected maximum " << amount_outs_vec.size() - 1;
        return false;
      }

      const TransactionEntry &tx = transactionByIndex(amount_outs_vec[i].first);

      if (amount_outs_vec[i].second >= tx.tx.outputs.size())
      {
        logger(logging::ERROR, logging::BRIGHT_RED)
            << "Wrong index in transaction outputs: "
            << amount_outs_vec[i].second << ", expected less than "
            << tx.tx.outputs.size();
        return false;
      }

      if (!vis.handle_output(tx.tx, tx.tx.outputs[amount_outs_vec[i].second],
                             amount_outs_vec[i].second))
      {
        logger(logging::INFO) << "Failed to handle_output for output no = "
                              << count << ", with absolute offset " << i;
        return false;
      }

      if (count++ == absolute_offsets.size() - 1 &&
          pmax_related_block_height &&
          *pmax_related_block_height < amount_outs_vec[i].first.block)
      {
        *pmax_related_block_height = amount_outs_vec[i].first.block;
      }
    }

    return true;
  }

  template <class t_ids_container, class t_blocks_container, class t_missed_container>
  bool Blockchain::getBlocks(const t_ids_container &block_ids,
                             t_blocks_container &blocks,
                             t_missed_container &missed_bs)
  {
    std::lock_guard<std::recursive_mutex> lk(m_blockchain_lock);

    for (const auto &bl_id : block_ids)
    {
      auto it = m_hashToHeight.find(bl_id);
      if (it == m_hashToHeight.end())
      {
        missed_bs.push_back(bl_id);
        continue;
      }

      uint32_t height = it->second;
      if (height >= blocksSize())
      {
        logger(logging::ERROR, logging::BRIGHT_RED)
            << "Internal error: bl_id=" << common::podToHex(bl_id)
            << " height=" << height << " >= blocksSize()=" << blocksSize();
        return false;
      }

      blocks.push_back(blocksAt(height).bl);
    }

    return true;
  }

  template <class t_ids_container, class t_tx_container, class t_missed_container>
  void Blockchain::getBlockchainTransactions(const t_ids_container &txs_ids,
                                             t_tx_container &txs,
                                             t_missed_container &missed_txs)
  {
    std::lock_guard<std::recursive_mutex> bcLock(m_blockchain_lock);

    for (const auto &tx_id : txs_ids)
    {
      auto it = m_transactionMap.find(tx_id);
      if (it == m_transactionMap.end())
        missed_txs.push_back(tx_id);
      else
        txs.push_back(transactionByIndex(it->second).tx);
    }
  }

  template <class t_ids_container, class t_tx_container, class t_missed_container>
  void Blockchain::getTransactions(const t_ids_container &txs_ids,
                                   t_tx_container &txs,
                                   t_missed_container &missed_txs,
                                   bool checkTxPool)
  {
    if (checkTxPool)
    {
      std::lock_guard<decltype(m_tx_pool)> txLock(m_tx_pool);
      getBlockchainTransactions(txs_ids, txs, missed_txs);
      auto poolTxIds = std::move(missed_txs);
      missed_txs.clear();
      m_tx_pool.getTransactions(poolTxIds, txs, missed_txs);
    }
    else
    {
      getBlockchainTransactions(txs_ids, txs, missed_txs);
    }
  }

} // namespace cn