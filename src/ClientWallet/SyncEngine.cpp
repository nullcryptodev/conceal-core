// SyncEngine - multi-strategy wallet sync implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SyncEngine.h"
#include "Blockchain/BlockchainFilter.h"
#include "BoltCore/BoltCoreTypes.h"
#include "BoltCore/OutputUtils.h"
#include "BoltRPC/StateManager.h"
#include "BoltRPC/SyncManager.h"
#include "BoltSync/BoltSync.h"
#include "BoltSync/BlockDeserializer.h"
#include "BoltCore/NewOutputScanner.h"
#include "Blockchain/BlockchainFilter.h"
#include "BoltSync/CryptoHelpers.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "Common/PathHelpers.h"
#include "Common/StringTools.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "crypto/crypto.h"
#include "INode.h"
#include "NodeClient/NodeClient.h"

#include <chrono>
#include <fstream>
#include <future>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <mdbx.h>

static std::mutex g_syncLogWriteMutex;

static void syncLog(const std::string &msg)
{
  std::lock_guard<std::mutex> lock(g_syncLogWriteMutex);
  std::ofstream log("/tmp/conceal-wallet-sync.log", std::ios::app);
  if (!log)
    return;
  log << msg << std::endl;
}

namespace ClientWallet
{

  namespace
  {
    constexpr const char *kWalletStateFile = "wallet_state.bin";

    struct OutputRef
    {
      crypto::Hash txHash;
      uint32_t outputIndex;

      bool operator==(const OutputRef &other) const
      {
        return txHash == other.txHash && outputIndex == other.outputIndex;
      }
    };

    struct OutputRefHash
    {
      size_t operator()(const OutputRef &ref) const
      {
        size_t hash = 0;
        for (size_t i = 0; i < sizeof(crypto::Hash); ++i)
          hash ^= static_cast<size_t>(ref.txHash.data[i]) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        hash ^= static_cast<size_t>(ref.outputIndex) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
        return hash;
      }
    };

    struct MsigOutputRef
    {
      crypto::Hash txHash;
      uint32_t outputIndex;
      uint32_t term;
    };

    bool looksLikeCliFlag(const std::string &s)
    {
      return s.size() >= 2 && s[0] == '-' && s[1] == '-';
    }

    // Use path as the wallet state file (--state FILE).
    std::string resolveStateFilePath(const std::string &path)
    {
      if (path.empty() || looksLikeCliFlag(path))
        return {};
      if (path.back() == '/' || path.back() == '\\')
        return PathHelpers::appendPath(path, kWalletStateFile);
      return path;
    }

    bool lookupGlobalOutputIndexInBlock(MDBX_env *env,
                                        uint32_t blockHeight,
                                        const crypto::Hash &txHash,
                                        uint32_t outputIndex,
                                        uint32_t &globalIndex)
    {
      MDBX_txn *rt = nullptr;
      if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rt) != MDBX_SUCCESS)
        return false;

      MDBX_dbi dbiBlockEntries = 0;
      if (mdbx_dbi_open(rt, "block_entries", MDBX_DB_DEFAULTS, &dbiBlockEntries) != MDBX_SUCCESS)
      {
        mdbx_txn_abort(rt);
        return false;
      }

      std::ostringstream keyStr;
      keyStr << "be_" << std::setw(8) << std::setfill('0') << blockHeight;
      std::string key = keyStr.str();
      MDBX_val bk = {const_cast<char *>(key.data()), key.size()};
      MDBX_val bv;

      bool ok = false;
      if (mdbx_get(rt, dbiBlockEntries, &bk, &bv) == MDBX_SUCCESS)
      {
        cn::BinaryArray ba(static_cast<const uint8_t *>(bv.iov_base),
                           static_cast<const uint8_t *>(bv.iov_base) + bv.iov_len);
        cn::Block block;
        std::vector<cn::Transaction> transactions;
        std::vector<std::vector<uint32_t>> globalIndexesPerTx;
        if (BoltSync::deserializeBlockEntry(ba, block, transactions, &globalIndexesPerTx))
        {
          for (size_t txIdx = 0; txIdx < transactions.size(); ++txIdx)
          {
            if (getObjectHash(transactions[txIdx]) != txHash)
              continue;
            if (txIdx >= globalIndexesPerTx.size())
              break;
            const auto &indexes = globalIndexesPerTx[txIdx];
            if (outputIndex < indexes.size())
            {
              globalIndex = indexes[outputIndex];
              ok = true;
            }
            break;
          }
        }
      }

