// SyncEngine - multi-strategy wallet sync implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SyncEngine.h"
#include "Blockchain/BlockchainFilter.h"
#include "BoltCore/BoltCoreTypes.h"
#include "BoltRPC/StateManager.h"
#include "BoltRPC/SyncManager.h"
#include "BoltSync/BoltSync.h"
#include "BoltSync/BlockDeserializer.h"
#include "BoltSync/CryptoHelpers.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "Common/PathHelpers.h"
#include "Common/StringTools.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "crypto/crypto.h"
#include "INode.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <mdbx.h>

static void syncLog(const std::string &msg)
{
  std::ofstream log("/tmp/conceal-wallet-sync.log", std::ios::app);
  log << msg << std::endl;
}

namespace ClientWallet
{

  SyncEngine::SyncEngine(const std::string &dataDir,
                         const crypto::SecretKey &viewKey,
                         const crypto::PublicKey &spendPub,
                         const crypto::SecretKey *spendKey)
      : m_dataDir(dataDir), m_viewKey(viewKey), m_spendPub(spendPub), m_spendKey(spendKey)
  {
  }

  SyncEngine::~SyncEngine()
  {
    stop();
  }

  void SyncEngine::setNode(cn::INode *node) { m_node = node; }
  void SyncEngine::setDaemonRpc(std::function<std::string(const std::string &, const std::string &)> rpc) { m_daemonRpc = rpc; }

  // ── Lifecycle ──────────────────────────────────────────────────────────

  void SyncEngine::start(OutputCallback onOutputs, StatusCallback onStatus)
  {
    syncLog("SyncEngine::start() called, dataDir=" + m_dataDir);
    m_onOutputs = std::move(onOutputs);
    m_onStatus = std::move(onStatus);
    m_strategy = detectBestStrategy();
    syncLog("Strategy selected: " + std::to_string((int)m_strategy));

    SyncStatus status;
    status.strategy = m_strategy;
    if (m_onStatus)
      m_onStatus(status);
    incrementalSync();
  }

