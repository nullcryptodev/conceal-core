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
    std::string result = jsonRpcCall("getblockcount", "{}");
    if (result.empty())
    {
      m_connected.store(false);
      return false;
    }

    uint64_t count = 0;
    jsonGetUint(result, "count", count);
    m_lastLocalBlockHeight = static_cast<uint32_t>(count);
    m_lastKnownBlockHeight = m_lastLocalBlockHeight;
    m_lastLocalBlockTimestamp = 0;
    m_peerCount = 0;
    m_connected.store(true);
    return true;
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
                                      const std::string &paramsJson)
  {
    try
    {
      std::ostringstream body;
      body << R"({"jsonrpc":"2.0","id":1,"method":")" << method
           << R"(","params":)" << paramsJson << "}";

      BoltHttp::HttpClient client(m_host, m_port);
      BoltHttp::HttpClientResponse response = client.post("/json_rpc", body.str());

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

  uint32_t NodeClient::getLastLocalBlockHeight() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastLocalBlockHeight;
  }

  uint32_t NodeClient::getLastKnownBlockHeight() const
  {
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
      std::string txHex = transactionToHex(transaction);
      std::string params = R"({"tx":")" + txHex + R"("})";
      std::string result = jsonRpcCall("relayTransaction", params);

      if (result.empty())
        callback(std::make_error_code(std::errc::io_error));
      else
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
    try
    {
      std::string amountsStr = "[";
      for (size_t i = 0; i < amounts.size(); ++i)
      {
        if (i > 0)
          amountsStr += ",";
        amountsStr += std::to_string(amounts[i]);
      }
      amountsStr += "]";

      std::string params = R"({"amounts":)" + amountsStr +
                           R"(,"outsCount":)" + std::to_string(outsCount) + "}";

      std::string response = jsonRpcCall("getRandomOutsByAmounts", params);
      if (response.empty())
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }

      result.clear();

      std::string outsArr = jsonExtractArray(response, "outs");
      if (outsArr.empty())
      {
        callback(std::error_code());
        return;
      }

      std::vector<std::string> amountObjs = jsonSplitObjects(outsArr);
      for (const auto &obj : amountObjs)
      {
        cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount ofa;

        uint64_t amt = 0;
        jsonGetUint(obj, "amount", amt);
        ofa.amount = amt;

        std::string outArr = jsonExtractArray(obj, "outs");
        std::vector<std::string> outObjs = jsonSplitObjects(outArr);
        for (const auto &outObj : outObjs)
        {
          cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::out_entry entry;

          uint64_t idx = 0;
          jsonGetUint(outObj, "global_index", idx);
          entry.global_amount_index = idx;

          std::string keyHex = jsonGetHexString(outObj, "public_key");
          if (!keyHex.empty() && keyHex.size() == 64)
            common::podFromHex(keyHex, entry.out_key);

          ofa.outs.push_back(entry);
        }

        result.push_back(ofa);
      }

      callback(std::error_code());
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
      std::string hashHex = common::podToHex(transactionHash);
      std::string params = R"({"transactionHash":")" + hashHex + R"("})";
      std::string response = jsonRpcCall("getTransactionOutsGlobalIndices", params);

      if (response.empty())
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }

      outsGlobalIndices.clear();
      std::string indicesArr = jsonExtractArray(response, "outsGlobalIndices");
      if (!indicesArr.empty())
      {
        size_t pos = 0;
        while (pos < indicesArr.size())
        {
          while (pos < indicesArr.size() && (indicesArr[pos] == ',' || indicesArr[pos] == ' ' || indicesArr[pos] == '\n'))
            ++pos;
          if (pos >= indicesArr.size())
            break;
          char *end = nullptr;
          unsigned long val = std::strtoul(indicesArr.c_str() + pos, &end, 10);
          if (end == indicesArr.c_str() + pos)
            break;
          outsGlobalIndices.push_back(static_cast<uint32_t>(val));
          pos = end - indicesArr.c_str();
        }
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
      std::string idsStr = "[";
      for (size_t i = 0; i < knownPoolTxIds.size(); ++i)
      {
        if (i > 0)
          idsStr += ",";
        idsStr += "\"" + common::podToHex(knownPoolTxIds[i]) + "\"";
      }
      idsStr += "]";

      std::string params = R"({"knownPoolTxIds":)" + idsStr +
                           R"(,"knownBlockId":")" + common::podToHex(knownBlockId) + R"("})";

      std::string response = jsonRpcCall("getPoolSymmetricDifference", params);
      if (response.empty())
      {
        callback(std::make_error_code(std::errc::io_error));
        return;
      }

      uint64_t bcActual = 0;
      jsonGetUint(response, "isBcActual", bcActual);
      isBcActual = (bcActual != 0);

      newTxs.clear();
      deletedTxIds.clear();

      std::string deletedArr = jsonExtractArray(response, "deletedTxIds");
      std::vector<std::string> deletedStrs = jsonSplitObjects(deletedArr);
      for (const auto &s : deletedStrs)
      {
        crypto::Hash h;
        std::string hex = jsonGetString(s, "hash");
        if (!hex.empty() && hex.size() == 64)
        {
          common::podFromHex(hex, h);
          deletedTxIds.push_back(h);
        }
      }

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
    std::string response = jsonRpcCall("getPoolTransactions", "{}");
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
    std::string hashHex = common::podToHex(txHash);
    std::string params = R"({"hash":")" + hashHex + R"("})";
    std::string result = jsonRpcCall("getTransaction", params);
    if (result.empty())
      return false;

    std::string txHex = jsonGetHexString(result, "tx");
    if (txHex.empty())
      txHex = jsonGetHexString(result, "transaction");
    if (txHex.empty())
      return false;

    return transactionFromHex(txHex, tx);
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