// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "SyncManager.h"
#include "Common/JsonValue.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "INode.h"
#include "Rpc/HttpClient.h"

#include <algorithm>
#include <cstring>
#include <future>
#include <fstream>
#include <sstream>

using namespace cn;

namespace BoltRPC
{

  // ─── JSON helpers (no external dependency) ─────────────────────────────────

  namespace
  {

    std::string jsonEscape(const std::string &s)
    {
      std::string out;
      out.reserve(s.size() + 4);
      for (char c : s)
      {
        if (c == '"' || c == '\\')
          out += '\\';
        out += c;
      }
      return out;
    }

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

    bool jsonGetUint64(const std::string &json, const std::string &key, uint64_t &out)
    {
      std::string search = "\"" + key + "\":";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return false;
      pos += search.size();
      while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n'))
        ++pos;
      char *end = nullptr;
      out = std::strtoull(json.c_str() + pos, &end, 10);
      return end != json.c_str() + pos;
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

    size_t findJsonArrayStart(const std::string &json, const std::string &key)
    {
      const std::string patterns[] = {
          "\"" + key + "\":[",
          "\"" + key + "\": [",
          "\\\"" + key + "\\\":[",
          "\\\"" + key + "\\\": ["};
      for (const auto &search : patterns)
      {
        size_t pos = json.find(search);
        if (pos != std::string::npos)
          return pos + search.size();
      }
      return std::string::npos;
    }

    std::vector<std::string> jsonGetStringArray(const std::string &json, const std::string &key)
    {
      std::vector<std::string> result;
      size_t pos = findJsonArrayStart(json, key);
      if (pos == std::string::npos)
        return result;
      while (pos < json.size())
      {
        if (json[pos] == ']')
          break;
        if (json[pos] == ',' || json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\t')
        {
          ++pos;
          continue;
        }
        if (json[pos] == '"')
        {
          size_t start = pos + 1;
          size_t end = start;
          while (end < json.size() && json[end] != '"')
            ++end;
          if (end < json.size())
          {
            result.push_back(json.substr(start, end - start));
            pos = end + 1;
            continue;
          }
        }
        ++pos;
      }
      return result;
    }

    std::string jsonExtractArray(const std::string &json, const std::string &key);
    std::vector<std::string> jsonSplitObjects(const std::string &arrayContent);
    OutputInfo parseOutputObject(const std::string &obj);

    // Scan-only parsing — safe for multi‑MB snapshots (do not JsonValue::fromString the whole blob).
    void parseSnapshotFields(const std::string &snapshotJson,
                             uint32_t &currentHeight,
                             uint32_t &reportedKeyCount,
                             std::vector<crypto::PublicKey> &newTxPubKeys,
                             std::vector<OutputInfo> &outputs,
                             std::unordered_set<std::string> &spentKeyImages,
                             bool parseOutputs,
                             size_t &hexEntries,
                             size_t &hexDecoded,
                             SyncProgress *progressOut = nullptr)
    {
      hexEntries = 0;
      hexDecoded = 0;
      jsonGetUint(snapshotJson, "current_height", currentHeight);
      jsonGetUint(snapshotJson, "new_tx_pub_key_count", reportedKeyCount);

      if (progressOut)
      {
        progressOut->currentHeight = currentHeight;
        progressOut->totalKeys = reportedKeyCount;
      }

      std::vector<std::string> newKeysArr = jsonGetStringArray(snapshotJson, "new_tx_pub_keys");
      hexEntries = newKeysArr.size();
      newTxPubKeys.clear();
      newTxPubKeys.reserve(newKeysArr.size());
      for (const auto &pkHex : newKeysArr)
      {
        crypto::PublicKey pk;
        if (common::podFromHex(pkHex, pk))
        {
          newTxPubKeys.push_back(pk);
          ++hexDecoded;
        }
      }

      if (!parseOutputs)
        return;

      std::string outputsArr = jsonExtractArray(snapshotJson, "outputs");
      std::vector<std::string> outputObjs = jsonSplitObjects(outputsArr);
      outputs.clear();
      outputs.reserve(outputObjs.size());
      for (const auto &obj : outputObjs)
        outputs.push_back(parseOutputObject(obj));

      spentKeyImages.clear();
      std::vector<std::string> spentArr = jsonGetStringArray(snapshotJson, "spent_key_images");
      for (const auto &kiHex : spentArr)
        spentKeyImages.insert(kiHex);
    }

    size_t findSnapshotValueStart(const std::string &response)
    {
      const char *markers[] = {"\"snapshot\":\"", "\"snapshot\": \""};
      for (const char *marker : markers)
      {
        size_t pos = response.find(marker);
        if (pos != std::string::npos)
          return pos + std::strlen(marker);
      }
      return std::string::npos;
    }

    bool hasJsonRpcErrorEnvelope(const std::string &response)
    {
      if (response.find("\"result\"") != std::string::npos)
        return false;
      return response.find("\"error\":") != std::string::npos ||
             response.find("\"error\" :") != std::string::npos;
    }

    // Unescape a JSON string value after "field":" (handles large RPC bodies without JsonValue::fromString).
    std::string extractEscapedJsonStringValue(const std::string &json, const std::string &field)
    {
      const std::string search = "\"" + field + "\":\"";
      size_t pos = json.find(search);
      if (pos == std::string::npos)
        return "";
      pos += search.size();
      std::string out;
      out.reserve(json.size() - pos);
      for (size_t i = pos; i < json.size(); ++i)
      {
        char c = json[i];
        if (c == '"')
          break;
        if (c == '\\' && i + 1 < json.size())
        {
          char e = json[++i];
          switch (e)
          {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          default:
            out += e;
            break;
          }
          continue;
        }
        out += c;
      }
      return out;
    }

    std::string extractSnapshotFromDaemonRpc(const std::string &response)
    {
      size_t start = findSnapshotValueStart(response);
      if (start == std::string::npos)
        return "";

      std::string out;
      out.reserve(response.size() - start);
      for (size_t i = start; i < response.size(); ++i)
      {
        char c = response[i];
        if (c == '"')
          break;
        if (c == '\\' && i + 1 < response.size())
        {
          char e = response[++i];
          switch (e)
          {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          default:
            out += e;
            break;
          }
          continue;
        }
        out += c;
      }
      return out;
    }

    uint32_t extractDaemonRpcUint(const std::string &response, const std::string &field)
    {
      uint32_t value = 0;
      if (jsonGetUint(response, field, value))
        return value;
      size_t resultPos = response.find("\"result\"");
      if (resultPos != std::string::npos && jsonGetUint(response.substr(resultPos), field, value))
        return value;
      return 0;
    }

    std::string describeDaemonRpcFailure(const std::string &response)
    {
      if (response.empty())
        return "no response from daemon (is conceald-archive running? check --daemon-host/--daemon-port, default 16000)";

      if (hasJsonRpcErrorEnvelope(response))
      {
        std::string msg = extractEscapedJsonStringValue(response, "message");
        if (!msg.empty())
          return msg;
      }

      try
      {
        common::JsonValue rpc = common::JsonValue::fromString(response);
        if (rpc.isObject() && rpc.contains("error"))
        {
          const common::JsonValue &err = rpc("error");
          if (err.isObject() && err.contains("message") && err("message").isString())
            return err("message").getString();
        }
      }
      catch (...)
      {
      }

      if (response.find("\"result\"") == std::string::npos)
        return "daemon returned no JSON-RPC result";

      return "could not parse get_wallet_snapshot response";
    }

    // Extract the content of a JSON array (raw text between [ and ], handles nesting)
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

    // Split JSON array of objects into individual object strings
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

    OutputInfo parseOutputObject(const std::string &obj)
    {
      OutputInfo info;
      std::memset(&info, 0, sizeof(info));

      jsonGetUint(obj, "height", info.blockHeight);
      jsonGetUint64(obj, "amount", info.amount);
      jsonGetUint(obj, "output_index", info.outputIndex);

      std::string txHashHex = jsonGetString(obj, "tx_hash");
      if (!txHashHex.empty())
        common::podFromHex(txHashHex, info.txHash);

      std::string outputKeyHex = jsonGetString(obj, "output_key");
      if (!outputKeyHex.empty())
        common::podFromHex(outputKeyHex, info.outputKey);

      std::string txPubkeyHex = jsonGetString(obj, "tx_pubkey");
      if (!txPubkeyHex.empty())
        common::podFromHex(txPubkeyHex, info.txPublicKey);

      return info;
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
    loadCachedKeys();
  }

  SyncManager::~SyncManager()
  {
    stop();
    saveCachedKeys();
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

  // ─── Main Loop ─────────────────────────────────────────────────────────────

  void SyncManager::runLoop()
  {
    try
    {
      SyncProgress progress;

      // ── Determine if this is first sync ────────────────────────────────────
      {
        std::lock_guard<std::mutex> lock(m_keysMutex);
        if (m_knownTxPubKeys.empty())
        {
          doBootstrap(progress);
        }
      }

      // ── Background incremental loop ────────────────────────────────────────
      while (!m_stop.load())
      {
        // Wait for poll interval or manual trigger
        for (uint32_t i = 0; i < POLL_INTERVAL_SECONDS * 2 && !m_stop.load() && !m_triggerSync.load(); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(500));

        if (m_stop.load())
          break;

        doIncrementalSync(progress);

        m_triggerSync.store(false);
      }
    }
    catch (const std::exception &e)
    {
      SyncProgress progress;
      progress.phase = SyncProgress::COMPLETE;
      progress.errorMessage = std::string("Sync thread error: ") + e.what();
      if (m_onProgress)
        m_onProgress(progress);
    }
    catch (...)
    {
      SyncProgress progress;
      progress.phase = SyncProgress::COMPLETE;
      progress.errorMessage = "Sync thread error: unknown exception";
      if (m_onProgress)
        m_onProgress(progress);
    }
  }

  // ─── Bootstrap Sync ────────────────────────────────────────────────────────

  void SyncManager::doBootstrap(SyncProgress &progress)
  {
    // Step 1: Fetch all tx_pub_keys from daemon
    progress = SyncProgress();
    progress.phase = SyncProgress::FETCHING_KEYS;
    if (m_rpcCallback)
      progress.currentHeight = extractDaemonRpcUint(m_rpcCallback("getblockcount", "{}"), "count");
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<crypto::PublicKey> allKeys;
    uint32_t chainHeight = 0;

    {
      std::vector<OutputInfo> dummyOutputs;
      std::unordered_set<std::string> dummySpent;
      std::vector<crypto::PublicKey> newKeys;

      // Empty tx_pub_keys → daemon returns all keys from tx_pubkey_seen index
      std::vector<crypto::PublicKey> emptyKeys;
      std::string rpcError;
      if (!callGetWalletSnapshot(emptyKeys, 0, dummyOutputs, dummySpent, newKeys, chainHeight, &rpcError, &progress))
      {
        progress.errorMessage = "Failed to fetch tx_pub_keys from daemon: " + rpcError;
        progress.phase = SyncProgress::COMPLETE;
        if (m_onProgress)
          m_onProgress(progress);
        return;
      }

      allKeys = std::move(newKeys);
      progress.currentHeight = chainHeight;
      progress.totalKeys = static_cast<uint32_t>(allKeys.size());
      progress.processedKeys = progress.totalKeys;
      if (m_onProgress)
        m_onProgress(progress);
    }

    if (allKeys.empty())
    {
      std::ostringstream msg;
      msg << "Daemon returned no tx_pub_keys at height " << chainHeight
          << ". Archive node needs wallet indexes: use --enable-wallet-indexes when first "
             "creating the DB; use --rebuild-wallet-indexes if the chain was synced/migrated "
             "without indexes. If curl to the same host:port shows keys, set conceal-rpc "
             "--daemon-host/--daemon-port to that archive (default 16000) and retry syncNow.";
      progress.errorMessage = msg.str();
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    if (m_stop.load())
      return;
    if (m_onProgress)
      m_onProgress(progress);

    // Cache keys now so we don't lose them if interrupted
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      m_knownTxPubKeys = allKeys;
    }
    saveCachedKeys();

    // Step 2: Fetch all candidate outputs (batched — do not POST millions of keys in one RPC)
    progress.phase = SyncProgress::FETCHING_OUTPUTS;
    progress.totalKeys = static_cast<uint32_t>(allKeys.size());
    progress.processedKeys = 0;
    progress.totalOutputs = 0;
    progress.processedOutputs = 0;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<OutputInfo> allOutputs;
    std::unordered_set<std::string> allSpentImages;
    std::vector<crypto::PublicKey> additionalKeys;

    std::string outputFetchError;
    // wallet_height = chainHeight skips re-downloading the full new_tx_pub_keys list
    if (!fetchOutputsForKeys(allKeys, chainHeight, allOutputs, allSpentImages, additionalKeys, chainHeight,
                             &progress, &outputFetchError))
    {
      progress.errorMessage = outputFetchError.empty() ? "Failed to fetch outputs from daemon" : outputFetchError;
      progress.phase = SyncProgress::COMPLETE;
      if (m_onProgress)
        m_onProgress(progress);
      return;
    }

    progress.totalOutputs = static_cast<uint32_t>(allOutputs.size());
    progress.currentHeight = chainHeight;
    m_lastScannedHeight.store(chainHeight);

    if (!additionalKeys.empty())
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      for (const auto &pk : additionalKeys)
      {
        if (std::find(m_knownTxPubKeys.begin(), m_knownTxPubKeys.end(), pk) == m_knownTxPubKeys.end())
          m_knownTxPubKeys.push_back(pk);
      }
      saveCachedKeys();
    }

    if (m_stop.load())
      return;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 3: Derive ownership locally
    progress.phase = SyncProgress::DERIVING;
    if (m_onProgress)
      m_onProgress(progress);

    std::vector<OutputInfo> ownedOutputs;
    deriveOwnedOutputs(allOutputs, allSpentImages, ownedOutputs);

    progress.processedOutputs = progress.totalOutputs;
    progress.ownedOutputs = static_cast<uint32_t>(ownedOutputs.size());
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);

    // Step 4: Report to wallet
    if (m_onOutputs && !ownedOutputs.empty())
    {
      std::vector<crypto::KeyImage> empty;
      m_onOutputs(ownedOutputs, empty);
    }
  }

  // ─── Incremental Sync ──────────────────────────────────────────────────────

  void SyncManager::doIncrementalSync(SyncProgress &progress)
  {
    progress = SyncProgress();
    progress.phase = SyncProgress::INCREMENTAL;

    std::vector<crypto::PublicKey> keys;
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      keys = m_knownTxPubKeys;
    }

    uint32_t walletHeight = m_lastScannedHeight.load();

    std::vector<OutputInfo> newOutputs;
    std::unordered_set<std::string> spentImages;
    std::vector<crypto::PublicKey> newKeys;
    uint32_t chainHeight = 0;

    if (!fetchOutputsForKeys(keys, walletHeight, newOutputs, spentImages, newKeys, chainHeight, &progress))
    {
      // Silently retry on next poll
      return;
    }

    progress.totalOutputs = static_cast<uint32_t>(newOutputs.size());
    progress.currentHeight = chainHeight;

    if (!newOutputs.empty())
    {
      std::vector<OutputInfo> owned;
      deriveOwnedOutputs(newOutputs, spentImages, owned);

      progress.processedOutputs = progress.totalOutputs;
      progress.ownedOutputs = static_cast<uint32_t>(owned.size());

      if (m_onOutputs && !owned.empty())
      {
        std::vector<crypto::KeyImage> empty;
        m_onOutputs(owned, empty);
      }
    }

    // Add new keys to cache
    if (!newKeys.empty())
    {
      std::lock_guard<std::mutex> lock(m_keysMutex);
      bool changed = false;
      for (const auto &pk : newKeys)
      {
        if (std::find(m_knownTxPubKeys.begin(), m_knownTxPubKeys.end(), pk) == m_knownTxPubKeys.end())
        {
          m_knownTxPubKeys.push_back(pk);
          changed = true;
        }
      }
      if (changed)
        saveCachedKeys();
    }

    m_lastScannedHeight.store(chainHeight);
    progress.phase = SyncProgress::COMPLETE;
    if (m_onProgress)
      m_onProgress(progress);
  }

