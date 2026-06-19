// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "SyncManager.h"
#include "BoltSync/CryptoHelpers.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "INode.h"
#include "Rpc/HttpClient.h"
#include "BoltCore/NewOutputScanner.h"
#include "CryptoNoteConfig.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <fstream>
#include <set>
#include <sstream>

using namespace cn;

namespace BoltRPC
{

  // ─── JSON helpers (no external dependency) ─────────────────────────────────

  namespace
  {

    bool jsonGetUint(const std::string &json, const std::string &key, uint32_t &out)
    {
      std::string search = "\"" + key + "\":";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return false;
      pos += search.size();
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
        ++pos;
      char *end = nullptr;
      unsigned long val = std::strtoul(json.c_str() + pos, &end, 10);
      if (end == json.c_str() + pos)
        return false;
      out = static_cast<uint32_t>(val);
      return true;
    }

    std::string jsonGetString(const std::string &json, const std::string &key)
    {
      std::string search = "\"" + key + "\":\"";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return "";
      pos += search.size();
      size_t end = pos;
      while (end < json.size())
      {
        if (json[end] == '"' && (end == pos || json[end - 1] != '\\'))
          break;
        ++end;
      }
      return json.substr(pos, end - pos);
    }

    std::string jsonExtractArray(const std::string &json, const std::string &key)
    {
      std::string search = "\"" + key + "\":[";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return "";
      pos += search.size();
      size_t start = pos;
      int depth = 1;
      bool inString = false;
      while (pos < json.size() && depth > 0)
      {
        char c = json[pos];
        if (c == '"' && (pos == start || json[pos - 1] != '\\'))
          inString = !inString;
        if (!inString)
        {
          if (c == '[')
            ++depth;
          else if (c == ']')
            --depth;
        }
        ++pos;
      }
      return json.substr(start, pos - start - 1);
    }

    std::vector<std::string> jsonSplitObjects(const std::string &arrayContent)
    {
      std::vector<std::string> result;
      if (arrayContent.empty())
        return result;
      int depth = 0;
      bool inString = false;
      size_t start = 0;
      for (size_t i = 0; i < arrayContent.size(); ++i)
      {
        char c = arrayContent[i];
        if (c == '"' && (i == 0 || arrayContent[i - 1] != '\\'))
          inString = !inString;
        if (!inString)
        {
          if (c == '{')
          {
            if (depth == 0)
              start = i;
            ++depth;
          }
          else if (c == '}')
          {
            --depth;
            if (depth == 0)
              result.push_back(arrayContent.substr(start, i - start + 1));
          }
        }
      }
      return result;
    }

    // Parse a single FilterEntry from JSON object
    cn::FilterEntry parseFilterEntry(const std::string &obj)
    {
      cn::FilterEntry entry;
      std::memset(&entry, 0, sizeof(entry));

      uint32_t view_tag = 0, output_index = 0, tx_index = 0;
      jsonGetUint(obj, "view_tag", view_tag);
      jsonGetUint(obj, "output_index", output_index);
      jsonGetUint(obj, "tx_index", tx_index);

      entry.view_tag = static_cast<uint8_t>(view_tag);
      entry.output_index = static_cast<uint8_t>(output_index);
      entry.tx_index = static_cast<uint16_t>(tx_index);

      return entry;
    }

    // Parse a single BlockFilterRecord from JSON object
    cn::BlockFilterRecord parseFilterRecord(const std::string &obj)
    {
      cn::BlockFilterRecord record;
      record.block_height = 0;

      jsonGetUint(obj, "block_height", record.block_height);

      std::string entriesArr = jsonExtractArray(obj, "entries");
      std::vector<std::string> entryObjs = jsonSplitObjects(entriesArr);
      record.entries.reserve(entryObjs.size());
      for (const auto &entryObj : entryObjs)
        record.entries.push_back(parseFilterEntry(entryObj));

      return record;
    }

  } // anonymous namespace

  // ─── Constructor / Destructor ──────────────────────────────────────────────