  void SyncEngine::stop()
  {
    m_stop = true;
    // Don't join — let the thread finish on its own.
    // The destructor will join if still joinable.
    if (m_thread.joinable())
    {
      // Give it a brief moment, then detach
      m_thread.detach();
    }
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
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      m_cachedOutputs.clear();
    }
    incrementalSync();
  }

  void SyncEngine::incrementalSync()
  {
    if (m_active)
    {
      syncLog("incrementalSync: already active");
      return;
    }
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
    if (newHeight > m_scannedHeight)
    {
      syncLog("onNewBlockHeight: " + std::to_string(newHeight));
      incrementalSync();
    }
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

    if (m_scannedHeight >= topHeight)
    {
      mdbx_env_close(env);
      markSpentOutputs(topHeight);
      status.isSyncing = false;
      if (m_onStatus)
        m_onStatus(status);
      return;
    }

    std::vector<BoltCore::OutputInfo> outputs;
    uint32_t checked = 0;

    auto scanTxOutputs = [this, &outputs](
                             const cn::Transaction &tx, const crypto::PublicKey &txPubKey,
                             const crypto::Hash &txHash, uint32_t blockHeight,
                             const cn::BlockFilterRecord *filterRecord, size_t &filterIdx)
    {
      if (txPubKey == cn::NULL_PUBLIC_KEY)
        return;

      crypto::KeyDerivation derivation;
      if (!crypto::generate_key_derivation(txPubKey, m_viewKey, derivation))
        return;

      size_t keyIndex = 0;
      for (size_t o = 0; o < tx.outputs.size(); ++o)
      {
        const auto &out = tx.outputs[o];
        if (out.target.type() == typeid(cn::KeyOutput))
        {
          const auto &keyOut = boost::get<cn::KeyOutput>(out.target);

          if (filterRecord && filterIdx < filterRecord->entries.size())
          {
            const auto &entry = filterRecord->entries[filterIdx];
            ++filterIdx;

            uint8_t walletTag = BoltSync::computeDaemonViewTag(txPubKey, o);
            if (walletTag != entry.view_tag)
            {
              ++keyIndex;
              continue;
            }
          }

          crypto::PublicKey derivedKey;
          if (crypto::derive_public_key(derivation, keyIndex, m_spendPub, derivedKey) &&
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
            if (m_spendKey)
              info.keyImage = deriveKeyImage(info);
            outputs.push_back(info);
          }
          ++keyIndex;
        }
        else if (out.target.type() == typeid(cn::MultisignatureOutput))
        {
          const auto &msigOut = boost::get<cn::MultisignatureOutput>(out.target);
          if (msigOut.term > 0)
          {
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
                info.isDeposit = true;
                info.term = msigOut.term;
                outputs.push_back(info);
                break;
              }
            }
          }
          keyIndex += msigOut.keys.size();
        }
      }
    };

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
        if (BoltSync::deserializeBlockEntry(ba, block, transactions))
        {
          cn::BlockFilterRecord filterRecord;
          bool haveFilter = false;
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

          size_t filterIdx = 0;

          // transactions[0] is the coinbase — process all transactions
          // through the same path so filterIdx stays aligned
          for (const auto &tx : transactions)
          {
            crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
            scanTxOutputs(tx, txPubKey, getObjectHash(tx), h,
                          haveFilter ? &filterRecord : nullptr, filterIdx);
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
        if (!outputs.empty() && m_onOutputs)
        {
          syncLog("runDirectScan: dispatching " + std::to_string(outputs.size()) + " outputs");
          m_onOutputs(outputs);
          std::lock_guard<std::mutex> lock(m_outputCacheMutex);
          m_cachedOutputs.insert(m_cachedOutputs.end(), outputs.begin(), outputs.end());
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

    if (!outputs.empty() && m_onOutputs)
    {
      m_onOutputs(outputs);
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      m_cachedOutputs.insert(m_cachedOutputs.end(), outputs.begin(), outputs.end());
    }

    m_scannedHeight = topHeight;
    status.scannedHeight = m_scannedHeight;
    status.ownedOutputs = static_cast<uint32_t>(m_cachedOutputs.size());
    mdbx_env_close(env);

    markSpentOutputs(topHeight);

    status.isSyncing = false;
    if (m_onStatus)
      m_onStatus(status);
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
    syncMgr.start(
        [this, &status](const BoltRPC::SyncProgress &progress)
        {
          status.scannedHeight = progress.currentHeight;
          status.currentHeight = progress.currentHeight;
          if (m_onStatus)
            m_onStatus(status);
        },
        [this](const std::vector<BoltRPC::OutputInfo> &newOutputs,
               const std::vector<crypto::KeyImage> &spentKeyImages)
        {
          std::vector<BoltCore::OutputInfo> converted;
          converted.reserve(newOutputs.size());
          for (const auto &rpcOut : newOutputs)
          {
            BoltCore::OutputInfo info;
            info.blockHeight = rpcOut.blockHeight;
            info.txHash = rpcOut.txHash;
            info.amount = rpcOut.amount;
            info.outputIndex = rpcOut.outputIndex;
            info.outputKey = rpcOut.outputKey;
            info.txPublicKey = rpcOut.txPublicKey;
            info.spent = rpcOut.spent;
            info.isDeposit = rpcOut.isDeposit;
            info.term = rpcOut.term;
            converted.push_back(info);
          }
          if (!converted.empty() && m_onOutputs)
          {
            m_onOutputs(converted);
            std::lock_guard<std::mutex> lock(m_outputCacheMutex);
            m_cachedOutputs.insert(m_cachedOutputs.end(), converted.begin(), converted.end());
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

  // ── Mark spent outputs ─────────────────────────────────────────────────

  void SyncEngine::markSpentOutputs(uint32_t topHeight)
  {
    syncLog("markSpentOutputs: starting spent check");
    std::lock_guard<std::mutex> lock(m_outputCacheMutex);
    if (m_cachedOutputs.empty() || !m_spendKey)
      return;

    std::unordered_map<crypto::KeyImage, size_t> keyImageIndex;
    for (size_t i = 0; i < m_cachedOutputs.size(); ++i)
    {
      const auto &fo = m_cachedOutputs[i];
      if (!fo.isDeposit)
      {
        static const crypto::KeyImage NULL_KI = {};
        if (std::memcmp(&fo.keyImage, &NULL_KI, sizeof(crypto::KeyImage)) != 0)
          keyImageIndex[fo.keyImage] = i;
      }
    }
    if (keyImageIndex.empty())
      return;

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

    uint32_t spentCount = 0;
    for (uint32_t h = 0; h <= topHeight; ++h)
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
          auto checkInputs = [&](const cn::Transaction &tx)
          {
            for (const auto &input : tx.inputs)
            {
              if (input.type() == typeid(cn::KeyInput))
              {
                const auto &ki = boost::get<cn::KeyInput>(input).keyImage;
                auto it = keyImageIndex.find(ki);
                if (it != keyImageIndex.end())
                {
                  m_cachedOutputs[it->second].spent = true;
                  spentCount++;
                }
              }
            }
          };
          checkInputs(block.baseTransaction);
          for (const auto &tx : transactions)
            checkInputs(tx);
        }
      }
      mdbx_txn_abort(rt);
    }

    syncLog("markSpentOutputs: marked " + std::to_string(spentCount) + " outputs as spent");
    mdbx_env_close(env);
  }

  // ── State file ─────────────────────────────────────────────────────────

  bool SyncEngine::loadStateFile(const std::string &path)
  {
    syncLog("loadStateFile: " + path);
    BoltRPC::StateManager mgr(path);
    BoltRPC::WalletState state;
    if (!mgr.load(state))
      return false;
    m_scannedHeight = state.lastHeight;

    std::vector<BoltCore::OutputInfo> converted;
    converted.reserve(state.ownedOutputs.size());
    for (const auto &rpcOut : state.ownedOutputs)
    {
      BoltCore::OutputInfo info;
      info.blockHeight = rpcOut.blockHeight;
      info.txHash = rpcOut.txHash;
      info.amount = rpcOut.amount;
      info.outputIndex = rpcOut.outputIndex;
      info.outputKey = rpcOut.outputKey;
      info.txPublicKey = rpcOut.txPublicKey;
      info.spent = rpcOut.spent;
      info.isDeposit = rpcOut.isDeposit;
      info.term = rpcOut.term;
      converted.push_back(info);
    }
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      m_cachedOutputs = converted;
    }
    if (m_onOutputs && !converted.empty())
      m_onOutputs(converted);
    m_strategy = SyncStrategy::Offline;
    syncLog("loadStateFile: loaded " + std::to_string(converted.size()) + " outputs at height " + std::to_string(state.lastHeight));
    return true;
  }

  bool SyncEngine::saveStateFile(const std::string &path)
  {
    syncLog("saveStateFile: " + path);
    BoltRPC::StateManager mgr(path);
    BoltRPC::WalletState state;
    state.lastHeight = m_scannedHeight;
    {
      std::lock_guard<std::mutex> lock(m_outputCacheMutex);
      state.ownedOutputs.reserve(m_cachedOutputs.size());
      for (const auto &coreOut : m_cachedOutputs)
      {
        BoltRPC::OutputInfo rpcOut;
        rpcOut.blockHeight = coreOut.blockHeight;
        rpcOut.txHash = coreOut.txHash;
        rpcOut.amount = coreOut.amount;
        rpcOut.outputIndex = coreOut.outputIndex;
        rpcOut.outputKey = coreOut.outputKey;
        rpcOut.txPublicKey = coreOut.txPublicKey;
        rpcOut.spent = coreOut.spent;
        rpcOut.isDeposit = coreOut.isDeposit;
        rpcOut.term = coreOut.term;
        state.ownedOutputs.push_back(rpcOut);
        if (coreOut.spent)
          state.spentKeyImages.push_back(coreOut.keyImage);
        if (!coreOut.spent)
          state.balance += coreOut.amount;
      }
    }
    syncLog("saveStateFile: saving " + std::to_string(state.ownedOutputs.size()) + " outputs");
    return mgr.save(state);
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
    if (!crypto::derive_public_key(derivation, candidate.outputIndex, m_spendPub, derivedKey))
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
    crypto::derive_secret_key(derivation, output.outputIndex, *m_spendKey, ephemeralSec);
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

} // namespace ClientWallet