  // ─── Batched output fetch ────────────────────────────────────────────────────

  bool SyncManager::fetchOutputsForKeys(
      const std::vector<crypto::PublicKey> &keys,
      uint32_t walletHeight,
      std::vector<OutputInfo> &outputs,
      std::unordered_set<std::string> &spentKeyImages,
      std::vector<crypto::PublicKey> &additionalKeys,
      uint32_t &currentHeight,
      SyncProgress *progress,
      std::string *errorOut)
  {
    outputs.clear();
    spentKeyImages.clear();
    additionalKeys.clear();

    if (keys.empty())
      return callGetWalletSnapshot(keys, walletHeight, outputs, spentKeyImages, additionalKeys, currentHeight,
                                   errorOut, progress);

    const size_t batchSize = WALLET_SNAPSHOT_KEY_BATCH_SIZE;
    if (keys.size() <= batchSize)
      return callGetWalletSnapshot(keys, walletHeight, outputs, spentKeyImages, additionalKeys, currentHeight,
                                   errorOut, progress);

    if (progress)
    {
      progress->totalKeys = static_cast<uint32_t>(keys.size());
      progress->processedKeys = 0;
    }

    for (size_t offset = 0; offset < keys.size(); offset += batchSize)
    {
      if (m_stop.load())
        return false;

      const size_t end = std::min(offset + batchSize, keys.size());
      std::vector<crypto::PublicKey> batch(keys.begin() + static_cast<std::ptrdiff_t>(offset),
                                           keys.begin() + static_cast<std::ptrdiff_t>(end));

      std::vector<OutputInfo> batchOutputs;
      std::unordered_set<std::string> batchSpent;
      std::vector<crypto::PublicKey> batchAdditional;

      // After the first batch, pin wallet_height to chain tip so daemon does not re-send new_tx_pub_keys
      const uint32_t batchWalletHeight =
          (offset == 0) ? walletHeight : (currentHeight > 0 ? currentHeight : walletHeight);

      if (!callGetWalletSnapshot(batch, batchWalletHeight, batchOutputs, batchSpent, batchAdditional, currentHeight,
                                 errorOut))
        return false;

      outputs.insert(outputs.end(), batchOutputs.begin(), batchOutputs.end());
      for (const auto &ki : batchSpent)
        spentKeyImages.insert(ki);
      for (const auto &pk : batchAdditional)
      {
        if (std::find(additionalKeys.begin(), additionalKeys.end(), pk) == additionalKeys.end())
          additionalKeys.push_back(pk);
      }

      if (progress)
      {
        progress->processedKeys = static_cast<uint32_t>(end);
        progress->processedOutputs = static_cast<uint32_t>(outputs.size());
        if (m_onProgress)
          m_onProgress(*progress);
      }
    }

    return true;
  }