  SyncManager::SyncManager(cn::INode &node,
                           const crypto::SecretKey &viewSecretKey,
                           const crypto::PublicKey &spendPublicKey,
                           const std::string &dataDir,
                           DaemonRpcCallback rpcCallback)
      : m_node(node),
        m_viewSecretKey(viewSecretKey),
        m_spendPublicKey(spendPublicKey),
        m_dataDir(dataDir),
        m_rpcCallback(std::move(rpcCallback))
  {
    loadProgress();
  }

  SyncManager::~SyncManager()
  {
    stop();
    saveProgress();
  }

  // ─── Public API ────────────────────────────────────────────────────────────

  void SyncManager::start(ProgressCallback onProgress, OutputCallback onOutputs)
  {
    if (m_active.exchange(true))
      return;

    m_onProgress = std::move(onProgress);
    m_onOutputs = std::move(onOutputs);
    m_stop.store(false);
    m_triggerSync.store(false);
    m_thread = std::thread(&SyncManager::runLoop, this);
  }

  void SyncManager::stop()
  {
    m_stop.store(true);
    if (m_thread.joinable())
      m_thread.join();
    m_active.store(false);
  }

  void SyncManager::syncNow()
  {
    m_triggerSync.store(true);
  }

  void SyncManager::seedScannedHeight(uint32_t height)
  {
    const uint32_t current = m_lastScannedHeight.load();
    if (height > current)
    {
      m_lastScannedHeight.store(height);
      saveProgress();
    }
  }

  // ─── Main Loop ─────────────────────────────────────────────────────────────

  void SyncManager::runLoop()
  {
    SyncProgress progress;

    // ── First sync: full bootstrap via filter records ──────────────────────
    if (m_lastScannedHeight.load() == 0)
    {
      doBootstrap(progress);
    }

    // ── Background incremental loop ────────────────────────────────────────
    while (!m_stop.load())
    {
      for (uint32_t i = 0; i < POLL_INTERVAL_SECONDS * 2 && !m_stop.load() && !m_triggerSync.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

      if (m_stop.load())
        break;

      doIncrementalSync(progress);
      m_triggerSync.store(false);
    }
  }

  // ─── Bootstrap Sync ────────────────────────────────────────────────────────
  //
  // 1. Download filter records for the entire chain in batches
  // 2. Run view-tag filter locally to find candidate outputs
  // 3. Fetch full blocks only for blocks that contain candidates
  // 4. Do full ECDH derivation to confirm ownership

  void SyncManager::doBootstrap(SyncProgress &progress)
  {
    progress = SyncProgress();

    // Step 1: Get chain height
    uint32_t chainHeight = 0;
    {
      std::vector<cn::BlockFilterRecord> dummy;
      if (!callGetFilterRecords(0, 0, dummy, chainHeight))
      {
        progress.errorMessage = "Failed to connect to daemon";
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }
    }

    progress.totalBlocks = chainHeight;
    progress.currentHeight = chainHeight;

    if (chainHeight == 0)
    {
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    uint32_t forkHeight = getForkHeight();
    std::vector<OutputInfo> allOwned;

    // Step 2: Sync pre-fork range with filters
    uint32_t preForkEnd = std::min(chainHeight, forkHeight > 0 ? forkHeight - 1 : chainHeight);
    if (!syncPreForkFilters(0, preForkEnd, progress, allOwned))
      return;

    // Step 3: Sync post-fork range by scanning full blocks
    std::vector<crypto::KeyImage> spentKIs;
    if (chainHeight >= forkHeight)
    {
      scanPostForkBlocks(std::max(forkHeight, 0u), chainHeight, progress, allOwned, spentKIs);
    }

    progress.ownedOutputs = static_cast<uint32_t>(allOwned.size());
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);

    // Report outputs + spent key images
    if (m_onOutputs && (!allOwned.empty() || !spentKIs.empty()))
    {
      m_onOutputs(allOwned, spentKIs);
    }

    m_lastScannedHeight.store(chainHeight);
    saveProgress();
  }

  // ─── Incremental Sync ──────────────────────────────────────────────────────

  void SyncManager::doIncrementalSync(SyncProgress &progress)
  {
    progress = SyncProgress();
    progress.phase = SyncProgress::INCREMENTAL;

    uint32_t walletHeight = m_lastScannedHeight.load();
    uint32_t chainHeight = 0;

    {
      std::vector<cn::BlockFilterRecord> dummy;
      if (!callGetFilterRecords(0, 0, dummy, chainHeight))
      {
        // get_filter_records is unavailable — this is an old-style daemon that
        // does not implement the BoltRPC filter endpoint.  Block scanning falls
        // back to scanDaemonGapBlocks (getTransactionsAtHeight) in SyncEngine.
        // Log once so the operator knows which path is active.
        if (!m_loggedOldDaemon && m_onProgress)
        {
          m_loggedOldDaemon = true;
          SyncProgress p;
          p.errorMessage = "get_filter_records unavailable — old daemon detected; "
                           "falling back to getTransactionsAtHeight via scanDaemonGapBlocks";
          p.phase = SyncProgress::COMPLETE;
          m_onProgress(p);
        }
        return;
      }
    }

    if (chainHeight <= walletHeight)
      return;

    progress.totalBlocks = chainHeight;
    progress.currentHeight = chainHeight;

    uint32_t forkHeight = getForkHeight();
    uint32_t startHeight = walletHeight + 1;
    std::vector<OutputInfo> allOwned;

    // Pre-fork portion (if any)
    if (startHeight < forkHeight)
    {
      uint32_t preForkEnd = std::min(chainHeight, forkHeight - 1);
      if (!syncPreForkFilters(startHeight, preForkEnd, progress, allOwned))
        return;
    }

    // Post-fork portion (if any)
    std::vector<crypto::KeyImage> spentKIs;
    if (chainHeight >= forkHeight)
    {
      uint32_t postForkStart = std::max(startHeight, forkHeight);
      scanPostForkBlocks(postForkStart, chainHeight, progress, allOwned, spentKIs);
    }

    progress.ownedOutputs = static_cast<uint32_t>(allOwned.size());
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);

    if (m_onOutputs && (!allOwned.empty() || !spentKIs.empty()))
    {
      m_onOutputs(allOwned, spentKIs);
    }

    m_lastScannedHeight.store(chainHeight);
    saveProgress();
  }

