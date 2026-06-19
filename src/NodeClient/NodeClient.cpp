// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "NodeClient.h"
#include "BoltHttp/BoltHttpClient.h"
#include "Common/StringTools.h"
#include "Common/VectorOutputStream.h"
#include "Common/StringInputStream.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"
#include "Serialization/SerializationTools.h"
#include "Serialization/SerializationTools.h"
#include "Rpc/CoreRpcServerCommandsDefinitions.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteConfig.h"

#include <cstring>
#include <sstream>
#include <stdexcept>

namespace NodeClient
{

  // ─── JSON helpers ───────────────────────────────────────────────────────

  namespace
  {

    std::string extractResult(const std::string &response)
    {
      size_t pos = response.find("\"result\"");
      if (pos == std::string::npos)
        return response;

      pos = response.find(':', pos);
      if (pos == std::string::npos)
        return response;

      ++pos;
      while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\t' || response[pos] == '\n'))
        ++pos;

      if (pos >= response.size())
        return "";

      size_t start = pos;
      int depth = 0;
      bool inString = false;
      char openChar = response[pos];
      char closeChar = (openChar == '{') ? '}' : (openChar == '[') ? ']'
                                                                   : '\0';

      if (closeChar == '\0')
      {
        size_t end = pos;
        while (end < response.size() && response[end] != ',' && response[end] != '}' && response[end] != ']')
          ++end;
        return response.substr(pos, end - pos);
      }

      for (size_t i = pos; i < response.size(); ++i)
      {
        char c = response[i];
        if (c == '"' && (i == start || response[i - 1] != '\\'))
          inString = !inString;
        if (!inString)
        {
          if (c == openChar)
            ++depth;
          else if (c == closeChar)
          {
            --depth;
            if (depth == 0)
              return response.substr(start, i - start + 1);
          }
        }
      }
      return "";
    }

    std::string extractError(const std::string &response)
    {
      size_t pos = response.find("\"error\"");
      if (pos == std::string::npos)
        return "";

      pos = response.find(':', pos);
      if (pos == std::string::npos)
        return "";

      ++pos;
      while (pos < response.size() && (response[pos] == ' ' || response[pos] == '\t' || response[pos] == '\n'))
        ++pos;

      if (pos >= response.size() || response[pos] == 'n')
        return "";

      return "error present";
    }

    bool jsonGetUint(const std::string &json, const std::string &key, uint64_t &out)
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
      return (end != json.c_str() + pos);
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