      mdbx_txn_abort(rt);
      return ok;
    }

    bool lookupGlobalOutputIndicesViaNode(cn::INode *node,
                                          const crypto::Hash &txHash,
                                          std::vector<uint32_t> &indices)
    {
      indices.clear();
      if (!node)
        return false;

      std::promise<std::error_code> promise;
      auto future = promise.get_future();
      node->getTransactionOutsGlobalIndices(
          txHash, indices,
          [&promise](std::error_code ec)
          { promise.set_value(ec); });
      const std::error_code ec = future.get();
      return !ec && !indices.empty();
    }

    bool lookupGlobalOutputIndexViaNode(cn::INode *node,
                                        const BoltCore::OutputInfo &output,
                                        uint32_t &globalIndex)
    {
      std::vector<uint32_t> indices;
      if (!lookupGlobalOutputIndicesViaNode(node, output.txHash, indices))
        return false;
      if (output.outputIndex >= indices.size())
        return false;

      globalIndex = indices[output.outputIndex];
      return true;
    }

    // WalletGreen / TransfersContainer: globalOutputIndex = globalIndices[outputInTransaction].
    void applyGlobalIndicesToOutputs(std::vector<BoltCore::OutputInfo> &outputs,
                                     const std::vector<uint32_t> &globalIndices)
    {
      if (globalIndices.empty())
        return;
      for (auto &out : outputs)
      {
        if (out.outputIndex >= globalIndices.size())
          continue;
        out.globalOutputIndex = globalIndices[out.outputIndex];
        out.hasGlobalOutputIndex = true;
      }
    }

    bool fetchGlobalIndicesForConfirmedTx(cn::INode *node,
                                          NodeClient::NodeClient *nc,
                                          const crypto::Hash &txHash,
                                          uint32_t blockHeight,
                                          std::vector<uint32_t> &globalIndices)
    {
      globalIndices.clear();
      if (blockHeight == 0)
        return false;

      if (nc)
      {
        std::vector<cn::Transaction> txs;
        std::vector<crypto::Hash> hashes;
        std::vector<std::vector<uint32_t>> globalIndexesPerTx;
        if (nc->getTransactionsAtHeight(blockHeight, txs, hashes, &globalIndexesPerTx))
        {
          for (size_t i = 0; i < hashes.size(); ++i)
          {
            if (hashes[i] != txHash)
              continue;
            if (i < globalIndexesPerTx.size())
              globalIndices = globalIndexesPerTx[i];
            return !globalIndices.empty();
          }
        }
      }

      return lookupGlobalOutputIndicesViaNode(node, txHash, globalIndices);
    }

    std::vector<BoltCore::OutputInfo> backfillOutputsInCache(
        std::vector<BoltCore::OutputInfo> &cachedOutputs,
        cn::INode *node,
        MDBX_env *env,
        std::unordered_set<crypto::Hash, boost::hash<crypto::Hash>> *unresolvedTxHashes)
    {
      std::vector<BoltCore::OutputInfo> updated;
      for (auto &out : cachedOutputs)
      {
        if (out.spent || out.hasGlobalOutputIndex)
          continue;

        // Global amount indices exist only after the tx is included in a block.
        if (out.blockHeight == 0)
          continue;

        if (unresolvedTxHashes && unresolvedTxHashes->count(out.txHash) != 0)
          continue;

        uint32_t globalIndex = 0;
        bool resolved = false;
        if (env &&
            lookupGlobalOutputIndexInBlock(env, out.blockHeight, out.txHash, out.outputIndex, globalIndex))
          resolved = true;
        else if (lookupGlobalOutputIndexViaNode(node, out, globalIndex))
          resolved = true;

        if (!resolved)
        {
          if (unresolvedTxHashes)
            unresolvedTxHashes->insert(out.txHash);
          continue;
        }

        if (unresolvedTxHashes)
          unresolvedTxHashes->erase(out.txHash);

        out.globalOutputIndex = globalIndex;
        out.hasGlobalOutputIndex = true;
        updated.push_back(out);
      }
      return updated;
    }

    void dedupeOutputsByRef(std::vector<BoltCore::OutputInfo> &outputs)
    {
      std::vector<BoltCore::OutputInfo> unique;
      unique.reserve(outputs.size());
      for (const auto &out : outputs)
      {
        bool exists = false;
        for (const auto &existing : unique)
        {
          if (existing.txHash == out.txHash && existing.outputIndex == out.outputIndex)
          {
            exists = true;
            break;
          }
        }
        if (!exists)
          unique.push_back(out);
      }
      outputs.swap(unique);
    }

    void dedupeOutputsByKey(std::vector<BoltCore::OutputInfo> &outputs)
    {
      static const crypto::PublicKey NULL_KEY = {};
      std::vector<BoltCore::OutputInfo> unique;
      unique.reserve(outputs.size());
      for (const auto &out : outputs)
      {
        bool exists = false;
        for (const auto &existing : unique)
        {
          if (existing.txHash != out.txHash)
            continue;
          if (existing.outputIndex == out.outputIndex)
          {
            exists = true;
            break;
          }
          if (out.outputKey != NULL_KEY && existing.outputKey == out.outputKey)
          {
            exists = true;
            break;
          }
          if (out.hasGlobalOutputIndex && existing.hasGlobalOutputIndex &&
              out.globalOutputIndex == existing.globalOutputIndex)
          {
            exists = true;
            break;
          }
        }
        if (!exists)
          unique.push_back(out);
      }
      outputs.swap(unique);
    }

    bool outputsPhysicallyMatch(const BoltCore::OutputInfo &existing,
                                const BoltCore::OutputInfo &out)
    {
      static const crypto::PublicKey NULL_KEY = {};
      if (existing.txHash != out.txHash)
        return false;
      if (existing.isDeposit != out.isDeposit)
        return false;
      if (existing.outputIndex == out.outputIndex)
        return true;
      if (out.outputKey != NULL_KEY && existing.outputKey == out.outputKey)
        return true;
      if (out.hasGlobalOutputIndex && existing.hasGlobalOutputIndex &&
          out.globalOutputIndex == existing.globalOutputIndex)
        return true;
      if (!out.isDeposit && !existing.isDeposit && !out.spent && !existing.spent &&
          out.amount == existing.amount)
      {
        if (out.blockHeight > 0 && existing.blockHeight > 0 &&
            out.blockHeight == existing.blockHeight)
          return true;
        if (out.blockHeight == 0 || existing.blockHeight == 0)
          return true;
      }
      return false;
    }

    BoltCore::OutputInfo *findCachedOutputMatch(std::vector<BoltCore::OutputInfo> &cache,
                                                const BoltCore::OutputInfo &out)
    {
      for (auto &existing : cache)
      {
        if (outputsPhysicallyMatch(existing, out))
          return &existing;
      }

      if (!out.isDeposit && !out.spent)
      {
        BoltCore::OutputInfo *byTxAmount = nullptr;
        uint32_t sameTxAmount = 0;
        for (auto &existing : cache)
        {
          if (existing.txHash != out.txHash || existing.isDeposit || existing.spent)
            continue;
          if (existing.amount != out.amount)
            continue;
          if (out.blockHeight > 0 && existing.blockHeight > 0 &&
              out.blockHeight != existing.blockHeight)
            continue;
          byTxAmount = &existing;
          if (++sameTxAmount > 1)
            break;
        }
        if (sameTxAmount == 1)
          return byTxAmount;
      }

      return nullptr;
    }

    bool outputAlreadyCached(const BoltCore::OutputInfo &out,
                             const std::vector<BoltCore::OutputInfo> &cache)
    {
      std::vector<BoltCore::OutputInfo> mutableCache = cache;
      return findCachedOutputMatch(mutableCache, out) != nullptr;
    }

    void dedupeOutputsPhysically(std::vector<BoltCore::OutputInfo> &outputs)
    {
      std::vector<BoltCore::OutputInfo> unique;
      unique.reserve(outputs.size());
      for (const auto &out : outputs)
      {
        bool exists = false;
        for (const auto &existing : unique)
        {
          if (outputsPhysicallyMatch(existing, out))
          {
            exists = true;
            break;
          }
        }
        if (!exists)
          unique.push_back(out);
      }
      outputs.swap(unique);
    }
  }

  SyncEngine::SyncEngine(const std::string &dataDir,
                         const crypto::SecretKey &viewKey,
                         const crypto::PublicKey &spendPub,
                         const crypto::SecretKey *spendKey,
                         bool enableChainSync)
      : m_dataDir(dataDir), m_viewKey(viewKey), m_spendPub(spendPub), m_spendKey(spendKey),
        m_enableChainSync(enableChainSync)
  {
  }

  SyncEngine::~SyncEngine()
  {
    stop();
  }

  void SyncEngine::setNode(cn::INode *node) { m_node = node; }
  void SyncEngine::setDaemonRpc(std::function<std::string(const std::string &, const std::string &)> rpc) { m_daemonRpc = rpc; }

  // ── Lifecycle ──────────────────────────────────────────────────────────

  void SyncEngine::start(OutputCallback onOutputs, StatusCallback onStatus, SpentCallback onSpent,
                         MetadataCallback onMetadata)
  {
    syncLog("SyncEngine::start() called, dataDir=" + m_dataDir);
    m_onOutputs = std::move(onOutputs);
    m_onStatus = std::move(onStatus);
    m_onSpent = std::move(onSpent);
    m_onMetadata = std::move(onMetadata);
    if (!m_enableChainSync)
    {
      m_strategy = SyncStrategy::Offline;
      syncLog("Strategy selected: 3 (Offline, chain sync disabled)");
    }
    else
    {
      m_strategy = detectBestStrategy();
      syncLog("Strategy selected: " + std::to_string((int)m_strategy));
    }

    SyncStatus status;
    status.strategy = m_strategy;
    if (m_onStatus)
      m_onStatus(status);
    if (m_strategy != SyncStrategy::Offline)
      incrementalSync();
  }

  void SyncEngine::stop()
  {
    requestStop();
    std::lock_guard<std::mutex> lock(m_threadMutex);
    if (m_thread.joinable())
      m_thread.join();
    m_active = false;
  }

  // ── Strategy detection ─────────────────────────────────────────────────

  SyncStrategy SyncEngine::detectBestStrategy()
  {
    std::string dbPath = PathHelpers::appendPath(m_dataDir, "mdbx_blocks");
    std::ifstream testFile(dbPath, std::ios::binary);
    if (testFile.good())
    {
      testFile.close();
      syncLog("detectBestStrategy: database file exists, using DirectScan");
      return SyncStrategy::DirectScan;
    }
    syncLog("detectBestStrategy: no local database found");
    if (m_daemonRpc)
      return SyncStrategy::Polling;
    if (m_node)
      return SyncStrategy::Polling;
    return SyncStrategy::Offline;
  }

  // ── Manual triggers ────────────────────────────────────────────────────

  void SyncEngine::fullSync()
  {
    m_scannedHeight = 0;
    m_lastDaemonGapScanHeight = 0;
    m_multisigIndex.clear();
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      m_cachedOutputs.clear();
    }
    {
      std::lock_guard<std::mutex> lock(m_appliedTxMutex);
      m_walletAppliedTxHashes.clear();
    }
    incrementalSync();
  }

  void SyncEngine::incrementalSync()
  {
    std::lock_guard<std::mutex> lock(m_threadMutex);
    if (m_stop)
      return;
    if (m_active)
    {
      syncLog("incrementalSync: already active");
      return;
    }
    if (m_thread.joinable())
      m_thread.join();
    if (m_stop)
      return;

    syncLog("incrementalSync: starting background thread, strategy=" + std::to_string((int)m_strategy));
    m_active = true;
    m_stop = false;
    m_thread = std::thread([this]()
                           {
      syncLog("Background sync thread started");
      switch (m_strategy) {
        case SyncStrategy::DirectScan: runDirectScan(); break;
        case SyncStrategy::Polling:    runPollingSync(); break;
        case SyncStrategy::Offline:    break;
        default: syncLog("No strategy selected"); break;
      }
      syncLog("Background sync thread finished");
      m_active = false; });
  }

  void SyncEngine::onNewBlockHeight(uint32_t newHeight)
  {
    notifyDaemonHeight(newHeight);
    syncIfBehind();
  }

  void SyncEngine::notifyDaemonHeight(uint32_t height)
  {
    m_daemonHeight.store(height);
  }

  void SyncEngine::syncIfBehind()
  {
    if (!m_enableChainSync || m_strategy == SyncStrategy::Offline)
      return;

    const uint32_t mdbxTop = peekMdbxTopHeight();
    const uint32_t daemonHeight = m_daemonHeight.load();

    if (daemonHeight > mdbxTop && daemonHeight > m_lastLoggedMdbxLag)
    {
      const uint32_t walletHeight = m_scannedHeight.load();
      if (mdbxTop == 0)
      {
        syncLog("syncIfBehind: no local mdbx_blocks (mdbx=0); wallet at height " +
                std::to_string(walletHeight) + ", daemon tip " +
                std::to_string(daemonHeight > 0 ? daemonHeight - 1 : 0) +
                " — gap blocks fetched via daemon RPC");
      }
      else
      {
        syncLog("syncIfBehind: local MDBX behind daemon by " +
                std::to_string(daemonHeight - mdbxTop) +
                " blocks (daemon=" + std::to_string(daemonHeight) +
                ", mdbx=" + std::to_string(mdbxTop) +
                ", wallet=" + std::to_string(walletHeight) + ")");
      }
      m_lastLoggedMdbxLag = daemonHeight;
    }

    if (mdbxTop > m_scannedHeight && !m_stop)
      incrementalSync();
  }

  uint32_t SyncEngine::peekMdbxTopHeight() const
  {
    const std::string dbPath = PathHelpers::appendPath(m_dataDir, "mdbx_blocks");
    std::ifstream testFile(dbPath, std::ios::binary);
    if (!testFile.good())
      return 0;
    testFile.close();

    MDBX_env *env = nullptr;
    if (mdbx_env_create(&env) != MDBX_SUCCESS)
      return 0;

    mdbx_env_set_maxdbs(env, 8);
    if (mdbx_env_open(env, dbPath.c_str(), MDBX_NOSUBDIR | MDBX_NORDAHEAD, 0664) != MDBX_SUCCESS)
    {
      mdbx_env_close(env);
      return 0;
    }

    uint32_t topHeight = 0;
    MDBX_txn *ht = nullptr;
    MDBX_dbi hdbi;
    if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &ht) == MDBX_SUCCESS &&
        mdbx_dbi_open(ht, "heights", MDBX_DB_DEFAULTS, &hdbi) == MDBX_SUCCESS)
    {
      MDBX_stat st;
      if (mdbx_dbi_stat(ht, hdbi, &st, sizeof(st)) == MDBX_SUCCESS && st.ms_entries > 0)
        topHeight = static_cast<uint32_t>(st.ms_entries - 1);
      mdbx_txn_abort(ht);
    }

    mdbx_env_close(env);
    return topHeight;
  }

  // ── Direct scan (local MDBX with view-tag filter) ──────────────────────

  void SyncEngine::runDirectScan()
  {
    syncLog("runDirectScan: starting with two-pass filter");
    SyncStatus status;
    status.strategy = m_strategy;
    status.isSyncing = true;

    std::string dbPath = PathHelpers::appendPath(m_dataDir, "mdbx_blocks");

    MDBX_env *env = nullptr;
    int rc = mdbx_env_create(&env);
    if (rc != MDBX_SUCCESS)
    {
      syncLog("runDirectScan: mdbx_env_create failed");
      status.isSyncing = false;
      if (m_onStatus)
        m_onStatus(status);
      return;
    }

    mdbx_env_set_maxdbs(env, 8);
    mdbx_env_set_geometry(env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);
    rc = mdbx_env_open(env, dbPath.c_str(), MDBX_NOSUBDIR | MDBX_NORDAHEAD, 0664);
    if (rc != MDBX_SUCCESS)
    {
      syncLog("runDirectScan: mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));
      mdbx_env_close(env);
      status.isSyncing = false;
      if (m_onStatus)
        m_onStatus(status);
      return;
    }

    MDBX_dbi dbiBlockEntries, dbiFilterRecords;
    {
      MDBX_txn *t = nullptr;
      rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &t);
      if (rc == MDBX_SUCCESS)
      {
        mdbx_dbi_open(t, "block_entries", MDBX_DB_DEFAULTS, &dbiBlockEntries);
        mdbx_dbi_open(t, "filter_records", MDBX_DB_DEFAULTS, &dbiFilterRecords);
        mdbx_txn_abort(t);
      }
    }

    uint32_t topHeight = 0;
    {
      MDBX_txn *ht = nullptr;
      MDBX_dbi hdbi;
      if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &ht) == MDBX_SUCCESS &&
          mdbx_dbi_open(ht, "heights", MDBX_DB_DEFAULTS, &hdbi) == MDBX_SUCCESS)
      {
        MDBX_stat st;
        if (mdbx_dbi_stat(ht, hdbi, &st, sizeof(st)) == MDBX_SUCCESS && st.ms_entries > 0)
          topHeight = static_cast<uint32_t>(st.ms_entries - 1);
        mdbx_txn_abort(ht);
      }
    }

    syncLog("runDirectScan: topHeight=" + std::to_string(topHeight));

    const uint32_t daemonHeight = m_daemonHeight.load();
    if (daemonHeight > topHeight)
    {
      syncLog("runDirectScan: MDBX behind daemon by " +
              std::to_string(daemonHeight - topHeight) +
              " blocks (daemon=" + std::to_string(daemonHeight) +
              ", mdbx=" + std::to_string(topHeight) + ")");
    }

    if (m_scannedHeight >= topHeight)
    {
      // Re-scan the tip block when already caught up (picks up outputs missed by older scanners).
      if (m_scannedHeight == topHeight && topHeight > 0)
      {
        m_scannedHeight = topHeight - 1;
      }
      else
      {
        bool needsBackfill = false;
        {
          std::lock_guard<std::mutex> lock(m_outputCacheMutex);
          for (const auto &out : m_cachedOutputs)
          {
            if (!out.spent && !out.hasGlobalOutputIndex)
            {
              needsBackfill = true;
              break;
            }
          }
        }

        if (needsBackfill)
        {
          std::vector<BoltCore::OutputInfo> backfilled;
          {
            std::lock_guard<std::mutex> lock(m_outputCacheMutex);
            std::lock_guard<std::mutex> backfillLock(m_backfillMutex);
            backfilled = backfillOutputsInCache(m_cachedOutputs, m_node, env, &m_unresolvedGlobalIndexTxs);
          }
          if (!backfilled.empty())
          {
            syncLog("runDirectScan: backfilled " + std::to_string(backfilled.size()) +
                    " global indices (cache only)");
            if (m_onMetadata)
              m_onMetadata(backfilled);
          }
        }

        mdbx_env_close(env);
        markSpentOutputs(topHeight);
        status.isSyncing = false;
        if (m_onStatus)
          m_onStatus(status);
        return;
      }
    }

    std::vector<BoltCore::OutputInfo> outputs;
    uint32_t checked = 0;

    // Main scan loop
    for (uint32_t h = m_scannedHeight + 1; h <= topHeight && !m_stop; ++h)
    {
      MDBX_txn *rt = nullptr;
      if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rt) != MDBX_SUCCESS)
        continue;

      std::ostringstream keyStr;
      keyStr << "be_" << std::setw(8) << std::setfill('0') << h;
      std::string key = keyStr.str();
      MDBX_val bk = {const_cast<char *>(key.data()), key.size()};
      MDBX_val bv;

      if (mdbx_get(rt, dbiBlockEntries, &bk, &bv) == MDBX_SUCCESS)
      {
        cn::BinaryArray ba(static_cast<const uint8_t *>(bv.iov_base),
                           static_cast<const uint8_t *>(bv.iov_base) + bv.iov_len);
        cn::Block block;
        std::vector<cn::Transaction> transactions;
        std::vector<std::vector<uint32_t>> globalIndexesPerTx;
        if (BoltSync::deserializeBlockEntry(ba, block, transactions, &globalIndexesPerTx))
        {
          cn::BlockFilterRecord filterRecord;
          bool haveFilter = false;
          size_t filterIdx = 0;
          {
            std::ostringstream frKeyStr;
            frKeyStr << "fr_" << std::setw(8) << std::setfill('0') << h;
            std::string frKey = frKeyStr.str();
            MDBX_val fk = {const_cast<char *>(frKey.data()), frKey.size()};
            MDBX_val fv;
            if (mdbx_get(rt, dbiFilterRecords, &fk, &fv) == MDBX_SUCCESS)
            {
              cn::BinaryArray filterBa(
                  static_cast<const uint8_t *>(fv.iov_base),
                  static_cast<const uint8_t *>(fv.iov_base) + fv.iov_len);
              if (cn::fromBinaryArray(filterRecord, filterBa))
                haveFilter = true;
            }
          }

          // Filter order is baseTransaction then tx[1..n]; transactions[0] is the coinbase
          // (same as baseTransaction). Scan transactions[] only — never both.
          for (size_t txIdx = 0; txIdx < transactions.size(); ++txIdx)
          {
            const std::vector<uint32_t> *indexes = txIdx < globalIndexesPerTx.size()
                                                       ? &globalIndexesPerTx[txIdx]
                                                       : nullptr;
            const crypto::Hash blockTxHash = cn::getObjectHash(transactions[txIdx]);
            scanBlockTransaction(transactions[txIdx], h, indexes, outputs,
                                 haveFilter ? &filterRecord : nullptr, haveFilter, filterIdx,
                                 &blockTxHash);
          }
        }
      }
      mdbx_txn_abort(rt);

      checked++;
      m_scannedHeight = h;

      if (checked % 1000 == 0 && m_onStatus)
      {
        status.scannedHeight = h;
        status.currentHeight = topHeight;
        m_onStatus(status);
      }

      if (checked % 100000 == 0)
      {
        syncLog("runDirectScan: checked " + std::to_string(checked) +
                " blocks, found " + std::to_string(outputs.size()));
        if (!outputs.empty())
        {
          syncLog("runDirectScan: dispatching " + std::to_string(outputs.size()) + " outputs");
          dispatchDiscoveredOutputs(outputs);
          outputs.clear();
        }
        status.scannedHeight = checked;
        status.currentHeight = topHeight;
        if (m_onStatus)
          m_onStatus(status);
      }
    }

    syncLog("runDirectScan: checked " + std::to_string(checked) +
            " blocks, found " + std::to_string(outputs.size()) + " owned outputs");

    if (!outputs.empty())
      dispatchDiscoveredOutputs(outputs);

    m_scannedHeight = topHeight;
    status.scannedHeight = m_scannedHeight;
    status.ownedOutputs = static_cast<uint32_t>(m_cachedOutputs.size());

    const auto backfilled = [this, &env]()
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      std::lock_guard<std::mutex> backfillLock(m_backfillMutex);
      return backfillOutputsInCache(m_cachedOutputs, m_node, env, &m_unresolvedGlobalIndexTxs);
    }();
    if (!backfilled.empty())
    {
      syncLog("runDirectScan: backfilled " + std::to_string(backfilled.size()) +
              " global indices (cache only)");
      if (m_onMetadata)
        m_onMetadata(backfilled);
    }

    mdbx_env_close(env);

    markSpentOutputs(m_scannedHeight);

    status.isSyncing = false;
    if (m_onStatus)
      m_onStatus(status);

    // MDBX can advance while a long rescan runs — schedule a catch-up pass.
    const uint32_t liveTop = peekMdbxTopHeight();
    if (liveTop > m_scannedHeight && !m_stop)
    {
      syncLog("runDirectScan: MDBX grew during scan to " + std::to_string(liveTop) +
              " (scanned through " + std::to_string(m_scannedHeight) + "), scheduling catch-up");
      incrementalSync();
    }
  }

  // ── Polling sync ───────────────────────────────────────────────────────

  void SyncEngine::runPollingSync()
  {
    syncLog("runPollingSync: starting");
    if (!m_node || !m_daemonRpc)
    {
      syncLog("runPollingSync: missing node or RPC callback");
      return;
    }

    SyncStatus status;
    status.strategy = m_strategy;
    status.isSyncing = true;
    if (m_onStatus)
      m_onStatus(status);

    BoltRPC::SyncManager syncMgr(*m_node, m_viewKey, m_spendPub, m_dataDir, m_daemonRpc);

    const uint32_t walletHeight = m_scannedHeight.load();
    if (walletHeight > 0)
    {
      syncMgr.seedScannedHeight(walletHeight);
      syncLog("runPollingSync: seeded SyncManager at height " + std::to_string(walletHeight));
    }

    syncMgr.start(
        [this, &status](const BoltRPC::SyncProgress &progress)
        {
          status.scannedHeight = progress.currentHeight;
          status.currentHeight = progress.currentHeight;
          if (m_onStatus)
            m_onStatus(status);
          // Keep m_scannedHeight in sync with SyncManager's position so that
          // saveStateFile persists the correct height and a future seedScannedHeight
          // call starts the next SyncManager at the right block (not at genesis).
          if (progress.currentHeight > m_scannedHeight.load(std::memory_order_relaxed))
            m_scannedHeight.store(progress.currentHeight, std::memory_order_relaxed);
        },
        [this](const std::vector<BoltRPC::OutputInfo> &newOutputs,
               const std::vector<crypto::KeyImage> &spentKeyImages)
        {
          static const crypto::KeyImage NULL_KI = {};
          std::vector<BoltCore::OutputInfo> converted;
          converted.reserve(newOutputs.size());
          for (const auto &rpcOut : newOutputs)
          {
            BoltCore::OutputInfo info;
            info.blockHeight = rpcOut.blockHeight;
            info.txHash = rpcOut.txHash;
            info.amount = rpcOut.amount;
            info.outputIndex = rpcOut.outputIndex;
            info.globalOutputIndex = rpcOut.globalOutputIndex;
            info.hasGlobalOutputIndex = rpcOut.hasGlobalOutputIndex;
            info.outputKey = rpcOut.outputKey;
            info.txPublicKey = rpcOut.txPublicKey;
            info.keyImage = rpcOut.keyImage;
            info.spent = rpcOut.spent;
            info.isDeposit = rpcOut.isDeposit;
            info.term = rpcOut.term;
            info.keyDerivationIndex = rpcOut.keyDerivationIndex;
            info.hasKeyDerivationIndex = rpcOut.hasKeyDerivationIndex;

            // SyncManager doesn't have the spend key, so key images are NULL.
            // Compute them here so spend detection can match them later.
            if (m_spendKey && !info.isDeposit && info.blockHeight > 0 &&
                std::memcmp(&info.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) == 0)
            {
              crypto::KeyDerivation deriv;
              if (crypto::generate_key_derivation(info.txPublicKey, m_viewKey, deriv))
              {
                const uint32_t derivIdx = info.hasKeyDerivationIndex
                    ? info.keyDerivationIndex : info.outputIndex;
                crypto::SecretKey outSec = BoltSync::deriveOutputSecretKey(
                    deriv, derivIdx, *m_spendKey);
                crypto::generate_key_image(info.outputKey, outSec, info.keyImage);
              }
            }

            converted.push_back(info);
          }
          if (!converted.empty())
            dispatchDiscoveredOutputs(converted);

          // Match received key images against our cached outputs and mark them spent.
          if (!spentKeyImages.empty())
          {
            std::vector<crypto::KeyImage> matchedKIs;
            std::vector<std::pair<crypto::Hash, uint32_t>> outputSpends;
            std::vector<std::pair<crypto::Hash, uint32_t>> depositSpends;

            static const crypto::KeyImage NULL_KI = {};
            std::lock_guard<std::mutex> lock(m_outputCacheMutex);
            std::unordered_map<crypto::KeyImage, size_t> kiIndex;
            std::unordered_map<OutputRef, size_t, OutputRefHash> depIndex;
            for (size_t i = 0; i < m_cachedOutputs.size(); ++i)
            {
              const auto &fo = m_cachedOutputs[i];
              if (fo.isDeposit && !fo.spent)
                depIndex[{fo.txHash, fo.outputIndex}] = i;
              if (fo.isDeposit || fo.spent)
                continue;
              if (std::memcmp(&fo.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) != 0)
                kiIndex[fo.keyImage] = i;
            }

            for (const auto &ki : spentKeyImages)
            {
              auto it = kiIndex.find(ki);
              if (it != kiIndex.end() && !m_cachedOutputs[it->second].spent)
              {
                m_cachedOutputs[it->second].spent = true;
                matchedKIs.push_back(ki);
                const auto &spentOut = m_cachedOutputs[it->second];
                outputSpends.emplace_back(spentOut.txHash, spentOut.outputIndex);
              }
            }

            if (m_onSpent && (!matchedKIs.empty() || !depositSpends.empty()))
              m_onSpent(matchedKIs, depositSpends, outputSpends);
          }
        });

    while (!m_stop)
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    syncMgr.stop();
    status.isSyncing = false;
    if (m_onStatus)
      m_onStatus(status);
    syncLog("runPollingSync: finished");
  }

  // ── Inline spend detection (works without MDBX) ──────────────────────

  SyncEngine::SpendDetectionResult SyncEngine::detectSpendsInTransactions(
      const std::vector<cn::Transaction> &txs)
  {
    // Called from two sites:
    //  1. scanDaemonGapBlocks — confirmed blocks beyond wallet tip (both strategies).
    //     When SyncManager (Polling) advances m_scannedHeight via its progress
    //     callback, this starts where SyncManager left off.  When SyncManager
    //     is unavailable, this is the sole gap-filler.
    //  2. pollMempool — new-to-pool transactions, both strategies.
    //     Enables spend detection at mempool time, not just at block confirmation.
    SpendDetectionResult result;
    if (txs.empty())
      return result;

    std::lock_guard<std::mutex> lock(m_outputCacheMutex);
    if (m_cachedOutputs.empty())
      return result;

    // Build lookup: keyImage → cache index (for regular outputs)
    // Build lookup: (txHash, outputIndex) → cache index (for deposits)
    static const crypto::KeyImage NULL_KI = {};
    std::unordered_map<crypto::KeyImage, size_t> kiIndex;
    std::unordered_map<OutputRef, size_t, OutputRefHash> depIndex;
    for (size_t i = 0; i < m_cachedOutputs.size(); ++i)
    {
      const auto &fo = m_cachedOutputs[i];
      if (fo.isDeposit && !fo.spent)
        depIndex[{fo.txHash, fo.outputIndex}] = i;
      if (fo.isDeposit || fo.spent)
        continue;
      if (std::memcmp(&fo.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) != 0)
        kiIndex[fo.keyImage] = i;
    }

    if (kiIndex.empty() && depIndex.empty())
      return result;

    uint32_t spentCount = 0;
    for (const auto &tx : txs)
    {
      for (const auto &input : tx.inputs)
      {
        if (input.type() == typeid(cn::KeyInput))
        {
          const auto &ki = boost::get<cn::KeyInput>(input).keyImage;
          auto it = kiIndex.find(ki);
          if (it != kiIndex.end() && !m_cachedOutputs[it->second].spent)
          {
            m_cachedOutputs[it->second].spent = true;
            result.spentKeyImages.push_back(ki);
            const auto &spentOut = m_cachedOutputs[it->second];
            result.outputSpends.emplace_back(spentOut.txHash, spentOut.outputIndex);
            spentCount++;
          }
        }
        else if (input.type() == typeid(cn::MultisignatureInput))
        {
          const auto &ms = boost::get<cn::MultisignatureInput>(input);
          for (auto dit = depIndex.begin(); dit != depIndex.end();)
          {
            const auto &depOut = m_cachedOutputs[dit->second];
            if (depOut.spent)
            {
              dit = depIndex.erase(dit);
              continue;
            }
            if (depOut.amount != ms.amount)
            {
              ++dit;
              continue;
            }
            if (ms.term != 0 && depOut.term != ms.term)
            {
              ++dit;
              continue;
            }
            if (!depOut.hasGlobalOutputIndex || depOut.globalOutputIndex != ms.outputIndex)
            {
              ++dit;
              continue;
            }
            auto &spent = m_cachedOutputs[dit->second];
            spent.spent = true;
            result.depositSpends.emplace_back(spent.txHash, spent.outputIndex);
            dit = depIndex.erase(dit);
            spentCount++;
          }
        }
      }
    }

    if (spentCount > 0)
      syncLog("detectSpendsInTransactions: marked " + std::to_string(spentCount) + " outputs as spent");

    return result;
  }

  // ── Mark spent outputs ─────────────────────────────────────────────────

  void SyncEngine::markSpentOutputs(uint32_t topHeight)
  {
    // Only valid for DirectScan: requires the local MDBX block store.
    // In Polling mode, spend detection is handled by SyncManager's key-image
    // collection (via the output callback) and by detectSpendsInTransactions
    // called from both scanDaemonGapBlocks and pollMempool.
    if (m_strategy == SyncStrategy::Polling)
    {
      syncLog("markSpentOutputs: skipped (Polling mode — spend detection via SyncManager)");
      return;
    }

    const uint32_t mdbxTop = peekMdbxTopHeight();
    if (mdbxTop == 0)
    {
      syncLog("markSpentOutputs: skipped (no local mdbx_blocks; wallet at " +
              std::to_string(m_scannedHeight.load()) + ")");
      return;
    }

    uint32_t startHeight = m_lastSpentScanHeight + 1;
    if (startHeight > topHeight)
    {
      syncLog("markSpentOutputs: up to date through block " + std::to_string(topHeight));
      return;
    }

    syncLog("markSpentOutputs: scanning blocks " + std::to_string(startHeight) +
            ".." + std::to_string(topHeight) +
            " (wallet=" + std::to_string(m_scannedHeight.load()) +
            ", mdbx=" + std::to_string(mdbxTop) + ")");

    std::vector<crypto::KeyImage> newKeyImages;
    std::vector<std::pair<crypto::Hash, uint32_t>> newDepositSpends;
    std::vector<std::pair<crypto::Hash, uint32_t>> newOutputSpends;

    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      if (m_cachedOutputs.empty())
        return;

      std::unordered_map<crypto::KeyImage, size_t> keyImageIndex;
      std::unordered_map<OutputRef, size_t, OutputRefHash> depositIndex;
      for (size_t i = 0; i < m_cachedOutputs.size(); ++i)
      {
        const auto &fo = m_cachedOutputs[i];
        if (fo.isDeposit && !fo.spent)
          depositIndex[{fo.txHash, fo.outputIndex}] = i;
        if (fo.isDeposit || fo.spent)
          continue;
        static const crypto::KeyImage NULL_KI = {};
        if (std::memcmp(&fo.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) != 0)
          keyImageIndex[fo.keyImage] = i;
      }

      std::string dbPath = PathHelpers::appendPath(m_dataDir, "mdbx_blocks");
      MDBX_env *env = nullptr;
      if (mdbx_env_create(&env) != MDBX_SUCCESS)
        return;
      mdbx_env_set_maxdbs(env, 8);
      mdbx_env_set_geometry(env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);
      if (mdbx_env_open(env, dbPath.c_str(), MDBX_NOSUBDIR | MDBX_NORDAHEAD, 0664) != MDBX_SUCCESS)
      {
        mdbx_env_close(env);
        return;
      }

      MDBX_dbi dbi;
      {
        MDBX_txn *t = nullptr;
        int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &t);
        if (rc == MDBX_SUCCESS)
        {
          rc = mdbx_dbi_open(t, "block_entries", MDBX_DB_DEFAULTS, &dbi);
          mdbx_txn_abort(t);
        }
        if (rc != MDBX_SUCCESS)
        {
          mdbx_env_close(env);
          return;
        }
      }

      auto markDepositSpentAt = [&](size_t index)
      {
        auto &out = m_cachedOutputs[index];
        if (out.spent)
          return;
        out.spent = true;
        newDepositSpends.emplace_back(out.txHash, out.outputIndex);
      };

      uint32_t spentCount = 0;
      for (uint32_t h = startHeight; h <= topHeight; ++h)
      {
        if (m_stop)
          break;

        MDBX_txn *rt = nullptr;
        if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rt) != MDBX_SUCCESS)
          continue;

        std::ostringstream keyStr;
        keyStr << "be_" << std::setw(8) << std::setfill('0') << h;
        std::string key = keyStr.str();
        MDBX_val k = {const_cast<char *>(key.data()), key.size()};
        MDBX_val v;

        if (mdbx_get(rt, dbi, &k, &v) == MDBX_SUCCESS)
        {
          cn::BinaryArray ba(static_cast<const uint8_t *>(v.iov_base),
                             static_cast<const uint8_t *>(v.iov_base) + v.iov_len);
          cn::Block block;
          std::vector<cn::Transaction> transactions;
          if (BoltSync::deserializeBlockEntry(ba, block, transactions))
          {
            auto processTransaction = [&](const cn::Transaction &tx)
            {
              const crypto::Hash txHash = cn::getObjectHash(tx);

              for (const auto &input : tx.inputs)
              {
                if (input.type() == typeid(cn::KeyInput))
                {
                  const auto &ki = boost::get<cn::KeyInput>(input).keyImage;
                  auto it = keyImageIndex.find(ki);
                  if (it != keyImageIndex.end() && !m_cachedOutputs[it->second].spent)
                  {
                    const auto &spentOut = m_cachedOutputs[it->second];
                    m_cachedOutputs[it->second].spent = true;
                    newKeyImages.push_back(ki);
                    if (!spentOut.isDeposit)
                      newOutputSpends.emplace_back(spentOut.txHash, spentOut.outputIndex);
                    spentCount++;
                  }
                }
                else if (input.type() == typeid(cn::MultisignatureInput))
                {
                  const auto &ms = boost::get<cn::MultisignatureInput>(input);

                  for (auto it = depositIndex.begin(); it != depositIndex.end();)
                  {
                    const auto &depOut = m_cachedOutputs[it->second];
                    if (depOut.spent)
                    {
                      it = depositIndex.erase(it);
                      continue;
                    }
                    if (depOut.amount != ms.amount)
                    {
                      ++it;
                      continue;
                    }
                    if (ms.term != 0 && depOut.term != ms.term)
                    {
                      ++it;
                      continue;
                    }
                    if (!depOut.hasGlobalOutputIndex ||
                        depOut.globalOutputIndex != ms.outputIndex)
                    {
                      ++it;
                      continue;
                    }

                    markDepositSpentAt(it->second);
                    it = depositIndex.erase(it);
                    spentCount++;
                  }
                }
              }

              (void)txHash;
            };

            for (const auto &tx : transactions)
              processTransaction(tx);
          }
        }
        mdbx_txn_abort(rt);
      }

      syncLog("markSpentOutputs: marked " + std::to_string(spentCount) + " outputs as spent");
      mdbx_env_close(env);
    }

    m_lastSpentScanHeight = topHeight;

    if (m_onSpent && (!newKeyImages.empty() || !newDepositSpends.empty() || !newOutputSpends.empty()))
      m_onSpent(newKeyImages, newDepositSpends, newOutputSpends);
  }

  // ── State file ─────────────────────────────────────────────────────────

  std::vector<BoltCore::OutputInfo> SyncEngine::backfillMissingGlobalOutputIndices()
  {
    MDBX_env *env = nullptr;
    bool ownEnv = false;
    const std::string dbPath = PathHelpers::appendPath(m_dataDir, "mdbx_blocks");
    std::ifstream testFile(dbPath, std::ios::binary);
    if (testFile.good())
    {
      testFile.close();
      if (mdbx_env_create(&env) == MDBX_SUCCESS)
      {
        mdbx_env_set_maxdbs(env, 8);
        mdbx_env_set_geometry(env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);
        if (mdbx_env_open(env, dbPath.c_str(), MDBX_NOSUBDIR | MDBX_NORDAHEAD, 0664) == MDBX_SUCCESS)
          ownEnv = true;
        else
        {
          mdbx_env_close(env);
          env = nullptr;
        }
      }
    }

    std::vector<BoltCore::OutputInfo> updated;
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      std::lock_guard<std::mutex> backfillLock(m_backfillMutex);
      updated = backfillOutputsInCache(m_cachedOutputs, m_node, env, &m_unresolvedGlobalIndexTxs);
    }

    if (ownEnv && env)
      mdbx_env_close(env);

    if (!updated.empty())
      syncLog("backfillMissingGlobalOutputIndices: resolved " + std::to_string(updated.size()) + " outputs");

    return updated;
  }

  bool SyncEngine::lookupGlobalOutputIndex(const BoltCore::OutputInfo &out,
                                           uint32_t &globalIndex) const
  {
    globalIndex = 0;
    if (out.blockHeight == 0)
      return false;

    const std::string dbPath = PathHelpers::appendPath(m_dataDir, "mdbx_blocks");
    MDBX_env *env = nullptr;
    bool ownEnv = false;
    std::ifstream testFile(dbPath, std::ios::binary);
    if (testFile.good())
    {
      testFile.close();
      if (mdbx_env_create(&env) == MDBX_SUCCESS)
      {
        mdbx_env_set_maxdbs(env, 8);
        mdbx_env_set_geometry(env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);
        if (mdbx_env_open(env, dbPath.c_str(), MDBX_NOSUBDIR | MDBX_NORDAHEAD, 0664) == MDBX_SUCCESS)
          ownEnv = true;
        else
        {
          mdbx_env_close(env);
          env = nullptr;
        }
      }
    }

    if (env && lookupGlobalOutputIndexInBlock(env, out.blockHeight, out.txHash, out.outputIndex,
                                              globalIndex))
    {
      if (ownEnv)
        mdbx_env_close(env);
      return true;
    }

    if (ownEnv && env)
      mdbx_env_close(env);

    return lookupGlobalOutputIndexViaNode(m_node, out, globalIndex);
  }

  void SyncEngine::noteIncomingTxApplied(const crypto::Hash &txHash)
  {
    std::lock_guard<std::mutex> lock(m_appliedTxMutex);
    m_walletAppliedTxHashes.insert(txHash);
  }

  void SyncEngine::notifyOutputsSpent(const std::vector<BoltCore::OutputInfo> &spentInputs)
  {
    if (spentInputs.empty())
      return;
    static const crypto::KeyImage NULL_KI = {};
    std::lock_guard<std::mutex> lock(m_outputCacheMutex);
    for (const auto &spent : spentInputs)
    {
      for (auto &cached : m_cachedOutputs)
      {
        if (cached.txHash != spent.txHash || cached.outputIndex != spent.outputIndex || cached.spent)
          continue;
        cached.spent = true;
        // Capture key image computed during signing so MDBX spend detection works later.
        if (std::memcmp(&cached.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) == 0
            && std::memcmp(&spent.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) != 0)
        {
          cached.keyImage = spent.keyImage;
        }
        break;
      }
    }
  }

  void SyncEngine::dispatchDiscoveredOutputs(const std::vector<BoltCore::OutputInfo> &outputs)
  {
    if (outputs.empty())
      return;

    std::vector<BoltCore::OutputInfo> scanned = outputs;
    dedupeOutputsPhysically(scanned);

    std::vector<BoltCore::OutputInfo> novel;
    novel.reserve(scanned.size());
    std::vector<BoltCore::OutputInfo> metadataUpdates;

    for (const auto &out : scanned)
    {
      bool alreadyCached = false;
      {
        std::lock_guard<std::mutex> appliedLock(m_appliedTxMutex);
        if (m_walletAppliedTxHashes.count(out.txHash))
          alreadyCached = true;
      }
      if (!alreadyCached)
      {
        std::lock_guard<std::mutex> lock(m_outputCacheMutex);
        alreadyCached = outputAlreadyCached(out, m_cachedOutputs);
      }

      BoltCore::OutputInfo indexUpdate;
      mergeCachedOutput(out, &indexUpdate);
      if (indexUpdate.hasGlobalOutputIndex)
        metadataUpdates.push_back(std::move(indexUpdate));

      if (!alreadyCached)
        novel.push_back(out);
    }

    if (!novel.empty() && m_onOutputs)
    {
      syncLog("dispatchDiscoveredOutputs: " + std::to_string(novel.size()) + " new of " +
              std::to_string(scanned.size()) + " scanned");
      m_onOutputs(novel);
    }
    else if (!scanned.empty())
    {
      syncLog("dispatchDiscoveredOutputs: 0 new of " + std::to_string(scanned.size()) +
              " scanned (wallet/cache already have this tx)");
    }

    if (!metadataUpdates.empty() && m_onMetadata)
      m_onMetadata(metadataUpdates);
  }

  void SyncEngine::mergeCachedOutput(const BoltCore::OutputInfo &out,
                                     BoltCore::OutputInfo *walletIndexUpdate)
  {
    if (walletIndexUpdate)
      *walletIndexUpdate = {};
    std::lock_guard<std::mutex> lock(m_outputCacheMutex);
    if (BoltCore::OutputInfo *existing = findCachedOutputMatch(m_cachedOutputs, out))
    {
      if (out.blockHeight > 0 && existing->blockHeight == 0)
      {
        existing->blockHeight = out.blockHeight;
        std::lock_guard<std::mutex> backfillLock(m_backfillMutex);
        m_unresolvedGlobalIndexTxs.erase(out.txHash);
      }

      if (out.hasGlobalOutputIndex && !existing->hasGlobalOutputIndex)
      {
        existing->globalOutputIndex = out.globalOutputIndex;
        existing->hasGlobalOutputIndex = true;
        if (walletIndexUpdate)
          *walletIndexUpdate = *existing;
      }

      if (existing->outputKey == crypto::PublicKey{} && out.outputKey != crypto::PublicKey{})
        existing->outputKey = out.outputKey;

      if (out.hasGlobalOutputIndex)
      {
        std::lock_guard<std::mutex> backfillLock(m_backfillMutex);
        m_unresolvedGlobalIndexTxs.erase(out.txHash);
      }
      return;
    }

    m_cachedOutputs.push_back(out);
    if (out.hasGlobalOutputIndex)
    {
      std::lock_guard<std::mutex> backfillLock(m_backfillMutex);
      m_unresolvedGlobalIndexTxs.erase(out.txHash);
    }
  }

  bool SyncEngine::loadStateFile(const std::string &path)
  {
    const std::string filePath = resolveStateFilePath(path);
    if (filePath.empty())
    {
      syncLog("loadStateFile: invalid path \"" + path + "\"");
      return false;
    }
    m_walletStateBinPath = filePath;
    syncLog("loadStateFile: " + filePath);
    BoltRPC::StateManager mgr(filePath, true);
    BoltRPC::WalletState state;
    if (!mgr.load(state))
      return false;
    m_scannedHeight = state.lastHeight;
    m_lastSpentScanHeight = state.lastHeight;

    std::vector<BoltCore::OutputInfo> converted;
    converted.reserve(state.ownedOutputs.size());
    for (const auto &rpcOut : state.ownedOutputs)
    {
      BoltCore::OutputInfo info;
      info.blockHeight = rpcOut.blockHeight;
      info.txHash = rpcOut.txHash;
      info.amount = rpcOut.amount;
      info.outputIndex = rpcOut.outputIndex;
      info.globalOutputIndex = rpcOut.globalOutputIndex;
      info.hasGlobalOutputIndex = rpcOut.hasGlobalOutputIndex;
      info.outputKey = rpcOut.outputKey;
      info.txPublicKey = rpcOut.txPublicKey;
      info.keyImage = rpcOut.keyImage;
      info.spent = rpcOut.spent;
      info.isDeposit = rpcOut.isDeposit;
      info.term = rpcOut.term;
      converted.push_back(info);
    }
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      m_cachedOutputs = converted;
    }
    {
      std::lock_guard<std::mutex> lock(m_appliedTxMutex);
      m_walletAppliedTxHashes.clear();
      for (const auto &out : converted)
      {
        if (out.blockHeight > 0)
          m_walletAppliedTxHashes.insert(out.txHash);
      }
    }
    syncLog("loadStateFile: loaded " + std::to_string(converted.size()) + " outputs at height " + std::to_string(state.lastHeight));
    return true;
  }

  bool SyncEngine::saveStateFile(const std::string &path,
                                 const std::vector<BoltCore::OutputInfo> &outputs)
  {
    const std::string filePath = resolveStateFilePath(path);
    if (filePath.empty())
      return false;
    m_walletStateBinPath = filePath;
    syncLog("saveStateFile: " + filePath);
    BoltRPC::StateManager mgr(filePath, true);
    BoltRPC::WalletState state;
    state.lastHeight = m_scannedHeight;
    state.ownedOutputs.reserve(outputs.size());
    for (const auto &coreOut : outputs)
    {
      BoltRPC::OutputInfo rpcOut;
      rpcOut.blockHeight = coreOut.blockHeight;
      rpcOut.txHash = coreOut.txHash;
      rpcOut.amount = coreOut.amount;
      rpcOut.outputIndex = coreOut.outputIndex;
      rpcOut.globalOutputIndex = coreOut.globalOutputIndex;
      rpcOut.hasGlobalOutputIndex = coreOut.hasGlobalOutputIndex;
      rpcOut.outputKey = coreOut.outputKey;
      rpcOut.txPublicKey = coreOut.txPublicKey;
      rpcOut.keyImage = coreOut.keyImage;
      rpcOut.spent = coreOut.spent;
      rpcOut.isDeposit = coreOut.isDeposit;
      rpcOut.term = coreOut.term;
      state.ownedOutputs.push_back(rpcOut);
      if (coreOut.spent)
        state.spentKeyImages.push_back(coreOut.keyImage);
      else
        state.balance += coreOut.amount;
    }
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      m_cachedOutputs = outputs;
    }
    syncLog("saveStateFile: saving " + std::to_string(state.ownedOutputs.size()) + " outputs");
    const bool ok = mgr.save(state);
    if (!ok)
      syncLog("saveStateFile: failed to write state file");
    return ok;
  }

  std::vector<BoltCore::OutputInfo> SyncEngine::getCachedOutputs() const
  {
    std::lock_guard<std::mutex> lock(m_outputCacheMutex);
    return m_cachedOutputs;
  }

  bool SyncEngine::isOurOutput(const BoltCore::OutputInfo &candidate) const
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(candidate.txPublicKey, m_viewKey, derivation))
      return false;
    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, outputSigningIndex(candidate), m_spendPub, derivedKey))
      return false;
    return derivedKey == candidate.outputKey;
  }

  crypto::KeyImage SyncEngine::deriveKeyImage(const BoltCore::OutputInfo &output) const
  {
    crypto::KeyImage ki = {};
    if (!m_spendKey)
      return ki;
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(output.txPublicKey, m_viewKey, derivation))
      return ki;
    crypto::SecretKey ephemeralSec;
    crypto::derive_secret_key(derivation, outputSigningIndex(output), *m_spendKey, ephemeralSec);
    crypto::PublicKey ephemeralPub;
    if (!crypto::secret_key_to_public_key(ephemeralSec, ephemeralPub))
      return ki;
    crypto::generate_key_image(ephemeralPub, ephemeralSec, ki);
    return ki;
  }

  void SyncEngine::filterOwnedOutputs(std::vector<BoltCore::OutputInfo> &candidates)
  {
    std::vector<BoltCore::OutputInfo> owned;
    for (auto &c : candidates)
    {
      if (isOurOutput(c))
      {
        if (m_spendKey)
          c.keyImage = deriveKeyImage(c);
        owned.push_back(std::move(c));
      }
    }
    candidates = std::move(owned);
  }

  void SyncEngine::scanBlockTransaction(
      const cn::Transaction &tx,
      uint32_t blockHeight,
      const std::vector<uint32_t> *globalIndexes,
      std::vector<BoltCore::OutputInfo> &outputs,
      cn::BlockFilterRecord *filterRecord,
      bool &haveFilter,
      size_t &filterIdx,
      const crypto::Hash *canonicalTxHash)
  {
    static const crypto::PublicKey NULL_KEY = {};

    const crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
    if (txPubKey == NULL_KEY)
      return;

    const crypto::Hash computedHash = cn::getObjectHash(tx);
    const crypto::Hash txHash = canonicalTxHash != nullptr ? *canonicalTxHash : computedHash;
    std::vector<BoltCore::OutputInfo> txOutputs;

    if (BoltCore::NewOutputScanner::hasNewOutputs(tx))
    {
      std::vector<uint32_t> indexes;
      if (globalIndexes)
        indexes = *globalIndexes;

      std::vector<BoltSync::FoundOutput> found;
      BoltCore::NewOutputScanner::scanTransaction(
          tx, txPubKey, indexes, blockHeight,
          m_viewKey, m_spendPub, m_spendKey, found);

      for (const auto &fo : found)
      {
        BoltCore::OutputInfo info;
        info.blockHeight = fo.blockHeight;
        info.txHash = fo.txHash;
        info.outputIndex = fo.outputIndex;
        info.globalOutputIndex = fo.globalOutputIndex;
        info.hasGlobalOutputIndex = fo.hasGlobalOutputIndex;
        info.amount = fo.amount;
        info.outputKey = fo.outputKey;
        info.txPublicKey = fo.txPublicKey;
        info.keyImage = fo.keyImage;
        info.spent = false;
        info.isDeposit = fo.isDeposit;
        info.term = fo.term;
        info.keyDerivationIndex = fo.keyDerivationIndex;
        info.hasKeyDerivationIndex = fo.hasKeyDerivationIndex;
        txOutputs.push_back(std::move(info));
      }
      // Do not return — legacy KeyOutput / MultisignatureOutput scanning must still run
      // (WalletGreen change outputs are KeyOutput; skipping them broke balances after rescan).
    }

    std::unordered_set<uint32_t> newScannerIndices;
    for (const auto &out : txOutputs)
      newScannerIndices.insert(out.outputIndex);

    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, m_viewKey, derivation))
      return;

    size_t keyIndex = 0;
    for (size_t o = 0; o < tx.outputs.size(); ++o)
    {
      const auto &out = tx.outputs[o];
      if (out.target.type() == typeid(cn::KeyOutput))
      {
        if (newScannerIndices.count(static_cast<uint32_t>(o)) > 0)
        {
          ++keyIndex;
          continue;
        }

        const auto &keyOut = boost::get<cn::KeyOutput>(out.target);

        if (haveFilter && filterRecord)
        {
          if (filterIdx >= filterRecord->entries.size())
          {
            ++keyIndex;
            continue;
          }

          const auto &entry = filterRecord->entries[filterIdx];
          ++filterIdx;

          const uint8_t walletTag = BoltSync::computeDaemonViewTag(txPubKey, o);
          if (walletTag != entry.view_tag)
          {
            ++keyIndex;
            continue;
          }
        }

        // WalletGreen / TransfersConsumer: KeyOutput derivation uses the output index in the tx
        // (not a running count of KeyOutputs). A deposit multisig at index 0 must not make
        // change at index 1 sign with derivation index 0.
        crypto::PublicKey derivedKey;
        if (crypto::derive_public_key(derivation, o, m_spendPub, derivedKey) &&
            derivedKey == keyOut.key)
        {
          BoltCore::OutputInfo info;
          info.blockHeight = blockHeight;
          info.txHash = txHash;
          info.amount = out.amount;
          info.outputIndex = static_cast<uint32_t>(o);
          info.outputKey = keyOut.key;
          info.txPublicKey = txPubKey;
          info.spent = false;
          info.isDeposit = false;
          info.term = 0;
          if (globalIndexes && o < globalIndexes->size())
          {
            info.globalOutputIndex = (*globalIndexes)[o];
            info.hasGlobalOutputIndex = true;
          }
          if (m_spendKey)
          {
            crypto::SecretKey outSec = BoltSync::deriveOutputSecretKey(derivation, o, *m_spendKey);
            crypto::generate_key_image(keyOut.key, outSec, info.keyImage);
          }
          txOutputs.push_back(std::move(info));
        }
        ++keyIndex;
      }
      else if (out.target.type() == typeid(cn::MultisignatureOutput))
      {
        if (newScannerIndices.count(static_cast<uint32_t>(o)) > 0)
        {
          keyIndex += boost::get<cn::MultisignatureOutput>(out.target).keys.size();
          continue;
        }

        const auto &msigOut = boost::get<cn::MultisignatureOutput>(out.target);

        for (size_t ki = 0; ki < msigOut.keys.size(); ++ki)
        {
          crypto::PublicKey recoveredSpend;
          if (crypto::underive_public_key(derivation, o, msigOut.keys[ki], recoveredSpend) &&
              recoveredSpend == m_spendPub)
          {
            BoltCore::OutputInfo info;
            info.blockHeight = blockHeight;
            info.txHash = txHash;
            info.amount = out.amount;
            info.outputIndex = static_cast<uint32_t>(o);
            info.outputKey = msigOut.keys[ki];
            info.txPublicKey = txPubKey;
            info.spent = false;
            info.isDeposit = (msigOut.term > 0);
            info.term = msigOut.term;
            if (globalIndexes && o < globalIndexes->size())
            {
              info.globalOutputIndex = (*globalIndexes)[o];
              info.hasGlobalOutputIndex = true;
            }
            txOutputs.push_back(std::move(info));
            break;
          }
        }
        keyIndex += msigOut.keys.size();
      }
    }

    dedupeOutputsByRef(txOutputs);
    dedupeOutputsByKey(txOutputs);
    dedupeOutputsPhysically(txOutputs);
    for (auto &out : txOutputs)
      out.txHash = txHash;
    outputs.insert(outputs.end(), txOutputs.begin(), txOutputs.end());
  }

  std::vector<BoltCore::OutputInfo> SyncEngine::scanTransactionOutputs(
      const cn::Transaction &tx,
      const crypto::Hash &txHash,
      uint32_t blockHeight)
  {
    std::vector<BoltCore::OutputInfo> outputs;
    bool haveFilter = false;
    size_t filterIdx = 0;
    scanBlockTransaction(tx, blockHeight, nullptr, outputs, nullptr, haveFilter, filterIdx, &txHash);
    for (auto &out : outputs)
      out.txHash = txHash;
    return outputs;
  }

  bool SyncEngine::tryResolveIncomingTx(const crypto::Hash &txHash,
                                        std::vector<BoltCore::OutputInfo> &outputs,
                                        uint64_t &totalAmount,
                                        bool &confirmed,
                                        cn::Transaction *rawTxOut)
  {
    outputs.clear();
    totalAmount = 0;
    confirmed = false;

    if (!m_node)
      return false;

    cn::Transaction tx;
    uint32_t blockHeight = 0;
    auto *nc = dynamic_cast<NodeClient::NodeClient *>(m_node);

    if (nc)
    {
      bool inBlock = false;
      if (nc->fetchTransactionDetails(txHash, tx, blockHeight, inBlock))
      {
        confirmed = inBlock;
        outputs = scanTransactionOutputs(tx, txHash, inBlock ? blockHeight : 0);
        if (rawTxOut)
          *rawTxOut = tx;
      }
    }

    if (outputs.empty() && m_node->getTransactionSync(txHash, tx))
    {
      confirmed = false;
      outputs = scanTransactionOutputs(tx, txHash, 0);
      if (rawTxOut)
        *rawTxOut = tx;
    }

    if (outputs.empty())
      return false;

    // WalletGreen: unconfirmed transfers have no global index; set on block confirm.
    if (confirmed && blockHeight > 0)
    {
      std::vector<uint32_t> globalIndices;
      if (fetchGlobalIndicesForConfirmedTx(m_node, nc, txHash, blockHeight, globalIndices))
        applyGlobalIndicesToOutputs(outputs, globalIndices);
    }

    for (auto &out : outputs)
    {
      out.txHash = txHash;
      totalAmount += out.amount;
    }
    return true;
  }

  std::vector<BoltCore::OutputInfo> SyncEngine::scanDaemonGapBlocks()
  {
    std::vector<BoltCore::OutputInfo> found;

    // This function is the fallback gap-filler for both DirectScan and Polling.
    //
    // Polling: SyncManager drives block scanning via get_filter_records RPC.
    // If that endpoint is unavailable on the remote daemon, SyncManager fails
    // silently and never advances m_scannedHeight.  This function then takes
    // over, fetching blocks via the standard getTransactionsAtHeight and
    // advancing m_scannedHeight itself.  When SyncManager IS working, Problem 3
    // (progress callback → m_scannedHeight) ensures firstHeight starts where
    // SyncManager left off, so overlap is minimal.  Any actual overlap is
    // harmless: m_walletAppliedTxHashes prevents double-crediting, and
    // markSpent is idempotent.
    //
    // DirectScan: fills the gap between MDBX tip and daemon tip.
    auto *nc = dynamic_cast<NodeClient::NodeClient *>(m_node);
    if (!nc)
      return found;

    const uint32_t mdbxTop = peekMdbxTopHeight();
    const uint32_t daemonHeight = m_daemonHeight.load();
    const uint32_t daemonTip = daemonHeight > 0 ? daemonHeight - 1 : 0;
    const uint32_t walletHeight = m_scannedHeight.load();

    // Wallet state may already cover most of the chain; only fetch blocks after wallet tip.
    if (walletHeight >= daemonTip)
      return found;

    if (daemonTip <= mdbxTop)
      return found;

    uint32_t firstHeight = std::max(mdbxTop + 1u, walletHeight + 1u);
    if (m_lastDaemonGapScanHeight + 1u > firstHeight)
      firstHeight = m_lastDaemonGapScanHeight + 1u;
    const uint32_t lastHeight = daemonTip;
    if (firstHeight > lastHeight)
      return found;

    syncLog("scanDaemonGapBlocks: heights " + std::to_string(firstHeight) +
            ".." + std::to_string(lastHeight) +
            " (wallet=" + std::to_string(walletHeight) + ", mdbx=" + std::to_string(mdbxTop) + ")");

    uint32_t batchTop = 0;
    std::vector<cn::Transaction> allTxs;
    for (uint32_t h = firstHeight; h <= lastHeight && h <= firstHeight + 4; ++h)
    {
      if (m_stop)
        break;
      std::vector<cn::Transaction> txs;
      std::vector<crypto::Hash> hashes;
      std::vector<std::vector<uint32_t>> globalIndexes;
      if (!nc->getTransactionsAtHeight(h, txs, hashes, &globalIndexes))
        continue;

      for (size_t i = 0; i < txs.size(); ++i)
      {
        const std::vector<uint32_t> *indexes = i < globalIndexes.size() ? &globalIndexes[i] : nullptr;
        bool haveFilter = false;
        size_t filterIdx = 0;
        const size_t before = found.size();
        scanBlockTransaction(txs[i], h, indexes, found, nullptr, haveFilter, filterIdx, &hashes[i]);
        for (size_t o = before; o < found.size(); ++o)
        {
          if (indexes && found[o].outputIndex < indexes->size())
          {
            found[o].globalOutputIndex = (*indexes)[found[o].outputIndex];
            found[o].hasGlobalOutputIndex = true;
          }
        }
      }

      allTxs.insert(allTxs.end(), txs.begin(), txs.end());
      m_lastDaemonGapScanHeight = h;
      batchTop = h;
    }

    // Inline spend detection: check all transaction inputs against our known key images.
    if (!allTxs.empty())
    {
      auto spends = detectSpendsInTransactions(allTxs);
      if (m_onSpent && (!spends.spentKeyImages.empty() || !spends.outputSpends.empty() ||
                        !spends.depositSpends.empty()))
        m_onSpent(spends.spentKeyImages, spends.depositSpends, spends.outputSpends);
    }

    if (batchTop > walletHeight)
    {
      m_scannedHeight.store(batchTop);
      syncLog("scanDaemonGapBlocks: advanced wallet height to " + std::to_string(batchTop));
    }

    if (!found.empty())
      syncLog("scanDaemonGapBlocks: found " + std::to_string(found.size()) + " outputs");

    return found;
  }

  SyncEngine::MempoolUpdate SyncEngine::pollMempool()
  {
    MempoolUpdate update;
    if (m_stop || !m_node)
      return update;

    const std::vector<crypto::Hash> poolIds = m_node->getPoolTransactions();
    std::vector<crypto::Hash> previous;
    {
      std::lock_guard<std::mutex> lock(m_poolMutex);
      previous = m_knownPoolTxIds;
    }

    auto isKnown = [&previous](const crypto::Hash &hash)
    {
      for (const auto &known : previous)
      {
        if (known == hash)
          return true;
      }
      return false;
    };

    auto isInPool = [&poolIds](const crypto::Hash &hash)
    {
      for (const auto &id : poolIds)
      {
        if (id == hash)
          return true;
      }
      return false;
    };

    std::vector<crypto::Hash> nextKnown;
    nextKnown.reserve(poolIds.size());
    std::unordered_set<crypto::Hash, boost::hash<crypto::Hash>> confirmedViaPool;

    // Collect raw transactions for mempool spend detection.  We scan the INPUTS
    // of every new-to-pool tx immediately so that spends are reported in the
    // same poll cycle as the incoming change output — not minutes later when the
    // block is confirmed.  Note: even txs where tryResolveIncomingTx returns
    // false (no outputs for us) might contain key images of our outputs as
    // inputs (e.g. a send-all with no change), so we always collect.
    std::vector<cn::Transaction> newPoolTxsForSpendScan;

    for (const auto &hash : poolIds)
    {
      if (m_stop)
        break;
      if (isKnown(hash))
      {
        nextKnown.push_back(hash);
        continue;
      }

      std::vector<BoltCore::OutputInfo> outputs;
      uint64_t total = 0;
      bool confirmed = false;
      cn::Transaction rawTx;
      if (!tryResolveIncomingTx(hash, outputs, total, confirmed, &rawTx))
      {
        // No outputs for us, but still collect the tx for input/spend scanning.
        cn::Transaction fallbackTx;
        if (m_node->getTransactionSync(hash, fallbackTx))
        {
          nextKnown.push_back(hash);
          newPoolTxsForSpendScan.push_back(std::move(fallbackTx));
        }
        continue;
      }

      nextKnown.push_back(hash);
      newPoolTxsForSpendScan.push_back(rawTx);

      if (confirmed)
      {
        confirmedViaPool.insert(hash);
        for (auto &out : outputs)
          update.confirmedOutputs.push_back(std::move(out));
      }
      else
      {
        IncomingMempoolTx incoming;
        incoming.txHash = hash;
        incoming.totalAmount = total;
        incoming.outputs = std::move(outputs);
        for (auto &out : incoming.outputs)
          out.blockHeight = 0;
        update.newIncoming.push_back(std::move(incoming));
      }
    }

    for (const auto &hash : previous)
    {
      if (isInPool(hash))
        continue;

      update.removedFromPool.push_back(hash);

      std::vector<BoltCore::OutputInfo> outputs;
      uint64_t total = 0;
      bool confirmed = false;
      if (!tryResolveIncomingTx(hash, outputs, total, confirmed))
        continue;

      if (confirmed)
      {
        confirmedViaPool.insert(hash);
        for (auto &out : outputs)
          update.confirmedOutputs.push_back(std::move(out));
      }
      else if (total > 0)
      {
        IncomingMempoolTx incoming;
        incoming.txHash = hash;
        incoming.totalAmount = total;
        incoming.outputs = std::move(outputs);
        for (auto &out : incoming.outputs)
          out.blockHeight = 0;
        update.newIncoming.push_back(std::move(incoming));
      }
    }

    {
      std::lock_guard<std::mutex> lock(m_poolMutex);
      m_knownPoolTxIds = std::move(nextKnown);
    }

    // Inline spend detection from mempool inputs.  This fires m_onSpent in the
    // same poll cycle as the incoming change is reported, eliminating the
    // minutes-long gap between "pending" appearing and the spent outputs being
    // deducted.  Confirmed-block spends are still handled by SyncManager (Polling)
    // or scanDaemonGapBlocks (DirectScan) as a redundant safety net.
    if (!newPoolTxsForSpendScan.empty())
    {
      auto mempoolSpends = detectSpendsInTransactions(newPoolTxsForSpendScan);
      if (m_onSpent && (!mempoolSpends.spentKeyImages.empty() ||
                        !mempoolSpends.outputSpends.empty() ||
                        !mempoolSpends.depositSpends.empty()))
      {
        syncLog("pollMempool: detected " +
                std::to_string(mempoolSpends.spentKeyImages.size()) +
                " mempool spends inline");
        m_onSpent(mempoolSpends.spentKeyImages,
                  mempoolSpends.depositSpends,
                  mempoolSpends.outputSpends);
      }
    }

    const auto gapOutputs = scanDaemonGapBlocks();
    for (const auto &out : gapOutputs)
    {
      if (confirmedViaPool.count(out.txHash))
        continue;
      mergeCachedOutput(out);
      update.confirmedOutputs.push_back(out);
    }
    dedupeOutputsByRef(update.confirmedOutputs);
    dedupeOutputsByKey(update.confirmedOutputs);
    dedupeOutputsPhysically(update.confirmedOutputs);

    if (!poolIds.empty() || !update.newIncoming.empty() || !update.removedFromPool.empty() ||
        !update.confirmedOutputs.empty())
    {
      syncLog("pollMempool: pool=" + std::to_string(poolIds.size()) + ", " +
              std::to_string(update.newIncoming.size()) + " new, " +
              std::to_string(update.removedFromPool.size()) + " removed, " +
              std::to_string(update.confirmedOutputs.size()) + " confirmed");
    }

    return update;
  }

} // namespace ClientWallet