  // ─── Daemon RPC: get_filter_records ────────────────────────────────────────

  bool SyncManager::callGetFilterRecords(uint32_t startHeight, uint32_t endHeight,
                                         std::vector<cn::BlockFilterRecord> &records,
                                         uint32_t &chainHeight)
  {
    if (!m_rpcCallback)
      return false;

    // Build params
    std::ostringstream paramsJson;
    paramsJson << "{"
               << "\"start_height\":" << startHeight << ","
               << "\"end_height\":" << endHeight
               << "}";

    std::string response = m_rpcCallback("get_filter_records", paramsJson.str());
    if (response.empty())
      return false;

    // Extract result object
    std::string resultJson;
    {
      std::string search = "\"result\":";
      size_t pos = response.find(search);
      if (pos != std::string::npos)
      {
        pos += search.size();
        int depth = 0;
        bool inStr = false;
        size_t start = pos;
        while (pos < response.size())
        {
          char c = response[pos];
          if (c == '"' && (pos == start || response[pos - 1] != '\\'))
            inStr = !inStr;
          if (!inStr)
          {
            if (c == '{')
              ++depth;
            else if (c == '}')
            {
              if (depth == 0)
              {
                ++pos;
                break;
              }
              --depth;
            }
          }
          ++pos;
        }
        resultJson = response.substr(start, pos - start);
      }
      else
      {
        resultJson = response;
      }
    }

    // Parse status
    std::string status = jsonGetString(resultJson, "status");
    if (status != "OK" && !resultJson.empty())
    {
      // If we got records, parse them anyway; status field is advisory
    }

    // Parse records array
    std::string recordsArr = jsonExtractArray(resultJson, "records");
    std::vector<std::string> recordObjs = jsonSplitObjects(recordsArr);
    records.clear();
    records.reserve(recordObjs.size());
    for (const auto &obj : recordObjs)
      records.push_back(parseFilterRecord(obj));

    // Try to get current_height from response (may not be present if just fetching records)
    if (!jsonGetUint(resultJson, "current_height", chainHeight))
    {
      // Fallback: derive from the last record
      if (!records.empty())
        chainHeight = records.back().block_height;
    }

    return true;
  }