    std::string jsonGetHexString(const std::string &json, const std::string &key)
    {
      return jsonGetString(json, key);
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

    // ─── Serialization helpers ──────────────────────────────────────────

    std::string transactionToHex(const cn::Transaction &tx)
    {
      std::vector<uint8_t> txData;
      common::VectorOutputStream stream(txData);
      cn::BinaryOutputStreamSerializer serializer(stream);
      cn::serialize(const_cast<cn::Transaction &>(tx), serializer);
      return common::toHex(txData);
    }

    bool transactionFromHex(const std::string &hex, cn::Transaction &tx)
    {
      try
      {
        std::vector<uint8_t> txData = common::fromHex(hex);
        std::string dataStr(reinterpret_cast<const char *>(txData.data()), txData.size());
        common::StringInputStream stream(dataStr);
        cn::BinaryInputStreamSerializer serializer(stream);
        cn::serialize(tx, serializer);
        return true;
      }
      catch (...)
      {
        return false;
      }
    }

    template <typename Request, typename Response>
    bool jsonCommand(const std::string &host, uint16_t port, const std::string &url,
                     const Request &req, Response &res,
                     int connectTimeoutMs = 5000, int recvTimeoutMs = 10000)
    {
      BoltHttp::HttpClient client(host, port);
      const BoltHttp::HttpClientResponse response =
          client.post(url, storeToJson(req), "application/json", connectTimeoutMs, recvTimeoutMs);
      if (!response.success || response.statusCode != 200)
        return false;
      if (!loadFromJson(res, response.body))
        return false;
      return res.status == cn::CORE_RPC_STATUS_OK;
    }

    template <typename Request, typename Response>
    bool binaryCommand(const std::string &host, uint16_t port, const std::string &url,
                       const Request &req, Response &res,
                       int connectTimeoutMs = 5000, int recvTimeoutMs = 10000,
                       std::string *errorOut = nullptr)
    {
      if (errorOut)
        errorOut->clear();
      BoltHttp::HttpClient client(host, port);
      const BoltHttp::HttpClientResponse response =
          client.post(url, storeToBinaryKeyValue(req), "application/octet-stream",
                      connectTimeoutMs, recvTimeoutMs);
      if (!response.success || response.statusCode != 200)
      {
        if (errorOut)
        {
          if (!response.error.empty())
            *errorOut = response.error;
          else if (response.statusCode == 500 && !response.body.empty())
            *errorOut = response.body;
          else
            *errorOut = "HTTP " + std::to_string(response.statusCode);
        }
        return false;
      }
      if (!loadFromBinaryKeyValue(res, response.body))
      {
        if (errorOut)
          *errorOut = "Failed to parse binary RPC response";
        return false;
      }
      if (res.status != cn::CORE_RPC_STATUS_OK)
      {
        if (errorOut)
          *errorOut = "Daemon status: " + res.status;
        return false;
      }
      return true;
    }

    bool fetchPoolTransactionSync(const std::string &host, uint16_t port,
                                  const crypto::Hash &txHash, cn::Transaction &tx)
    {
      cn::COMMAND_RPC_GET_POOL_CHANGES_LITE::request req{};
      cn::COMMAND_RPC_GET_POOL_CHANGES_LITE::response rsp{};
      req.tailBlockId = cn::NULL_HASH;

      if (binaryCommand(host, port, "/get_pool_changes_lite.bin", req, rsp))
      {
        for (const auto &tpi : rsp.addedTxs)
        {
          if (tpi.txHash == txHash)
          {
            static_cast<cn::TransactionPrefix &>(tx) = tpi.txPrefix;
            return true;
          }
        }
      }

      cn::COMMAND_RPC_GET_RAW_TRANSACTIONS_POOL::request poolReq;
      cn::COMMAND_RPC_GET_RAW_TRANSACTIONS_POOL::response poolResp;
      if (!jsonCommand(host, port, "/getrawtransactionspool", poolReq, poolResp))
        return false;

      for (const auto &entry : poolResp.transactions)
      {
        if (entry.hash == txHash)
        {
          static_cast<cn::TransactionPrefix &>(tx) = entry.transaction;
          return true;
        }
      }
      return false;
    }

    bool fetchRandomOutsJson(const std::string &host, uint16_t port,
                             const std::vector<uint64_t> &amounts, uint64_t outsCount,
                             std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &result,
                             std::string &errorOut, int recvTimeoutMs = 120000)
    {
      errorOut.clear();
      cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_JSON::request req;
      cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS_JSON::response rsp;
      req.amounts = amounts;
      req.outs_count = outsCount;

      BoltHttp::HttpClient client(host, port);
      const BoltHttp::HttpClientResponse response =
          client.post("/getrandom_outs", storeToJson(req), "application/json", 5000, recvTimeoutMs);
      if (!response.success || response.statusCode != 200)
      {
        if (!response.error.empty())
          errorOut = response.error;
        else if (response.statusCode == 500 && !response.body.empty())
          errorOut = response.body;
        else
          errorOut = "HTTP " + std::to_string(response.statusCode);
        return false;
      }
      if (!loadFromJson(rsp, response.body))
      {
        errorOut = "Failed to parse getrandom_outs JSON response";
        return false;
      }
      if (rsp.status != cn::CORE_RPC_STATUS_OK)
      {
        errorOut = "Daemon getrandom_outs status: " + rsp.status;
        return false;
      }

      result.clear();
      result.reserve(rsp.outs.size());
      for (const auto &jentry : rsp.outs)
      {
        cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount entry;
        entry.amount = jentry.amount;
        entry.outs.reserve(jentry.outs.size());
        for (const auto &jo : jentry.outs)
        {
          cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry oe;
          oe.global_amount_index = jo.global_amount_index;
          oe.out_key = jo.out_key;
          entry.outs.push_back(oe);
        }
        result.push_back(std::move(entry));
      }
      return true;
    }

    bool isTransactionInBlockchain(const std::string &host, uint16_t port,
                                   const crypto::Hash &txHash, int recvTimeoutMs)
    {
      const std::string txidHex = common::podToHex(txHash);
      const std::string params = R"({"hash":")" + txidHex + R"("})";

      try
      {
        std::ostringstream body;
        body << R"({"jsonrpc":"2.0","id":1,"method":"f_transaction_json","params":)" << params
             << "}";

        BoltHttp::HttpClient client(host, port);
        const BoltHttp::HttpClientResponse response =
            client.post("/json_rpc", body.str(), "application/json", 5000, recvTimeoutMs);
        if (!response.success || response.statusCode != 200)
          return false;

        if (!extractError(response.body).empty())
          return false;

        const std::string result = extractResult(response.body);
        if (result.empty())
          return false;

        const std::string blockKey = "\"block\":";
        const size_t blockPos = result.find(blockKey);
        if (blockPos == std::string::npos)
          return false;

        size_t pos = blockPos + blockKey.size();
        while (pos < result.size() && (result[pos] == ' ' || result[pos] == '\t' || result[pos] == '\n'))
          ++pos;
        if (pos >= result.size() || result[pos] == 'n')
          return false;

        const std::string hashKey = "\"hash\":\"";
        const size_t hashPos = result.find(hashKey, pos);
        if (hashPos == std::string::npos || hashPos > pos + 512)
          return false;

        const size_t valueStart = hashPos + hashKey.size();
        return valueStart < result.size() && result[valueStart] != '"';
      }
      catch (...)
      {
        return false;
      }
    }

    bool parseOIndexesFromJson(const std::string &json, std::vector<uint32_t> &indices)
    {
      size_t pos = json.find("\"o_indexes\"");
      if (pos == std::string::npos)
        return false;
      pos = json.find('[', pos);
      if (pos == std::string::npos)
        return false;
      ++pos;

      indices.clear();
      while (pos < json.size())
      {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == ','))
          ++pos;
        if (pos >= json.size() || json[pos] == ']')
          break;
        char *end = nullptr;
        const uint64_t value = std::strtoull(json.c_str() + pos, &end, 10);
        if (end == json.c_str() + pos)
          break;
        indices.push_back(static_cast<uint32_t>(value));
        pos = static_cast<size_t>(end - json.c_str());
      }
      return !indices.empty();
    }