  // ─── Daemon RPC Call ───────────────────────────────────────────────────────

  bool SyncManager::callGetWalletSnapshot(
      const std::vector<crypto::PublicKey> &txPubKeys,
      uint32_t walletHeight,
      std::vector<OutputInfo> &outputs,
      std::unordered_set<std::string> &spentKeyImages,
      std::vector<crypto::PublicKey> &newTxPubKeys,
      uint32_t &currentHeight,
      std::string *errorOut,
      SyncProgress *progressOut)
  {
    // Build JSON-RPC params
    std::ostringstream paramsJson;
    paramsJson << "{";
    paramsJson << R"("tx_pub_keys":[)";
    for (size_t i = 0; i < txPubKeys.size(); ++i)
    {
      if (i > 0)
        paramsJson << ",";
      paramsJson << R"(")" << common::podToHex(txPubKeys[i]) << R"(")";
    }
    paramsJson << R"(],"wallet_height":)" << walletHeight;
    paramsJson << "}";

    // Build full JSON-RPC request
    std::ostringstream requestBody;
    requestBody << R"({"jsonrpc":"2.0","id":1,"method":"get_wallet_snapshot","params":)"
                << paramsJson.str() << "}";

    auto fail = [&](const std::string &reason) -> bool
    {
      if (errorOut)
        *errorOut = reason;
      return false;
    };

    // Send to daemon via HTTP POST to /json_rpc
    if (!m_rpcCallback)
      return fail("daemon RPC callback not configured");

    std::string response = m_rpcCallback("get_wallet_snapshot", paramsJson.str());

    if (response.empty())
      return fail(describeDaemonRpcFailure(response));

    if (hasJsonRpcErrorEnvelope(response))
      return fail(describeDaemonRpcFailure(response));

    static constexpr size_t kMaxJsonTreeParseBytes = 8 * 1024 * 1024;

    // Extract inner snapshot without parsing the full multi‑MB JSON-RPC envelope into a tree.
    std::string resultJson = extractSnapshotFromDaemonRpc(response);
    if (resultJson.empty())
    {
      if (response.size() > kMaxJsonTreeParseBytes)
      {
        return fail("daemon snapshot response is " + std::to_string(response.size()) +
                    " bytes but snapshot field could not be extracted (rebuild conceald-archive + "
                    "conceal-rpc with JSON escape fix)");
      }

      try
      {
        common::JsonValue rpc = common::JsonValue::fromString(response);
        if (!rpc.isObject() || !rpc.contains("result"))
          return fail(describeDaemonRpcFailure(response));

        const common::JsonValue &result = rpc("result");
        if (result.isObject() && result.contains("snapshot") && result("snapshot").isString())
          resultJson = result("snapshot").getString();
        else if (result.isObject())
          resultJson = result.toString();
        else if (result.isString())
          resultJson = result.getString();
        else
          resultJson = result.toString();
      }
      catch (...)
      {
        return fail(describeDaemonRpcFailure(response));
      }
    }

    if (resultJson.empty())
    {
      return fail("empty snapshot in daemon response (" + std::to_string(response.size()) + " byte body)");
    }

    size_t hexEntries = 0;
    size_t hexDecoded = 0;
    uint32_t reportedKeyCount = 0;
    const bool parseOutputs = !txPubKeys.empty();
    parseSnapshotFields(resultJson, currentHeight, reportedKeyCount, newTxPubKeys, outputs,
                        spentKeyImages, parseOutputs, hexEntries, hexDecoded, progressOut);

    if (progressOut && m_onProgress)
      m_onProgress(*progressOut);

    if (newTxPubKeys.empty() && reportedKeyCount > 0)
    {
      return fail("daemon snapshot lists new_tx_pub_key_count=" + std::to_string(reportedKeyCount) +
                  " but wallet parsed 0 keys (" + std::to_string(hexEntries) +
                  " JSON entries, " + std::to_string(hexDecoded) +
                  " decoded). Response may be truncated — check conceal-rpc can reach the same archive "
                  "daemon you curl (host/port) and wait for a full HTTP body.");
    }

    return true;
  }