  // ─── Daemon RPC: get_blocks (by height) ────────────────────────────────────

  bool SyncManager::callGetBlocks(const std::vector<uint32_t> &heights,
                                  std::vector<cn::Block> &blocks,
                                  std::vector<std::vector<cn::Transaction>> &transactions)
  {
    blocks.clear();
    transactions.clear();

    if (heights.empty())
      return true;

    std::vector<std::vector<cn::BlockDetails>> blockDetailsVec;
    std::error_code ec;
    bool done = false;

    m_node.getBlocks(heights, blockDetailsVec, [&](std::error_code e)
                     {
      ec = e;
      done = true; });

    while (!done && !m_stop.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (ec || m_stop.load())
      return false;

    for (const auto &detailsVec : blockDetailsVec)
    {
      for (const auto &details : detailsVec)
      {
        // Convert BlockDetails → Block
        cn::Block block;
        block.majorVersion = details.majorVersion;
        block.minorVersion = details.minorVersion;
        block.timestamp = details.timestamp;
        block.previousBlockHash = details.prevBlockHash;
        block.nonce = details.nonce;
        block.baseTransaction = cn::Transaction(); // coinbase not in details

        std::vector<cn::Transaction> txs;
        for (const auto &txDetail : details.transactions)
        {
          // Convert TransactionDetails → Transaction
          cn::Transaction tx;
          // Fetch the raw transaction via sync method
          if (m_node.getTransactionSync(txDetail.hash, tx))
          {
            block.transactionHashes.push_back(txDetail.hash);
            txs.push_back(tx);
          }
        }

        blocks.push_back(block);
        transactions.push_back(std::move(txs));
      }
    }

    return !blocks.empty();
  }

  // ─── View-tag filter ───────────────────────────────────────────────────────

  void SyncManager::runFilterPass(const std::vector<cn::BlockFilterRecord> &records,
                                  std::vector<FilterCandidate> &candidates)
  {
    candidates.clear();

    // Pre-allocate: expect ~1/256 of outputs to pass view tag
    size_t totalEntries = 0;
    for (const auto &rec : records)
      totalEntries += rec.entries.size();
    candidates.reserve(totalEntries / 256 + 100);

    for (const auto &record : records)
    {
      for (const auto &entry : record.entries)
      {
        // Compute derivation from the txPubKey in the filter entry
        crypto::KeyDerivation derivation;
        if (!crypto::generate_key_derivation(entry.txPubKey, m_viewSecretKey, derivation))
          continue;

        // View tag check
        uint8_t computedTag = BoltSync::computeWalletViewTag(derivation, entry.output_index);
        if (computedTag != entry.view_tag)
          continue;

        // View tag passed! Store as candidate for full ECDH derivation
        FilterCandidate candidate;
        candidate.blockHeight = record.block_height;
        candidate.txIndex = entry.tx_index;
        candidate.outputIndex = entry.output_index;
        candidates.push_back(candidate);
      }
    }
  }

  // ─── Full derivation on candidates ─────────────────────────────────────────

  void SyncManager::deriveFromCandidates(
      const std::vector<FilterCandidate> &candidates,
      const std::vector<cn::Block> &blocks,
      const std::vector<std::vector<cn::Transaction>> &transactions,
      std::vector<OutputInfo> &owned)
  {
    owned.clear();

    // Build height → (block, transactions) map
    std::unordered_map<uint32_t, std::pair<const cn::Block *, const std::vector<cn::Transaction> *>> blockMap;
    for (size_t i = 0; i < blocks.size(); ++i)
    {
      if (!blocks[i].baseTransaction.inputs.empty() &&
          blocks[i].baseTransaction.inputs[0].type() == typeid(cn::BaseInput))
      {
        uint32_t h = boost::get<cn::BaseInput>(blocks[i].baseTransaction.inputs[0]).blockIndex;
        blockMap[h] = {&blocks[i], &transactions[i]};
      }
    }

    for (const auto &candidate : candidates)
    {
      if (m_stop.load())
        break;

      auto it = blockMap.find(candidate.blockHeight);
      if (it == blockMap.end())
        continue;

      const cn::Block &block = *it->second.first;
      const std::vector<cn::Transaction> &txs = *it->second.second;

      // Get the right transaction
      const cn::Transaction *tx = nullptr;
      if (candidate.txIndex == 0)
        tx = &block.baseTransaction;
      else if (static_cast<size_t>(candidate.txIndex) - 1 < txs.size())
        tx = &txs[candidate.txIndex - 1];
      else
        continue;

      // Get the right output
      if (candidate.outputIndex >= tx->outputs.size())
        continue;

      const auto &output = tx->outputs[candidate.outputIndex];
      if (output.target.type() != typeid(cn::KeyOutput))
        continue;

      const auto &keyOut = boost::get<cn::KeyOutput>(output.target);
      crypto::PublicKey txPubKey = getTransactionPublicKeyFromExtra(tx->extra);

      if (txPubKey == NULL_PUBLIC_KEY)
        continue;

      // Full ECDH derivation
      crypto::KeyDerivation derivation;
      if (!crypto::generate_key_derivation(txPubKey, m_viewSecretKey, derivation))
        continue;

      crypto::PublicKey derivedKey;
      if (!crypto::derive_public_key(derivation, candidate.outputIndex, m_spendPublicKey, derivedKey))
        continue;

      if (derivedKey == keyOut.key)
      {
        OutputInfo info;
        info.blockHeight = candidate.blockHeight;
        info.txHash = getObjectHash(*tx);
        info.amount = output.amount;
        info.outputIndex = candidate.outputIndex;
        info.outputKey = keyOut.key;
        info.txPublicKey = txPubKey;
        info.spent = false;
        info.isDeposit = false;
        info.term = 0;
        owned.push_back(info);
      }
    }
  }

  bool SyncManager::isOurOutput(const crypto::PublicKey &txPubKey,
                                size_t outputIndex,
                                const crypto::PublicKey &outputKey) const
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, m_viewSecretKey, derivation))
      return false;

    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, outputIndex, m_spendPublicKey, derivedKey))
      return false;

    return std::memcmp(&derivedKey, &outputKey, sizeof(crypto::PublicKey)) == 0;
  }

  // ─── Persistence ───────────────────────────────────────────────────────────

  std::string SyncManager::progressPath() const
  {
    return m_dataDir + "/bolt_sync_progress.bin";
  }

  void SyncManager::loadProgress()
  {
    std::ifstream file(progressPath(), std::ios::binary);
    if (!file.is_open())
      return;

    uint32_t height = 0;
    file.read(reinterpret_cast<char *>(&height), sizeof(height));
    if (file)
      m_lastScannedHeight.store(height);
  }

  void SyncManager::saveProgress()
  {
    std::ofstream file(progressPath(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return;

    uint32_t height = m_lastScannedHeight.load();
    file.write(reinterpret_cast<const char *>(&height), sizeof(height));
  }

  // ─── Fork height ────────────────────────────────────────────────────────────

  uint32_t SyncManager::getForkHeight() const
  {
    // Hardcoded from CryptoNoteConfig — matches daemon's UPGRADE_HEIGHT_V9.
    // Once the daemon exposes this via get_info, switch to RPC query.
    return cn::parameters::UPGRADE_HEIGHT_V9;
  }

  // ─── Pre-fork: filter-based sync ────────────────────────────────────────────

  bool SyncManager::syncPreForkFilters(uint32_t startHeight, uint32_t endHeight,
                                       SyncProgress &progress,
                                       std::vector<OutputInfo> &owned)
  {
    if (startHeight > endHeight)
      return true;

    progress.phase = SyncProgress::FETCHING_FILTERS;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 1: Download filter records for the pre-fork range
    std::vector<cn::BlockFilterRecord> allRecords;
    allRecords.reserve(endHeight - startHeight + 1);

    for (uint32_t h = startHeight; h <= endHeight; h += FILTER_BATCH_SIZE)
    {
      if (m_stop.load())
        return false;

      uint32_t batchEnd = std::min(h + FILTER_BATCH_SIZE - 1, endHeight);
      std::vector<cn::BlockFilterRecord> batch;
      uint32_t dummyHeight;

      if (!callGetFilterRecords(h, batchEnd, batch, dummyHeight))
      {
        progress.errorMessage = "Failed to fetch filter records at height " + std::to_string(h);
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return false;
      }

      allRecords.insert(allRecords.end(), batch.begin(), batch.end());
      progress.processedBlocks = batchEnd;
      if (m_onProgress)
        m_onProgress(progress);
    }

    if (m_stop.load())
      return false;

    // Step 2: Run view-tag filter locally
    progress.phase = SyncProgress::FILTERING;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<FilterCandidate> candidates;
    runFilterPass(allRecords, candidates);

    allRecords.clear();
    allRecords.shrink_to_fit();

    progress.candidatesFound = static_cast<uint32_t>(candidates.size());
    if (m_onProgress)
      m_onProgress(progress);

    if (candidates.empty())
      return true; // No owned outputs in this range

    // Step 3: Fetch candidate blocks
    progress.phase = SyncProgress::FETCHING_BLOCKS;
    if (m_onProgress)
      m_onProgress(progress);

    std::set<uint32_t> candidateHeights;
    for (const auto &c : candidates)
      candidateHeights.insert(c.blockHeight);

    std::vector<uint32_t> heights(candidateHeights.begin(), candidateHeights.end());

    std::vector<cn::Block> blocks;
    std::vector<std::vector<cn::Transaction>> transactions;

    for (size_t i = 0; i < heights.size(); i += BLOCK_BATCH_SIZE)
    {
      if (m_stop.load())
        return false;

      size_t endIdx = std::min(i + BLOCK_BATCH_SIZE, heights.size());
      std::vector<uint32_t> batch(heights.begin() + i, heights.begin() + endIdx);

      std::vector<cn::Block> batchBlocks;
      std::vector<std::vector<cn::Transaction>> batchTxs;

      if (!callGetBlocks(batch, batchBlocks, batchTxs))
      {
        progress.errorMessage = "Failed to fetch candidate blocks";
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return false;
      }

      blocks.insert(blocks.end(), batchBlocks.begin(), batchBlocks.end());
      transactions.insert(transactions.end(), batchTxs.begin(), batchTxs.end());
    }

    if (m_stop.load())
      return false;

    // Step 4: Full ECDH derivation
    progress.phase = SyncProgress::DERIVING;
    if (m_onProgress)
      m_onProgress(progress);

    deriveFromCandidates(candidates, blocks, transactions, owned);

    return true;
  }

  // ─── Post-fork: direct block scanning ────────────────────────────────────────

  void SyncManager::scanPostForkBlocks(uint32_t startHeight, uint32_t endHeight,
                                       SyncProgress &progress,
                                       std::vector<OutputInfo> &owned,
                                       std::vector<crypto::KeyImage> &spentKIs)
  {
    if (startHeight > endHeight)
      return;

    progress.phase = SyncProgress::FETCHING_BLOCKS;
    progress.totalBlocks = endHeight;
    if (m_onProgress)
      m_onProgress(progress);

    for (uint32_t h = startHeight; h <= endHeight; h += POST_FORK_BLOCK_BATCH)
    {
      if (m_stop.load())
        return;

      uint32_t batchEnd = std::min(h + POST_FORK_BLOCK_BATCH - 1, endHeight);
      std::vector<uint32_t> heights;
      for (uint32_t bh = h; bh <= batchEnd; ++bh)
        heights.push_back(bh);

      std::vector<cn::Block> blocks;
      std::vector<std::vector<cn::Transaction>> transactions;

      if (!callGetBlocks(heights, blocks, transactions))
      {
        progress.errorMessage = "Failed to fetch blocks for post-fork scan at height " + std::to_string(h);
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }

      progress.phase = SyncProgress::SCANNING_POST_FORK;
      if (m_onProgress)
        m_onProgress(progress);

      // Scan each block's transactions with NewOutputScanner
      for (size_t i = 0; i < blocks.size(); ++i)
      {
        if (m_stop.load())
          return;

        cn::Block &block = blocks[i];
        std::vector<cn::Transaction> &txs = transactions[i];
        uint32_t blockHeight = h + static_cast<uint32_t>(i);

        // Get coinbase tx public key
        crypto::PublicKey coinbaseTxPubKey = cn::getTransactionPublicKeyFromExtra(block.baseTransaction.extra);

        static const crypto::PublicKey NULL_KEY = {};
        if (coinbaseTxPubKey != NULL_KEY)
        {
          std::vector<BoltSync::FoundOutput> txResults;
          std::vector<uint32_t> emptyIndexes; // Global indexes not available via RPC sync
          BoltCore::NewOutputScanner::scanTransaction(
              block.baseTransaction, coinbaseTxPubKey, emptyIndexes, blockHeight,
              m_viewSecretKey, m_spendPublicKey, nullptr, txResults);

          for (const auto &fo : txResults)
          {
            if (!fo.spent)
            {
              OutputInfo info;
              info.blockHeight = fo.blockHeight;
              info.txHash = fo.txHash;
              info.amount = fo.amount;
              info.outputIndex = fo.outputIndex;
              info.globalOutputIndex = fo.globalOutputIndex;
              info.hasGlobalOutputIndex = fo.hasGlobalOutputIndex;
              info.outputKey = fo.outputKey;
              info.txPublicKey = fo.txPublicKey;
              info.spent = false;
              info.isDeposit = fo.isDeposit;
              info.term = fo.term;
              info.keyDerivationIndex = fo.keyDerivationIndex;
              info.hasKeyDerivationIndex = fo.hasKeyDerivationIndex;
              owned.push_back(info);
            }
          }
        }

        // Scan each non-coinbase transaction
        for (size_t txIdx = 0; txIdx < txs.size(); ++txIdx)
        {
          if (m_stop.load())
            return;

          cn::Transaction &tx = txs[txIdx];
          crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);

          if (txPubKey == NULL_KEY)
            continue;

          std::vector<BoltSync::FoundOutput> txResults;
          std::vector<uint32_t> emptyIndexes;
          BoltCore::NewOutputScanner::scanTransaction(
              tx, txPubKey, emptyIndexes, blockHeight,
              m_viewSecretKey, m_spendPublicKey, nullptr, txResults);

          for (const auto &fo : txResults)
          {
            if (!fo.spent)
            {
              OutputInfo info;
              info.blockHeight = fo.blockHeight;
              info.txHash = fo.txHash;
              info.amount = fo.amount;
              info.outputIndex = fo.outputIndex;
              info.globalOutputIndex = fo.globalOutputIndex;
              info.hasGlobalOutputIndex = fo.hasGlobalOutputIndex;
              info.outputKey = fo.outputKey;
              info.txPublicKey = fo.txPublicKey;
              info.spent = false;
              info.isDeposit = fo.isDeposit;
              info.term = fo.term;
              info.keyDerivationIndex = fo.keyDerivationIndex;
              info.hasKeyDerivationIndex = fo.hasKeyDerivationIndex;
              owned.push_back(info);
            }
          }

          // Collect key images from transaction inputs for spend detection.
          // The caller matches these against known wallet outputs.
          for (const auto &input : tx.inputs)
          {
            if (input.type() == typeid(cn::KeyInput))
              spentKIs.push_back(boost::get<cn::KeyInput>(input).keyImage);
          }
        }
      }

      progress.processedBlocks = batchEnd;
      if (m_onProgress)
        m_onProgress(progress);
    }
  }
} // namespace BoltRPC