    bool fetchTxGlobalOutputIndexesJson(const std::string &host, uint16_t port,
                                        const crypto::Hash &txHash,
                                        std::vector<uint32_t> &indices,
                                        std::string &errorOut, int recvTimeoutMs)
    {
      errorOut.clear();
      const std::string txidHex = common::podToHex(txHash);
      const std::string params = R"({"txid":")" + txidHex + R"("})";

      static const char *kMethods[] = {
          "get_tx_global_output_indexes",
          "get_o_indexes",
          nullptr};

      for (const char **method = kMethods; *method != nullptr; ++method)
      {
        try
        {
          std::ostringstream body;
          body << R"({"jsonrpc":"2.0","id":1,"method":")" << *method
               << R"(","params":)" << params << "}";

          BoltHttp::HttpClient client(host, port);
          const BoltHttp::HttpClientResponse response =
              client.post("/json_rpc", body.str(), "application/json", 5000, recvTimeoutMs);
          if (!response.success || response.statusCode != 200)
            continue;

          if (!extractError(response.body).empty())
            continue;

          const std::string result = extractResult(response.body);
          if (result.empty())
            continue;

          if (parseOIndexesFromJson(result, indices))
            return true;
        }
        catch (...)
        {
        }
      }

      errorOut = "JSON fallback for global output indexes failed";
      return false;
    }

    bool fetchTxGlobalOutputIndexes(const std::string &host, uint16_t port,
                                    const crypto::Hash &txHash,
                                    std::vector<uint32_t> &indices,
                                    std::string &errorOut,
                                    int connectMs = 5000, int recvMs = 30000)
    {
      errorOut.clear();

      // Global output indices exist only for txs included in a block. Skip get_o_indexes
      // for mempool/missing txs so the daemon does not log get_tx_outputs_gindexs warnings.
      if (!isTransactionInBlockchain(host, port, txHash, recvMs))
      {
        errorOut = "Transaction not in blockchain";
        return false;
      }

      cn::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::request req;
      cn::COMMAND_RPC_GET_TX_GLOBAL_OUTPUTS_INDEXES::response rsp;
      req.txid = txHash;

      if (binaryCommand(host, port, "/get_o_indexes.bin", req, rsp, connectMs, recvMs, &errorOut))
      {
        indices.clear();
        indices.reserve(rsp.o_indexes.size());
        for (auto idx : rsp.o_indexes)
          indices.push_back(static_cast<uint32_t>(idx));
        return true;
      }

      if (fetchTxGlobalOutputIndexesJson(host, port, txHash, indices, errorOut, recvMs))
        return true;

      if (errorOut.empty())
        errorOut = "Failed to fetch global output indexes from daemon";
      return false;
    }

    // ─── JSON parsers ───────────────────────────────────────────────────

    cn::TransactionDetails parseTransactionDetails(const std::string &json)
    {
      cn::TransactionDetails tx = {};

      std::string hashHex = jsonGetHexString(json, "hash");
      if (!hashHex.empty() && hashHex.size() == 64)
        common::podFromHex(hashHex, tx.hash);

      jsonGetUint(json, "size", tx.size);
      jsonGetUint(json, "fee", tx.fee);
      jsonGetUint(json, "totalInputsAmount", tx.totalInputsAmount);
      jsonGetUint(json, "totalOutputsAmount", tx.totalOutputsAmount);
      jsonGetUint(json, "mixin", tx.mixin);

      uint64_t unlockTime = 0;
      jsonGetUint(json, "unlockTime", unlockTime);
      tx.unlockTime = unlockTime;

      uint64_t timestamp = 0;
      jsonGetUint(json, "timestamp", timestamp);
      tx.timestamp = timestamp;

      std::string paymentId = jsonGetString(json, "paymentId");
      if (paymentId.size() == 64)
        common::podFromHex(paymentId, tx.paymentId);

      return tx;
    }

    cn::BlockDetails parseBlockDetails(const std::string &json)
    {
      cn::BlockDetails details = {};

      uint64_t height = 0;
      jsonGetUint(json, "blockHeight", height);
      details.height = static_cast<uint32_t>(height);

      jsonGetUint(json, "timestamp", details.timestamp);
      jsonGetUint(json, "blockSize", details.blockSize);
      jsonGetUint(json, "alreadyGeneratedTransactions", details.alreadyGeneratedTransactions);
      jsonGetUint(json, "alreadyGeneratedCoins", details.alreadyGeneratedCoins);
      jsonGetUint(json, "baseReward", details.baseReward);
      jsonGetUint(json, "totalFeeAmount", details.totalFeeAmount);
      jsonGetUint(json, "difficulty", details.difficulty);

      uint64_t majorVersion = 0, minorVersion = 0;
      jsonGetUint(json, "majorVersion", majorVersion);
      jsonGetUint(json, "minorVersion", minorVersion);
      details.majorVersion = static_cast<uint8_t>(majorVersion);
      details.minorVersion = static_cast<uint8_t>(minorVersion);

      uint64_t nonce = 0;
      jsonGetUint(json, "nonce", nonce);
      details.nonce = static_cast<uint32_t>(nonce);

      uint64_t orphaned = 0;
      jsonGetUint(json, "isOrphaned", orphaned);
      details.isOrphaned = (orphaned != 0);

      uint64_t reward = 0;
      jsonGetUint(json, "reward", reward);
      details.reward = reward;

      uint64_t cumulativeSize = 0;
      jsonGetUint(json, "transactionsCumulativeSize", cumulativeSize);
      details.transactionsCumulativeSize = cumulativeSize;

      uint64_t sizeMedian = 0;
      jsonGetUint(json, "sizeMedian", sizeMedian);
      details.sizeMedian = sizeMedian;

      details.penalty = 0.0;

      std::string hashHex = jsonGetHexString(json, "hash");
      if (!hashHex.empty() && hashHex.size() == 64)
        common::podFromHex(hashHex, details.hash);

      std::string prevHashHex = jsonGetHexString(json, "prevBlockHash");
      if (!prevHashHex.empty() && prevHashHex.size() == 64)
        common::podFromHex(prevHashHex, details.prevBlockHash);

      std::string txsArr = jsonExtractArray(json, "transactions");
      if (!txsArr.empty())
      {
        std::vector<std::string> txObjs = jsonSplitObjects(txsArr);
        for (const auto &txObj : txObjs)
          details.transactions.push_back(parseTransactionDetails(txObj));
      }

      return details;
    }

    cn::BlockShortEntry parseBlockShortEntry(const std::string &json)
    {
      cn::BlockShortEntry entry;

      std::string hashHex = jsonGetHexString(json, "blockHash");
      if (!hashHex.empty() && hashHex.size() == 64)
        common::podFromHex(hashHex, entry.blockHash);

      uint64_t hasBlock = 0;
      jsonGetUint(json, "hasBlock", hasBlock);
      entry.hasBlock = (hasBlock != 0);

      std::string txsArr = jsonExtractArray(json, "transactions");
      if (!txsArr.empty())
      {
        std::vector<std::string> txObjs = jsonSplitObjects(txsArr);
        for (const auto &txObj : txObjs)
        {
          cn::TransactionShortInfo shortInfo = {};

          std::string txIdHex = jsonGetHexString(txObj, "txId");
          if (!txIdHex.empty() && txIdHex.size() == 64)
            common::podFromHex(txIdHex, shortInfo.txId);

          entry.txsShortInfo.push_back(shortInfo);
        }
      }

      return entry;
    }

    bool isLoopbackHost(const std::string &host)
    {
      return host == "127.0.0.1" || host == "localhost" || host == "::1" ||
             host == "0:0:0:0:0:0:0:1";
    }

  } // anonymous namespace

  // ─── Constructor / Destructor ────────────────────────────────────────────

  NodeClient::NodeClient(const std::string &host, uint16_t port)
      : m_host(host), m_port(port)
  {
  }

  NodeClient::~NodeClient()
  {
    shutdown();
  }

  // ─── Connection ──────────────────────────────────────────────────────────

  bool NodeClient::init()
  {
    // Legacy entry point — do not block startup; use connectToDaemon() from background.
    return connectToDaemon();
  }

  bool NodeClient::connectToDaemon()
  {
    if (!refreshBlockHeight(5000, 10000))
    {
      m_connected.store(false);
      return false;
    }

    m_connected.store(true);
    return true;
  }

  std::string NodeClient::endpoint() const
  {
    return m_host + ":" + std::to_string(m_port);
  }

  bool NodeClient::shutdown()
  {
    m_connected.store(false);
    return true;
  }

  bool NodeClient::isConnected() const
  {
    return m_connected.load();
  }

  // ─── JSON-RPC helper ─────────────────────────────────────────────────────

  std::string NodeClient::jsonRpcCall(const std::string &method,
                                      const std::string &paramsJson,
                                      int connectTimeoutMs,
                                      int recvTimeoutMs)
  {
    try
    {
      std::ostringstream body;
      body << R"({"jsonrpc":"2.0","id":1,"method":")" << method
           << R"(","params":)" << paramsJson << "}";

      BoltHttp::HttpClient client(m_host, m_port);
      BoltHttp::HttpClientResponse response =
          client.post("/json_rpc", body.str(), "application/json", connectTimeoutMs, recvTimeoutMs);

      if (!response.success || response.statusCode != 200)
        return "";

      if (!extractError(response.body).empty())
        return "";

      return extractResult(response.body);
    }
    catch (...)
    {
      return "";
    }
  }

  // ─── cn::INode interface ─────────────────────────────────────────────────

  bool NodeClient::addObserver(cn::INodeObserver *) { return true; }
  bool NodeClient::removeObserver(cn::INodeObserver *) { return true; }

  void NodeClient::init(const Callback &callback)
  {
    bool ok = init();
    if (ok)
      callback(std::error_code());
    else
      callback(std::make_error_code(std::errc::connection_refused));
  }

  size_t NodeClient::getPeerCount() const { return m_peerCount; }

  bool NodeClient::refreshBlockHeight(int connectTimeoutMs, int recvTimeoutMs)
  {
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (m_connected.load() && m_lastHeightRefresh.time_since_epoch().count() != 0 &&
          now - m_lastHeightRefresh < std::chrono::seconds(5))
        return m_lastKnownBlockHeight > 0;
    }

    std::string result = jsonRpcCall("getblockcount", "{}", connectTimeoutMs, recvTimeoutMs);
    if (result.empty())
      return false;

    uint64_t count = 0;
    if (!jsonGetUint(result, "count", count))
      return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_lastLocalBlockHeight = static_cast<uint32_t>(count);
    m_lastKnownBlockHeight = m_lastLocalBlockHeight;
    m_lastHeightRefresh = now;
    return true;
  }

  uint32_t NodeClient::getLastLocalBlockHeight() const
  {
    if (m_connected.load())
      const_cast<NodeClient *>(this)->refreshBlockHeight();
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastLocalBlockHeight;
  }

  uint32_t NodeClient::getLastKnownBlockHeight() const
  {
    if (m_connected.load())
      const_cast<NodeClient *>(this)->refreshBlockHeight();
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastKnownBlockHeight;
  }

  uint32_t NodeClient::getLocalBlockCount() const { return getLastLocalBlockHeight(); }
  uint32_t NodeClient::getKnownBlockCount() const { return getLastKnownBlockHeight(); }

  uint64_t NodeClient::getLastLocalBlockTimestamp() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastLocalBlockTimestamp;
  }

  // ─── Transaction relay ───────────────────────────────────────────────────

  void NodeClient::relayTransaction(const cn::Transaction &transaction,
                                    const Callback &callback)
  {
    try
    {
      cn::COMMAND_RPC_SEND_RAW_TX::request req;
      cn::COMMAND_RPC_SEND_RAW_TX::response rsp;
      req.tx_as_hex = transactionToHex(transaction);

      BoltHttp::HttpClient client(m_host, m_port);
      const BoltHttp::HttpClientResponse response =
          client.post("/sendrawtransaction", storeToJson(req), "application/json", 5000, 30000);
      if (!response.success || response.statusCode != 200)
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }
      if (!loadFromJson(rsp, response.body))
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }
      if (rsp.status != cn::CORE_RPC_STATUS_OK)
      {
        callback(std::make_error_code(std::errc::invalid_argument));
        return;
      }
      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Random outs ─────────────────────────────────────────────────────────

  void NodeClient::getRandomOutsByAmounts(
      std::vector<uint64_t> &&amounts,
      uint64_t outsCount,
      std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &result,
      const Callback &callback)
  {
    constexpr int kConnectMs = 2000;
    constexpr int kRecvMs = 20000;

    try
    {
      cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::request req;
      cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::response rsp;
      req.amounts = amounts;
      req.outs_count = outsCount;

      std::string rpcError;

      // Local daemon: JSON /getrandom_outs first (same path most web wallets use).
      if (isLoopbackHost(m_host))
      {
        if (fetchRandomOutsJson(m_host, m_port, req.amounts, outsCount, result, rpcError, kRecvMs))
        {
          callback(std::error_code());
          return;
        }
      }

      if (binaryCommand(m_host, m_port, "/getrandom_outs.bin", req, rsp,
                        kConnectMs, kRecvMs, &rpcError))
      {
        result = std::move(rsp.outs);
        callback(std::error_code());
        return;
      }

      if (fetchRandomOutsJson(m_host, m_port, req.amounts, outsCount, result, rpcError, kRecvMs))
      {
        callback(std::error_code());
        return;
      }

      std::error_code ec;
      if (rpcError.find("Failed to connect") != std::string::npos)
        ec = std::make_error_code(std::errc::connection_refused);
      else if (rpcError.find("Core is busy") != std::string::npos)
        ec = std::make_error_code(std::errc::resource_unavailable_try_again);
      else if (rpcError.find("Daemon") != std::string::npos || rpcError.find("HTTP 5") != std::string::npos)
        ec = std::make_error_code(std::errc::protocol_error);
      else
        ec = std::make_error_code(std::errc::io_error);

      callback(ec);
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── New blocks ──────────────────────────────────────────────────────────

  void NodeClient::getNewBlocks(std::vector<crypto::Hash> &&knownBlockIds,
                                std::vector<cn::block_complete_entry> &newBlocks,
                                uint32_t &startHeight,
                                const Callback &callback)
  {
    try
    {
      std::string idsStr = "[";
      for (size_t i = 0; i < knownBlockIds.size(); ++i)
      {
        if (i > 0)
          idsStr += ",";
        idsStr += "\"" + common::podToHex(knownBlockIds[i]) + "\"";
      }
      idsStr += "]";

      std::string params = R"({"knownBlockIds":)" + idsStr + "}";

      std::string response = jsonRpcCall("getNewBlocks", params);
      if (response.empty())
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }

      newBlocks.clear();

      uint64_t sh = 0;
      jsonGetUint(response, "startHeight", sh);
      startHeight = static_cast<uint32_t>(sh);

      std::string blocksArr = jsonExtractArray(response, "blocks");
      std::vector<std::string> blockObjs = jsonSplitObjects(blocksArr);
      for (const auto &blockObj : blockObjs)
      {
        cn::block_complete_entry entry;

        std::string blockHex = jsonGetHexString(blockObj, "block");
        if (!blockHex.empty())
        {
          std::vector<uint8_t> raw = common::fromHex(blockHex);
          entry.block.assign(reinterpret_cast<const char *>(raw.data()), raw.size());
        }

        std::string txsArr = jsonExtractArray(blockObj, "txs");
        if (!txsArr.empty())
        {
          size_t pos = 0;
          while (pos < txsArr.size())
          {
            while (pos < txsArr.size() &&
                   (txsArr[pos] == ',' || txsArr[pos] == ' ' ||
                    txsArr[pos] == '\n' || txsArr[pos] == '[' || txsArr[pos] == ']'))
              ++pos;
            if (pos >= txsArr.size())
              break;
            if (txsArr[pos] == '"')
            {
              ++pos;
              size_t end = pos;
              while (end < txsArr.size() && txsArr[end] != '"')
                ++end;
              entry.txs.push_back(txsArr.substr(pos, end - pos));
              pos = end + 1;
            }
            else
            {
              size_t end = pos;
              while (end < txsArr.size() && txsArr[end] != ',' && txsArr[end] != ']')
                ++end;
              std::string val = txsArr.substr(pos, end - pos);
              if (!val.empty())
                entry.txs.push_back(val);
              pos = end;
            }
          }
        }

        newBlocks.push_back(entry);
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Transaction outs global indices ─────────────────────────────────────

  void NodeClient::getTransactionOutsGlobalIndices(
      const crypto::Hash &transactionHash,
      std::vector<uint32_t> &outsGlobalIndices,
      const Callback &callback)
  {
    try
    {
      std::string rpcError;
      if (!fetchTxGlobalOutputIndexes(m_host, m_port, transactionHash, outsGlobalIndices, rpcError))
      {
        if (rpcError.find("Failed to connect") != std::string::npos)
          callback(std::make_error_code(std::errc::connection_refused));
        else
          callback(std::make_error_code(std::errc::io_error));
        return;
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Query blocks ────────────────────────────────────────────────────────

  void NodeClient::queryBlocks(std::vector<crypto::Hash> &&knownBlockIds,
                               uint64_t timestamp,
                               std::vector<cn::BlockShortEntry> &newBlocks,
                               uint32_t &startHeight,
                               const Callback &callback)
  {
    try
    {
      std::string idsStr = "[";
      for (size_t i = 0; i < knownBlockIds.size(); ++i)
      {
        if (i > 0)
          idsStr += ",";
        idsStr += "\"" + common::podToHex(knownBlockIds[i]) + "\"";
      }
      idsStr += "]";

      std::string params = R"({"knownBlockIds":)" + idsStr +
                           R"(,"timestamp":)" + std::to_string(timestamp) + "}";

      std::string response = jsonRpcCall("queryBlocks", params);
      if (response.empty())
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }

      newBlocks.clear();

      uint64_t sh = 0;
      jsonGetUint(response, "startHeight", sh);
      startHeight = static_cast<uint32_t>(sh);

      std::string itemsArr = jsonExtractArray(response, "items");
      std::vector<std::string> itemObjs = jsonSplitObjects(itemsArr);
      for (const auto &item : itemObjs)
        newBlocks.push_back(parseBlockShortEntry(item));

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Pool symmetric difference ───────────────────────────────────────────

  void NodeClient::getPoolSymmetricDifference(
      std::vector<crypto::Hash> &&knownPoolTxIds,
      crypto::Hash knownBlockId,
      bool &isBcActual,
      std::vector<std::unique_ptr<cn::ITransactionReader>> &newTxs,
      std::vector<crypto::Hash> &deletedTxIds,
      const Callback &callback)
  {
    try
    {
      cn::COMMAND_RPC_GET_POOL_CHANGES_LITE::request req{};
      cn::COMMAND_RPC_GET_POOL_CHANGES_LITE::response rsp{};
      req.tailBlockId = knownBlockId;
      req.knownTxsIds = std::move(knownPoolTxIds);

      if (!binaryCommand(m_host, m_port, "/get_pool_changes_lite.bin", req, rsp))
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }

      isBcActual = rsp.isTailBlockActual;
      deletedTxIds = std::move(rsp.deletedTxsIds);
      newTxs.clear();
      newTxs.reserve(rsp.addedTxs.size());
      for (const auto &tpi : rsp.addedTxs)
        newTxs.push_back(createTransactionPrefix(tpi.txPrefix, tpi.txHash));

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Multisignature output ───────────────────────────────────────────────

  void NodeClient::getMultisignatureOutputByGlobalIndex(
      uint64_t, uint32_t, cn::MultisignatureOutput &, const Callback &callback)
  {
    callback(std::make_error_code(std::errc::not_supported));
  }

  // ─── Get blocks ──────────────────────────────────────────────────────────

  void NodeClient::getBlocks(const std::vector<uint32_t> &blockHeights,
                             std::vector<std::vector<cn::BlockDetails>> &blocks,
                             const Callback &callback)
  {
    getBlocksByHeight(blockHeights, blocks);
    callback(std::error_code());
  }

  void NodeClient::getBlocks(const std::vector<crypto::Hash> &blockHashes,
                             std::vector<cn::BlockDetails> &blocks,
                             const Callback &callback)
  {
    try
    {
      std::string hashesStr = "[";
      for (size_t i = 0; i < blockHashes.size(); ++i)
      {
        if (i > 0)
          hashesStr += ",";
        hashesStr += "\"" + common::podToHex(blockHashes[i]) + "\"";
      }
      hashesStr += "]";

      std::string params = R"({"blockHashes":)" + hashesStr + "}";
      std::string response = jsonRpcCall("getBlocksDetailsByHashes", params);

      blocks.clear();
      if (!response.empty())
      {
        std::string blocksArr = jsonExtractArray(response, "blocks");
        std::vector<std::string> blockObjs = jsonSplitObjects(blocksArr);
        for (const auto &obj : blockObjs)
          blocks.push_back(parseBlockDetails(obj));
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  void NodeClient::getBlocks(uint64_t timestampBegin,
                             uint64_t timestampEnd,
                             uint32_t blocksNumberLimit,
                             std::vector<cn::BlockDetails> &blocks,
                             uint32_t &blocksNumberWithinTimestamps,
                             const Callback &callback)
  {
    try
    {
      std::string params = R"({"timestampBegin":)" + std::to_string(timestampBegin) +
                           R"(,"timestampEnd":)" + std::to_string(timestampEnd) +
                           R"(,"blocksNumberLimit":)" + std::to_string(blocksNumberLimit) + "}";

      std::string response = jsonRpcCall("getBlocksByTimestamps", params);

      blocks.clear();
      blocksNumberWithinTimestamps = 0;

      if (!response.empty())
      {
        uint64_t count = 0;
        jsonGetUint(response, "blocksNumberWithinTimestamps", count);
        blocksNumberWithinTimestamps = static_cast<uint32_t>(count);

        std::string blocksArr = jsonExtractArray(response, "blocks");
        std::vector<std::string> blockObjs = jsonSplitObjects(blocksArr);
        for (const auto &obj : blockObjs)
          blocks.push_back(parseBlockDetails(obj));
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Get transactions ────────────────────────────────────────────────────

  void NodeClient::getTransactions(const std::vector<crypto::Hash> &transactionHashes,
                                   std::vector<cn::TransactionDetails> &transactions,
                                   const Callback &callback)
  {
    try
    {
      std::string hashesStr = "[";
      for (size_t i = 0; i < transactionHashes.size(); ++i)
      {
        if (i > 0)
          hashesStr += ",";
        hashesStr += "\"" + common::podToHex(transactionHashes[i]) + "\"";
      }
      hashesStr += "]";

      std::string params = R"({"transactionHashes":)" + hashesStr + "}";
      std::string response = jsonRpcCall("getTransactions", params);

      transactions.clear();
      if (!response.empty())
      {
        std::string txsArr = jsonExtractArray(response, "transactions");
        std::vector<std::string> txObjs = jsonSplitObjects(txsArr);
        for (const auto &obj : txObjs)
          transactions.push_back(parseTransactionDetails(obj));
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  void NodeClient::getTransactionsByPaymentId(
      const crypto::Hash &paymentId,
      std::vector<cn::TransactionDetails> &transactions,
      const Callback &callback)
  {
    try
    {
      std::string pidHex = common::podToHex(paymentId);
      std::string params = R"({"paymentId":")" + pidHex + R"("})";
      std::string response = jsonRpcCall("getTransactionsByPaymentId", params);

      transactions.clear();
      if (!response.empty())
      {
        std::string txsArr = jsonExtractArray(response, "transactions");
        std::vector<std::string> txObjs = jsonSplitObjects(txsArr);
        for (const auto &obj : txObjs)
          transactions.push_back(parseTransactionDetails(obj));
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  void NodeClient::getPoolTransactions(uint64_t timestampBegin,
                                       uint64_t timestampEnd,
                                       uint32_t transactionsNumberLimit,
                                       std::vector<cn::TransactionDetails> &transactions,
                                       uint64_t &transactionsNumberWithinTimestamps,
                                       const Callback &callback)
  {
    try
    {
      std::string params = R"({"timestampBegin":)" + std::to_string(timestampBegin) +
                           R"(,"timestampEnd":)" + std::to_string(timestampEnd) +
                           R"(,"transactionsNumberLimit":)" + std::to_string(transactionsNumberLimit) + "}";

      std::string response = jsonRpcCall("getPoolTransactions", params);

      transactions.clear();
      transactionsNumberWithinTimestamps = 0;

      if (!response.empty())
      {
        jsonGetUint(response, "transactionsNumberWithinTimestamps", transactionsNumberWithinTimestamps);

        std::string txsArr = jsonExtractArray(response, "transactions");
        std::vector<std::string> txObjs = jsonSplitObjects(txsArr);
        for (const auto &obj : txObjs)
          transactions.push_back(parseTransactionDetails(obj));
      }

      callback(std::error_code());
    }
    catch (...)
    {
      callback(std::make_error_code(std::errc::io_error));
    }
  }

  // ─── Is synchronized ─────────────────────────────────────────────────────

  void NodeClient::isSynchronized(bool &syncStatus, const Callback &callback)
  {
    std::string result = jsonRpcCall("getblockcount", "{}");
    if (result.empty())
    {
      syncStatus = false;
      callback(std::make_error_code(std::errc::io_error));
      return;
    }

    uint64_t count = 0;
    jsonGetUint(result, "count", count);
    syncStatus = (count > 0);
    callback(std::error_code());
  }

  // ─── Get transaction ─────────────────────────────────────────────────────

  void NodeClient::getTransaction(const crypto::Hash &transactionHash,
                                  cn::Transaction &transaction,
                                  const Callback &callback)
  {
    if (getTransactionSync(transactionHash, transaction))
      callback(std::error_code());
    else
      callback(std::make_error_code(std::errc::io_error));
  }

  std::vector<crypto::Hash> NodeClient::getPoolTransactions()
  {
    std::vector<crypto::Hash> result;

    // Same path as WalletGreen / NodeRpcProxy: /get_pool_changes_lite.bin
    cn::COMMAND_RPC_GET_POOL_CHANGES_LITE::request req{};
    cn::COMMAND_RPC_GET_POOL_CHANGES_LITE::response rsp{};
    req.tailBlockId = cn::NULL_HASH;

    if (binaryCommand(m_host, m_port, "/get_pool_changes_lite.bin", req, rsp))
    {
      result.reserve(rsp.addedTxs.size());
      for (const auto &tpi : rsp.addedTxs)
        result.push_back(tpi.txHash);
      return result;
    }

    // Fallback when binary pool RPC is unavailable.
    std::string response = jsonRpcCall("f_on_transactions_pool_json", "{}");
    if (response.empty())
      return result;

    std::string txsArr = jsonExtractArray(response, "transactions");
    std::vector<std::string> txObjs = jsonSplitObjects(txsArr);
    for (const auto &txObj : txObjs)
    {
      std::string hashHex = jsonGetHexString(txObj, "hash");
      if (!hashHex.empty() && hashHex.size() == 64)
      {
        crypto::Hash h;
        common::podFromHex(hashHex, h);
        result.push_back(h);
      }
    }

    return result;
  }

  bool NodeClient::getTransactionSync(const crypto::Hash &txHash, cn::Transaction &tx)
  {
    cn::COMMAND_RPC_GET_TRANSACTIONS::request req;
    cn::COMMAND_RPC_GET_TRANSACTIONS::response resp;
    req.txs_hashes.push_back(common::podToHex(txHash));

    if (jsonCommand(m_host, m_port, "/gettransactions", req, resp) &&
        resp.missed_tx.empty() && !resp.txs_as_hex.empty())
      return transactionFromHex(resp.txs_as_hex[0], tx);

    // /gettransactions only searches the blockchain (checkTxPool=false on daemon).
    return fetchPoolTransactionSync(m_host, m_port, txHash, tx);
  }

  bool NodeClient::getTransactionsAtHeight(uint32_t height,
                                           std::vector<cn::Transaction> &transactions,
                                           std::vector<crypto::Hash> &hashes,
                                           std::vector<std::vector<uint32_t>> *outputGlobalIndexes)
  {
    transactions.clear();
    hashes.clear();
    if (outputGlobalIndexes)
      outputGlobalIndexes->clear();

    cn::COMMAND_RPC_GET_TRANSACTIONS_WITH_OUTPUT_GLOBAL_INDEXES::request req;
    cn::COMMAND_RPC_GET_TRANSACTIONS_WITH_OUTPUT_GLOBAL_INDEXES::response resp;
    req.heights.push_back(height);
    req.include_miner_txs = false;
    req.range = false;

    if (!jsonCommand(m_host, m_port, "/get_raw_transactions_by_heights", req, resp))
      return false;

    transactions.reserve(resp.transactions.size());
    hashes.reserve(resp.transactions.size());
    for (const auto &entry : resp.transactions)
    {
      cn::Transaction tx;
      static_cast<cn::TransactionPrefix &>(tx) = entry.transaction;
      hashes.push_back(entry.hash);
      transactions.push_back(std::move(tx));
      if (outputGlobalIndexes)
        outputGlobalIndexes->push_back(entry.output_indexes);
    }

    return true;
  }

  bool NodeClient::fetchTransactionDetails(const crypto::Hash &txHash,
                                           cn::Transaction &transaction,
                                           uint32_t &blockHeight,
                                           bool &inBlock)
  {
    blockHeight = 0;
    inBlock = false;

    cn::F_COMMAND_RPC_GET_TRANSACTION_DETAILS::request req;
    cn::F_COMMAND_RPC_GET_TRANSACTION_DETAILS::response resp;
    req.hash = common::podToHex(txHash);

    const std::string params = storeToJson(req);
    const std::string result = jsonRpcCall("f_transaction_json", params);
    if (!result.empty() && loadFromJson(resp, result) && resp.status == cn::CORE_RPC_STATUS_OK)
    {
      transaction = resp.tx;
      if (!resp.block.hash.empty())
      {
        inBlock = true;
        blockHeight = resp.block.height;
      }
      return true;
    }

    if (fetchPoolTransactionSync(m_host, m_port, txHash, transaction))
      return true;

    return false;
  }

  std::string NodeClient::callDaemonMethod(const std::string &method, const std::string &paramsJson)
  {
    return jsonRpcCall(method, paramsJson);
  }

  // ─── Additional helpers ──────────────────────────────────────────────────

  void NodeClient::getBlockDetailsByHeight(uint32_t blockHeight,
                                           cn::BlockDetails &blockDetails)
  {
    std::string params = R"({"blockHeight":)" + std::to_string(blockHeight) + "}";
    std::string result = jsonRpcCall("getBlockDetailsByHeight", params);
    if (!result.empty())
      blockDetails = parseBlockDetails(result);
  }

  void NodeClient::getBlocksByHeight(
      const std::vector<uint32_t> &blockHeights,
      std::vector<std::vector<cn::BlockDetails>> &blocks)
  {
    blocks.clear();
    if (blockHeights.empty())
      return;

    std::string heightsStr = "[";
    for (size_t i = 0; i < blockHeights.size(); ++i)
    {
      if (i > 0)
        heightsStr += ",";
      heightsStr += std::to_string(blockHeights[i]);
    }
    heightsStr += "]";

    std::string params = R"({"blockHeights":)" + heightsStr + "}";
    std::string result = jsonRpcCall("getBlocksDetailsByHeights", params);

    if (result.empty())
    {
      for (auto h : blockHeights)
      {
        std::vector<cn::BlockDetails> single;
        cn::BlockDetails details;
        getBlockDetailsByHeight(h, details);
        single.push_back(details);
        blocks.push_back(single);
      }
      return;
    }

    std::string outerArr = jsonExtractArray(result, "blocks");
    std::vector<std::string> innerArrs = jsonSplitObjects(outerArr);

    if (innerArrs.empty())
    {
      std::vector<std::string> flatObjs = jsonSplitObjects(result);
      for (const auto &obj : flatObjs)
      {
        cn::BlockDetails details = parseBlockDetails(obj);
        if (details.height > 0)
        {
          std::vector<cn::BlockDetails> single;
          single.push_back(details);
          blocks.push_back(single);
        }
      }
    }
    else
    {
      for (const auto &innerArr : innerArrs)
      {
        std::vector<cn::BlockDetails> blockGroup;
        if (innerArr[0] == '[')
        {
          std::string innerContent = innerArr.substr(1, innerArr.size() - 2);
          std::vector<std::string> detailsObjs = jsonSplitObjects(innerContent);
          for (const auto &obj : detailsObjs)
            blockGroup.push_back(parseBlockDetails(obj));
        }
        else
        {
          blockGroup.push_back(parseBlockDetails(innerArr));
        }
        blocks.push_back(blockGroup);
      }
    }
  }

} // namespace NodeClient