  // ─── Local Derivation ──────────────────────────────────────────────────────

  bool SyncManager::isOurOutput(const OutputInfo &candidate) const
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(candidate.txPublicKey, m_viewSecretKey, derivation))
      return false;

    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, candidate.outputIndex, m_spendPublicKey, derivedKey))
      return false;

    return std::memcmp(&derivedKey, &candidate.outputKey, sizeof(crypto::PublicKey)) == 0;
  }

  crypto::KeyImage SyncManager::deriveKeyImage(const OutputInfo &output) const
  {
    crypto::KeyImage ki;
    std::memset(&ki, 0, sizeof(ki));

    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(output.txPublicKey, m_viewSecretKey, derivation))
      return ki;

    crypto::PublicKey ephemeralPub;
    if (!crypto::derive_public_key(derivation, output.outputIndex, m_spendPublicKey, ephemeralPub))
      return ki;

    // derive_public_key with spend secret gives us the output secret key equivalent
    // But we only have the spend PUBLIC key, not the secret.
    // To derive a key_image we need the spend SECRET key.
    // If we only have the spend public key, we cannot derive key images locally.
    // The wallet must provide the spend secret key for full spent detection.

    // For now, return empty key image — spent detection requires the spend secret.
    return ki;
  }

  void SyncManager::deriveOwnedOutputs(
      const std::vector<OutputInfo> &candidates,
      const std::unordered_set<std::string> &spentKeyImages,
      std::vector<OutputInfo> &owned)
  {
    owned.clear();
    owned.reserve(std::min(candidates.size(), size_t(10000)));

    size_t batchSize = std::max(size_t(1), candidates.size() / std::thread::hardware_concurrency());
    std::mutex ownedMutex;

    auto processBatch = [&](size_t start, size_t end)
    {
      std::vector<OutputInfo> localOwned;
      for (size_t i = start; i < end && i < candidates.size(); ++i)
      {
        if (m_stop.load())
          break;

        const auto &candidate = candidates[i];
        if (isOurOutput(candidate))
        {
          OutputInfo ownedOutput = candidate;
          ownedOutput.spent = false;
          localOwned.push_back(ownedOutput);

          // Track this tx_pub_key — it produced one of our outputs
          addKnownKey(candidate.txPublicKey);
        }
      }

      if (!localOwned.empty())
      {
        std::lock_guard<std::mutex> lock(ownedMutex);
        owned.insert(owned.end(), localOwned.begin(), localOwned.end());
      }
    };

    std::vector<std::future<void>> futures;
    for (size_t i = 0; i < candidates.size(); i += batchSize)
    {
      size_t end = std::min(i + batchSize, candidates.size());
      futures.push_back(std::async(std::launch::async, processBatch, i, end));
    }

    for (auto &f : futures)
      f.get();

    std::sort(owned.begin(), owned.end(),
              [](const OutputInfo &a, const OutputInfo &b)
              { return a.blockHeight < b.blockHeight; });

    // Save keys after derivation (batch save)
    saveCachedKeys();
  }

  // ─── Persistence ────────────────────────────────────────────────────────────

  std::string SyncManager::keysCachePath() const
  {
    return m_dataDir + "/tx_pub_keys_cache.bin";
  }

  void SyncManager::loadCachedKeys()
  {
    std::ifstream file(keysCachePath(), std::ios::binary);
    if (!file.is_open())
      return;

    uint32_t count = 0;
    file.read(reinterpret_cast<char *>(&count), sizeof(count));
    if (!file || count == 0 || count > 10000000) // sanity: max 10M keys (~320 MB)
      return;

    std::lock_guard<std::mutex> lock(m_keysMutex);
    m_knownTxPubKeys.resize(count);
    file.read(reinterpret_cast<char *>(m_knownTxPubKeys.data()), count * sizeof(crypto::PublicKey));

    // Also load last scanned height
    uint32_t height = 0;
    file.read(reinterpret_cast<char *>(&height), sizeof(height));
    if (file)
      m_lastScannedHeight.store(height);
  }

  void SyncManager::saveCachedKeys()
  {
    std::lock_guard<std::mutex> lock(m_keysMutex);

    std::ofstream file(keysCachePath(), std::ios::binary | std::ios::trunc);
    if (!file.is_open())
      return;

    uint32_t count = static_cast<uint32_t>(m_knownTxPubKeys.size());
    file.write(reinterpret_cast<const char *>(&count), sizeof(count));
    if (!m_knownTxPubKeys.empty())
      file.write(reinterpret_cast<const char *>(m_knownTxPubKeys.data()),
                 m_knownTxPubKeys.size() * sizeof(crypto::PublicKey));

    uint32_t height = m_lastScannedHeight.load();
    file.write(reinterpret_cast<const char *>(&height), sizeof(height));
  }

  // Called by deriveOwnedOutputs when an output is ours
  void SyncManager::addKnownKey(const crypto::PublicKey &txPubKey)
  {
    std::lock_guard<std::mutex> lock(m_keysMutex);
    if (std::find(m_knownTxPubKeys.begin(), m_knownTxPubKeys.end(), txPubKey) == m_knownTxPubKeys.end())
    {
      m_knownTxPubKeys.push_back(txPubKey);
      // Don't save to disk immediately — batch save on shutdown or periodically
    }
  }

} // namespace BoltRPC