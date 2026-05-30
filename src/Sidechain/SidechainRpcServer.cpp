// SidechainRpcServer implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainRpcServer.h"
#include "BoltAMM.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include <sstream>

namespace Sidechain
{
  // JSON-RPC error codes for sidechain-specific errors
  namespace RpcErrorCode
  {
    const int INSUFFICIENT_BALANCE = -32000;
    const int INSUFFICIENT_ESCROW = -32001;
    const int RATE_LIMITED = -32002;
    const int UNAUTHORIZED_MINT = -32003;
    const int ORDER_NOT_FOUND = -32004;
    const int ORDER_NOT_OWNER = -32005;
    const int DEX_NOT_ENABLED = -32006;
    const int FAUCET_ALREADY_CLAIMED = -32007;
    const int FAUCET_NO_HISTORY = -32008;
    const int DEPOSIT_NOT_FOUND = -32009;
    const int DEPOSIT_NOT_MATURED = -32010;
    const int INVALID_PARAMS = -32011;
    const int TRANSACTION_REJECTED = -32012;
  }

  SidechainRpcServer::SidechainRpcServer(logging::ILogger &logger,
                                         SidechainStorage &storage,
                                         SidechainValidator &validator)
      : m_logger(logger, "SidechainRPC"),
        m_storage(storage),
        m_validator(validator)
  {
  }

  void SidechainRpcServer::start(const std::string &bindIp, uint16_t bindPort, size_t threadCount)
  {
    m_httpServer.reset(new BoltHttp::Server(nullptr, threadCount));

    m_httpServer->onRequest([this](const BoltHttp::Request &req, BoltHttp::Response &resp)
                            {
        if (req.url == "/json_rpc" && req.method == "POST")
        {
            std::string body(req.body.begin(), req.body.end());
            resp.setBody(handleJsonRpc(body));
        }
        else
        {
            resp.status = 404;
            resp.setBody("Not found");
        } });

    m_httpServer->onWebSocket([this](BoltHttp::WebSocket &ws)
                              { handleWebSocketConnection(ws); });

    m_httpServer->onSse([this](BoltHttp::SseConnection &conn)
                        {
        m_logger(logging::INFO) << "SSE client connected to sidechain";
        std::ostringstream data;
        data << R"({"height":)" << m_storage.topBlockHeight()
             << R"(,"tokens":)" << m_storage.getAllTokens().size() << "}";
        conn.sendEvent("status", data.str()); });

    m_httpServer->start(bindIp, bindPort);
    m_logger(logging::INFO) << "Sidechain RPC server started on " << bindIp << ":" << bindPort
                            << " (HTTP + WebSocket + SSE)";
  }

  void SidechainRpcServer::stop()
  {
    if (m_httpServer)
      m_httpServer->stop();
    m_logger(logging::INFO) << "Sidechain RPC server stopped";
  }

  void SidechainRpcServer::handleWebSocketConnection(BoltHttp::WebSocket &ws)
  {
    m_logger(logging::INFO) << "WebSocket client connected to sidechain DEX";

    {
      std::lock_guard<std::mutex> lock(m_wsMutex);
      m_wsClients.push_back(&ws);
    }

    ws.onMessage([this, &ws](const std::string &message)
                 {
        try
        {
            common::JsonValue req = common::JsonValue::fromString(message);
            std::string type = req.contains("type") ? req("type").getString() : "";

            if (type == "subscribe_orders" && m_dexEngine)
            {
                uint64_t baseTokenId = static_cast<uint64_t>(req("baseTokenId").getInteger());
                uint64_t quoteTokenId = static_cast<uint64_t>(req("quoteTokenId").getInteger());
                auto orders = m_dexEngine->getOrders(baseTokenId, quoteTokenId);
                std::ostringstream snapshot;
                snapshot << R"({"type":"orderBookSnapshot","baseTokenId":)" << baseTokenId
                         << R"(,"quoteTokenId":)" << quoteTokenId << R"(,"orders":[)";
                for (size_t i = 0; i < orders.size(); ++i)
                {
                    if (i > 0) snapshot << ",";
                    snapshot << R"({"id":)" << orders[i].id
                             << R"(,"type":")" << (orders[i].type == BoltDex::OrderType::Buy ? "buy" : "sell") << R"(")"
                             << R"(,"amount":)" << orders[i].amount
                             << R"(,"price":)" << orders[i].price
                             << R"(,"filled":)" << orders[i].filled << "}";
                }
                snapshot << "]}";
                ws.sendText(snapshot.str());
            }
            else if (type == "subscribe_trades" && m_dexEngine)
            {
                uint64_t baseTokenId = static_cast<uint64_t>(req("baseTokenId").getInteger());
                uint64_t quoteTokenId = static_cast<uint64_t>(req("quoteTokenId").getInteger());
                auto trades = m_dexEngine->getTrades(baseTokenId, quoteTokenId, 20);
                std::ostringstream snapshot;
                snapshot << R"({"type":"tradeHistory","baseTokenId":)" << baseTokenId
                         << R"(,"quoteTokenId":)" << quoteTokenId << R"(,"trades":[)";
                for (size_t i = 0; i < trades.size(); ++i)
                {
                    if (i > 0) snapshot << ",";
                    snapshot << R"({"id":)" << trades[i].id
                             << R"(,"amount":)" << trades[i].amount
                             << R"(,"price":)" << trades[i].price
                             << R"(,"timestamp":)" << trades[i].timestamp << "}";
                }
                snapshot << "]}";
                ws.sendText(snapshot.str());
            }
        }
        catch (const std::exception &e)
        {
            m_logger(logging::WARNING) << "WebSocket message error: " << e.what();
        } });

    ws.onClose([this, &ws]()
               {
        std::lock_guard<std::mutex> lock(m_wsMutex);
        m_wsClients.erase(
            std::remove(m_wsClients.begin(), m_wsClients.end(), &ws),
            m_wsClients.end());
        m_logger(logging::INFO) << "WebSocket client disconnected from sidechain"; });
  }

  void SidechainRpcServer::pushBlockEvent(uint64_t height, uint64_t txCount, size_t votes)
  {
    std::ostringstream data;
    data << R"({"height":)" << height
         << R"(,"txCount":)" << txCount
         << R"(,"votes":)" << votes << "}";

    std::lock_guard<std::mutex> lock(m_wsMutex);
    for (auto *client : m_wsClients)
    {
      std::ostringstream msg;
      msg << R"({"type":"block","data":)" << data.str() << "}";
      client->sendText(msg.str());
    }
  }

  void SidechainRpcServer::pushDexTrade(const Sidechain::BoltDex::Trade &trade)
  {
    std::ostringstream data;
    data << R"({"id":)" << trade.id
         << R"(,"buyer":")" << common::podToHex(trade.buyer) << R"(")"
         << R"(,"seller":")" << common::podToHex(trade.seller) << R"(")"
         << R"(,"baseTokenId":)" << trade.baseTokenId
         << R"(,"quoteTokenId":)" << trade.quoteTokenId
         << R"(,"amount":)" << trade.amount
         << R"(,"price":)" << trade.price
         << R"(,"timestamp":)" << trade.timestamp << "}";

    std::lock_guard<std::mutex> lock(m_wsMutex);
    for (auto *client : m_wsClients)
    {
      std::ostringstream msg;
      msg << R"({"type":"dexTrade","data":)" << data.str() << "}";
      client->sendText(msg.str());
    }
  }

  void SidechainRpcServer::pushBridgeDeposit(uint64_t amount, const std::string &destHex, const std::string &txHash)
  {
    std::ostringstream data;
    data << R"({"amount":)" << amount
         << R"(,"destination":")" << destHex << R"(")"
         << R"(,"txHash":")" << txHash << R"("})";

    std::lock_guard<std::mutex> lock(m_wsMutex);
    for (auto *client : m_wsClients)
    {
      std::ostringstream msg;
      msg << R"({"type":"bridgeDeposit","data":)" << data.str() << "}";
      client->sendText(msg.str());
    }
  }

  namespace
  {
    std::string buildError(int code, const std::string &message, const std::string &id)
    {
      std::ostringstream err;
      err << R"({"jsonrpc":"2.0","error":{"code":)" << code
          << R"(,"message":")" << message << R"("},"id":)" << id << "}";
      return err.str();
    }
  }

  std::string SidechainRpcServer::handleJsonRpc(const std::string &body)
  {
    std::string id = "null";

    try
    {
      std::stringstream stream(body);
      common::JsonValue request;
      stream >> request;

      std::string method = request("method").getString();
      common::JsonValue params = request("params");

      const common::JsonValue &idVal = request("id");
      if (idVal.isInteger())
        id = std::to_string(idVal.getInteger());
      else if (idVal.isString())
        id = idVal.getString();

      std::stringstream paramsStream;
      paramsStream << params;
      m_logger(logging::INFO) << "Params: " << paramsStream.str();

      std::string result;
      if (method == "getBalance")
        result = methodGetBalance(params);
      else if (method == "getTokenBalance")
        result = methodGetTokenBalance(params);
      else if (method == "getTokens")
        result = methodGetTokens(params);
      else if (method == "getTokenByFingerprint")
        result = methodGetTokenByFingerprint(params);
      else if (method == "transfer")
        result = methodTransfer(params);
      else if (method == "createToken")
        result = methodCreateToken(params);
      else if (method == "mintToken")
        result = methodMintToken(params);
      else if (method == "burnToken")
        result = methodBurnToken(params);
      else if (method == "getStatus")
        result = methodGetStatus(params);
      else if (method == "getPendingTransactions")
        result = methodGetPendingTransactions(params);
      else if (method == "getValidators")
        result = methodGetValidators(params);
      else if (method == "getTransactions")
        result = methodGetTransactions(params);
      else if (method == "getAssetRegistry")
        result = methodGetAssetRegistry(params);
      else if (method == "getEquivalenceGroup")
        result = methodGetEquivalenceGroup(params);
      else if (method == "getBridgeStatus")
        result = methodGetBridgeStatus(params);
      else if (method == "bridgeUnlock")
        result = methodBridgeUnlock(params);
      else if (method == "faucet")
        result = methodFaucet(params);
      else if (method == "dex_getOrders")
        result = methodDexGetOrders(params);
      else if (method == "dex_getTrades")
        result = methodDexGetTrades(params);
      else if (method == "dex_getAllTrades")
        result = methodDexGetAllTrades(params);
      else if (method == "dex_submitOrder")
        result = methodDexSubmitOrder(params);
      else if (method == "dex_cancelOrder")
        result = methodDexCancelOrder(params);
      else if (method == "dex_deposit")
        result = methodDexDeposit(params);
      else if (method == "dex_withdraw")
        result = methodDexWithdraw(params);
      else if (method == "dex_getEscrowBalance")
        result = methodDexGetEscrowBalance(params);
      else if (method == "amm_getPools")
        result = methodAmmGetPools(params);
      else if (method == "amm_getQuote")
        result = methodAmmGetQuote(params);
      else if (method == "amm_createPool")
        result = methodAmmCreatePool(params);
      else if (method == "amm_addLiquidity")
        result = methodAmmAddLiquidity(params);
      else if (method == "amm_removeLiquidity")
        result = methodAmmRemoveLiquidity(params);
      else if (method == "amm_swap")
        result = methodAmmSwap(params);
      else if (method == "amm_getPositions")
        result = methodAmmGetPositions(params);
      else
        return buildError(-32601, "Method not found", id);

      return R"({"jsonrpc":"2.0","result":)" + result + R"(,"id":)" + id + "}";
    }
    catch (const std::exception &e)
    {
      return buildError(-32603, e.what(), id);
    }
  }

  // Account methods
  std::string SidechainRpcServer::methodGetBalance(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);
    uint64_t balance = 0;
    m_storage.getBalance(pubKey, 0, balance);
    return std::to_string(balance);
  }

  std::string SidechainRpcServer::methodGetTokenBalance(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    uint64_t tokenId = static_cast<uint64_t>(params("tokenId").getInteger());
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);
    uint64_t balance = 0;
    m_storage.getBalance(pubKey, tokenId, balance);
    return std::to_string(balance);
  }

  std::string SidechainRpcServer::methodGetTokens(const common::JsonValue &)
  {
    auto tokens = m_storage.getAllTokens();
    std::string result = "[";
    for (size_t i = 0; i < tokens.size(); ++i)
    {
      result += R"({"id":)" + std::to_string(tokens[i].id) +
                R"(,"fingerprint":")" + tokens[i].fingerprint + R"(")" +
                R"(,"name":")" + tokens[i].name +
                R"(","symbol":")" + tokens[i].symbol +
                R"(","totalSupply":)" + std::to_string(tokens[i].totalSupply) +
                R"(,"maxSupply":)" + std::to_string(tokens[i].maxSupply) +
                R"(,"decimals":)" + std::to_string(tokens[i].decimals) +
                R"(,"backingModel":)" + std::to_string(static_cast<uint8_t>(tokens[i].backingModel)) +
                R"(,"backingRatio":)" + std::to_string(tokens[i].backingRatio) +
                R"(,"lockedCCXAmount":)" + std::to_string(tokens[i].lockedCCXAmount);

      AssetRegistryEntry assetEntry;
      if (m_storage.getAssetByTokenId(tokens[i].id, assetEntry))
      {
        result += R"(,"sourceChain":")" + assetEntry.sourceChain + R"(")";
        result += R"(,"sourceAsset":")" + assetEntry.sourceAsset + R"(")";
        result += R"(,"bridgeOperator":")" + common::podToHex(assetEntry.bridgeOperator) + R"(")";
        result += R"(,"equivalenceClass":")" + assetEntry.equivalenceClass + R"(")";
        result += R"(,"verified":)" + std::string(assetEntry.verified ? "true" : "false");
      }

      result += "}";
      if (i < tokens.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodGetTokenByFingerprint(const common::JsonValue &params)
  {
    std::string fingerprint = params("fingerprint").getString();
    TokenInfo token;
    if (m_storage.getTokenByFingerprint(fingerprint, token))
    {
      return R"({"id":)" + std::to_string(token.id) +
             R"(,"fingerprint":")" + token.fingerprint + R"(")" +
             R"(,"name":")" + token.name +
             R"(","symbol":")" + token.symbol +
             R"(","totalSupply":)" + std::to_string(token.totalSupply) +
             R"(,"decimals":)" + std::to_string(token.decimals) + "}";
    }
    throw std::runtime_error("Token not found");
  }

  bool SidechainRpcServer::checkBalance(const crypto::PublicKey &sender, uint64_t tokenId, uint64_t amount, uint64_t fee)
  {
    uint64_t balance = 0;
    m_storage.getBalance(sender, tokenId, balance);
    return balance >= (amount + fee);
  }

  std::string SidechainRpcServer::methodTransfer(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::Transfer;
    common::podFromHex(params("from").getString(), tx.from);
    common::podFromHex(params("to").getString(), tx.to);
    tx.amount = static_cast<uint64_t>(params("amount").getInteger());
    tx.tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;

    if (!checkBalance(tx.from, tx.tokenId, tx.amount, tx.fee))
      throw std::runtime_error("Insufficient balance");

    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
    tx.extra.clear();

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Transaction rejected by validator");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) +
           R"(","fee":)" + std::to_string(tx.fee) + "}";
  }

  std::string SidechainRpcServer::methodCreateToken(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::CreateToken;

    const common::JsonValue &fromVal = params("from");
    const common::JsonValue &nameHexVal = params("nameHex");
    const common::JsonValue &symbolHexVal = params("symbolHex");
    const common::JsonValue &supplyVal = params("initialSupply");
    const common::JsonValue &modelVal = params("backingModel");

    std::string fromStr = fromVal.getString();
    common::podFromHex(fromStr, tx.from);
    tx.to = tx.from;
    tx.amount = static_cast<uint64_t>(supplyVal.getInteger());

    tx.fee = 0;
    tx.feeTokenId = 0;
    tx.tokenId = 0;

    uint8_t backingModel = static_cast<uint8_t>(static_cast<uint64_t>(modelVal.getInteger()));

    uint8_t decimals = 6;
    if (params.contains("decimals"))
      decimals = static_cast<uint8_t>(static_cast<uint64_t>(params("decimals").getInteger()));

    uint64_t backingRatio = 0;
    uint64_t lockedCCXAmount = 0;

    if (backingModel == static_cast<uint8_t>(TokenBackingModel::Backed) ||
        backingModel == static_cast<uint8_t>(TokenBackingModel::Hybrid))
    {
      if (params.contains("backingRatio"))
        backingRatio = static_cast<uint64_t>(params("backingRatio").getInteger());
      else
        backingRatio = SidechainConfig::DEFAULT_BACKING_RATIO;

      if (params.contains("lockedCCXAmount"))
        lockedCCXAmount = static_cast<uint64_t>(params("lockedCCXAmount").getInteger());
    }

    std::string nameHex = nameHexVal.getString();
    std::string symbolHex = symbolHexVal.getString();

    cn::BinaryArray nameBytes = common::fromHex(nameHex);
    cn::BinaryArray symbolBytes = common::fromHex(symbolHex);

    std::string name(nameBytes.begin(), nameBytes.end());
    std::string symbol(symbolBytes.begin(), symbolBytes.end());

    std::string combined = name + ":" + symbol + ":" + std::to_string(backingModel) +
                           ":" + std::to_string(decimals) +
                           ":" + std::to_string(backingRatio) +
                           ":" + std::to_string(lockedCCXAmount);
    tx.extra.assign(combined.begin(), combined.end());

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Token creation rejected by validator");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) +
           R"(","symbol":")" + symbol + R"(")";
  }

  std::string SidechainRpcServer::methodMintToken(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::Mint;
    common::podFromHex(params("from").getString(), tx.from);
    common::podFromHex(params("to").getString(), tx.to);
    tx.amount = static_cast<uint64_t>(params("amount").getInteger());
    tx.tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    // Verify the sender is authorized as a bridge operator for this token
    AssetRegistryEntry assetEntry;
    if (!m_storage.getAssetByTokenId(tx.tokenId, assetEntry) ||
        assetEntry.bridgeOperator != tx.from)
      throw std::runtime_error("Sender is not the bridge operator for this token");

    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;

    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));

    if (params.contains("mainChainTxHash"))
    {
      std::string txHash = params("mainChainTxHash").getString();
      tx.extra.assign(txHash.begin(), txHash.end());
    }

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Mint transaction rejected by validator");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) +
           R"(","mintedAmount":)" + std::to_string(tx.amount) +
           R"(,"tokenId":)" + std::to_string(tx.tokenId) +
           R"(,"recipient":")" + common::podToHex(tx.to) + R"(")";
  }

  std::string SidechainRpcServer::methodBurnToken(const common::JsonValue &params)
  {
    Transaction tx;
    tx.type = TransactionType::Burn;
    common::podFromHex(params("from").getString(), tx.from);
    tx.to = tx.from;
    tx.amount = static_cast<uint64_t>(params("amount").getInteger());
    tx.tokenId = static_cast<uint64_t>(params("tokenId").getInteger());

    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;

    if (!checkBalance(tx.from, tx.tokenId, tx.amount, tx.fee))
      throw std::runtime_error("Insufficient balance to burn");

    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
    tx.extra.clear();

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Burn transaction rejected by validator");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) +
           R"(","burnedAmount":)" + std::to_string(tx.amount) +
           R"(,"tokenId":)" + std::to_string(tx.tokenId) + "}";
  }

  std::string SidechainRpcServer::methodGetStatus(const common::JsonValue &)
  {
    uint64_t height = m_storage.topBlockHeight();
    uint64_t tokenCount = m_storage.getAllTokens().size();
    uint64_t pendingCount = m_validator.getPendingTransactions().size();
    return R"({"height":)" + std::to_string(height) +
           R"(,"tokenCount":)" + std::to_string(tokenCount) +
           R"(,"pendingTransactions":)" + std::to_string(pendingCount) + "}";
  }

  std::string SidechainRpcServer::methodGetPendingTransactions(const common::JsonValue &)
  {
    auto pending = m_validator.getPendingTransactions();
    return std::to_string(pending.size());
  }

  std::string SidechainRpcServer::methodGetValidators(const common::JsonValue &)
  {
    auto validators = m_validator.getValidators();
    std::string result = "[";
    for (size_t i = 0; i < validators.size(); ++i)
    {
      result += R"({"id":)" + std::to_string(validators[i].id) +
                R"(,"host":")" + validators[i].host +
                R"(","port":)" + std::to_string(validators[i].port) +
                R"(,"stake":)" + std::to_string(validators[i].stake) +
                R"(,"active":)" + (validators[i].active ? "true" : "false") + "}";
      if (i < validators.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodGetTransactions(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);

    uint64_t topHeight = m_storage.topBlockHeight();
    std::string result = "[";

    bool first = true;
    for (uint64_t h = 1; h <= topHeight; ++h)
    {
      Block block;
      if (!m_storage.getBlock(h, block))
        continue;

      for (const auto &tx : block.transactions)
      {
        if (tx.from == pubKey || tx.to == pubKey)
        {
          if (!first)
            result += ",";
          first = false;

          std::string typeStr;
          switch (tx.type)
          {
          case TransactionType::Transfer:
            typeStr = "Transfer";
            break;
          case TransactionType::CreateToken:
            typeStr = "CreateToken";
            break;
          case TransactionType::Mint:
            typeStr = "Mint";
            break;
          case TransactionType::Burn:
            typeStr = "Burn";
            break;
          default:
            typeStr = "Unknown";
            break;
          }

          result += "{";
          result += R"("txHash":")" + common::podToHex(tx.txHash) + R"(")";
          result += R"(,"type":")" + typeStr + R"(")";
          result += R"(,"from":")" + common::podToHex(tx.from) + R"(")";
          result += R"(,"to":")" + common::podToHex(tx.to) + R"(")";
          result += R"(,"amount":)" + std::to_string(tx.amount);
          result += R"(,"tokenId":)" + std::to_string(tx.tokenId);
          result += R"(,"fee":)" + std::to_string(tx.fee);
          result += R"(,"blockHeight":)" + std::to_string(h);
          result += R"(,"timestamp":)" + std::to_string(tx.timestamp);
          result += "}";
        }
      }
    }

    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodGetAssetRegistry(const common::JsonValue &)
  {
    auto assets = m_storage.getAllAssets();
    std::string result = "[";
    for (size_t i = 0; i < assets.size(); ++i)
    {
      result += "{";
      result += R"("tokenId":)" + std::to_string(assets[i].tokenId) + ",";
      result += R"("fingerprint":")" + assets[i].fingerprint + "\",";
      result += R"("sourceChain":")" + assets[i].sourceChain + "\",";
      result += R"("sourceAsset":")" + assets[i].sourceAsset + "\",";
      result += R"("bridgeOperator":")" + common::podToHex(assets[i].bridgeOperator) + "\",";
      result += R"("equivalenceClass":")" + assets[i].equivalenceClass + "\",";
      result += R"("verified":)" + std::string(assets[i].verified ? "true" : "false");

      TokenInfo token;
      if (m_storage.getToken(assets[i].tokenId, token))
      {
        result += R"(,"name":")" + token.name + R"(")";
        result += R"(,"symbol":")" + token.symbol + R"(")";
        result += R"(,"totalSupply":)" + std::to_string(token.totalSupply);
        result += R"(,"lockedCCXAmount":)" + std::to_string(token.lockedCCXAmount);
        result += R"(,"backingModel":)" + std::to_string(static_cast<uint8_t>(token.backingModel));
        result += R"(,"backingRatio":)" + std::to_string(token.backingRatio);
        result += R"(,"decimals":)" + std::to_string(token.decimals);
      }

      result += "}";
      if (i < assets.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodGetEquivalenceGroup(const common::JsonValue &params)
  {
    std::string equivalenceClass = params("equivalenceClass").getString();
    auto tokenIds = m_storage.getTokensByEquivalenceClass(equivalenceClass);

    std::string result = "[";
    for (size_t i = 0; i < tokenIds.size(); ++i)
    {
      TokenInfo token;
      if (m_storage.getToken(tokenIds[i], token))
      {
        result += "{";
        result += R"("tokenId":)" + std::to_string(token.id) + ",";
        result += R"("fingerprint":")" + token.fingerprint + "\",";
        result += R"("symbol":")" + token.symbol + "\",";
        result += R"("name":")" + token.name + "\"";
        result += "}";
        if (i < tokenIds.size() - 1)
          result += ",";
      }
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodGetBridgeStatus(const common::JsonValue &)
  {
    auto assets = m_storage.getAllAssets();
    auto pendingUnlocks = m_storage.getPendingUnlocks();

    std::string result = "{";
    result += R"("bridgeAssets":)";

    std::string assetsArray = "[";
    for (size_t i = 0; i < assets.size(); ++i)
    {
      assetsArray += "{";
      assetsArray += R"("tokenId":)" + std::to_string(assets[i].tokenId) + ",";
      assetsArray += R"("sourceChain":")" + assets[i].sourceChain + "\",";
      assetsArray += R"("sourceAsset":")" + assets[i].sourceAsset + "\",";
      assetsArray += R"("bridgeOperator":")" + common::podToHex(assets[i].bridgeOperator) + "\",";
      assetsArray += R"("totalLocked":)" + std::to_string(m_storage.getTotalLockedForToken(assets[i].tokenId));
      assetsArray += "}";
      if (i < assets.size() - 1)
        assetsArray += ",";
    }
    assetsArray += "]";
    result += assetsArray + ",";

    result += R"("pendingUnlocks":)" + std::to_string(pendingUnlocks.size()) + ",";

    std::string unlocksArray = "[";
    size_t unlockCount = 0;
    for (size_t i = 0; i < pendingUnlocks.size() && unlockCount < 20; ++i)
    {
      if (unlockCount > 0)
        unlocksArray += ",";
      unlocksArray += "{";
      unlocksArray += R"("lockId":)" + std::to_string(pendingUnlocks[i].id) + ",";
      unlocksArray += R"("userAddress":")" + common::podToHex(pendingUnlocks[i].userAddress) + "\",";
      unlocksArray += R"("tokenId":)" + std::to_string(pendingUnlocks[i].tokenId) + ",";
      unlocksArray += R"("amount":)" + std::to_string(pendingUnlocks[i].amount);
      unlocksArray += "}";
      ++unlockCount;
    }
    unlocksArray += "]";
    result += R"("pendingUnlockDetails":)" + unlocksArray;

    result += "}";
    return result;
  }

  std::string SidechainRpcServer::methodBridgeUnlock(const common::JsonValue &params)
  {
    uint64_t lockId = static_cast<uint64_t>(params("lockId").getInteger());
    std::string userAddress = params("userAddress").getString();
    crypto::PublicKey userPubKey;
    common::podFromHex(userAddress, userPubKey);

    // Verify the lock exists and belongs to this user
    BridgeLockEntry lockEntry;
    if (!m_storage.getBridgeLock(lockId, lockEntry))
      throw std::runtime_error("Bridge lock not found");

    if (lockEntry.userAddress != userPubKey)
      throw std::runtime_error("Bridge lock does not belong to this address");

    if (lockEntry.unlocked)
      throw std::runtime_error("Bridge lock already unlocked");

    // Create a burn transaction for the locked token amount
    Transaction tx;
    tx.type = TransactionType::Burn;
    tx.from = userPubKey;
    tx.to = userPubKey;
    tx.amount = lockEntry.amount;
    tx.tokenId = lockEntry.tokenId;
    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;
    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));
    tx.extra.clear();

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Bridge unlock transaction rejected by validator");

    // applyTransaction (called during block commit) will handle:
    // - Burning the tokens
    // - Calling markBridgeLockUnlocked(lockId) for matching locks
    // - Reducing lockedCCXAmount on the token

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) +
           R"(","lockId":)" + std::to_string(lockId) +
           R"(,"burnedAmount":)" + std::to_string(lockEntry.amount) +
           R"(,"tokenId":)" + std::to_string(lockEntry.tokenId) + "}";
  }

  std::string SidechainRpcServer::methodFaucet(const common::JsonValue &params)
  {
    std::string address = params("address").getString();
    crypto::PublicKey pubKey;
    common::podFromHex(address, pubKey);

    std::string claimKey = "faucet_" + common::podToHex(pubKey);
    std::vector<uint8_t> claimed;
    if (m_storage.getMeta(claimKey, claimed) && !claimed.empty())
      throw std::runtime_error("Faucet already claimed by this address");

    uint64_t topHeight = m_storage.topBlockHeight();
    bool hasHistory = false;
    for (uint64_t h = 1; h <= topHeight && !hasHistory; ++h)
    {
      Block block;
      if (!m_storage.getBlock(h, block))
        continue;
      for (const auto &tx : block.transactions)
      {
        if (tx.to == pubKey)
        {
          hasHistory = true;
          break;
        }
      }
    }

    if (!hasHistory)
      throw std::runtime_error("Address has no transaction history");

    uint64_t faucetAmount = SidechainConfig::FAUCET_AMOUNT;
    uint64_t currentBalance = 0;
    m_storage.getBalance(pubKey, 0, currentBalance);
    m_storage.setBalance(pubKey, 0, currentBalance + faucetAmount);
    m_storage.putMeta(claimKey, {1});

    return R"({"status":"ok","amount":)" + std::to_string(faucetAmount) +
           R"(,"balance":)" + std::to_string(currentBalance + faucetAmount) + "}";
  }

  std::string SidechainRpcServer::methodGetSidechainStatus(const common::JsonValue &)
  {
    return R"({"sidechainEnabled":true,"sidechainHost":")" + m_sidechainHost +
           R"(","sidechainPort":)" + std::to_string(m_sidechainPort) + "}";
  }

  std::string SidechainRpcServer::methodGetSidechainTokens(const common::JsonValue &)
  {
    return methodGetTokens(common::JsonValue(common::JsonValue::OBJECT));
  }

  std::string SidechainRpcServer::methodSidechainTransfer(const common::JsonValue &params)
  {
    return methodTransfer(params);
  }

  std::string SidechainRpcServer::methodSidechainCreateToken(const common::JsonValue &params)
  {
    return methodCreateToken(params);
  }

  // DEX methods
  std::string SidechainRpcServer::methodDexGetOrders(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");
    uint64_t baseTokenId = static_cast<uint64_t>(params("baseTokenId").getInteger());
    uint64_t quoteTokenId = static_cast<uint64_t>(params("quoteTokenId").getInteger());
    auto orders = m_dexEngine->getOrders(baseTokenId, quoteTokenId);
    std::string result = "[";
    for (size_t i = 0; i < orders.size(); ++i)
    {
      result += "{";
      result += R"("id":)" + std::to_string(orders[i].id) + ",";
      result += R"("type":")" + std::string(orders[i].type == BoltDex::OrderType::Buy ? "buy" : "sell") + "\",";
      result += R"("owner":")" + common::podToHex(orders[i].owner) + "\",";
      result += R"("amount":)" + std::to_string(orders[i].amount) + ",";
      result += R"("price":)" + std::to_string(orders[i].price) + ",";
      result += R"("filled":)" + std::to_string(orders[i].filled) + ",";
      std::string statusStr = "open";
      if (orders[i].status == BoltDex::OrderStatus::Filled)
        statusStr = "filled";
      else if (orders[i].status == BoltDex::OrderStatus::Cancelled)
        statusStr = "cancelled";
      result += R"("status":")" + statusStr + "\"}";
      if (i < orders.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodDexGetTrades(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");
    uint64_t baseTokenId = static_cast<uint64_t>(params("baseTokenId").getInteger());
    uint64_t quoteTokenId = static_cast<uint64_t>(params("quoteTokenId").getInteger());
    size_t limit = params.contains("limit") ? static_cast<size_t>(params("limit").getInteger()) : 50;
    auto trades = m_dexEngine->getTrades(baseTokenId, quoteTokenId, limit);
    std::string result = "[";
    for (size_t i = 0; i < trades.size(); ++i)
    {
      result += "{";
      result += R"("id":)" + std::to_string(trades[i].id) + ",";
      result += R"("buyer":")" + common::podToHex(trades[i].buyer) + "\",";
      result += R"("seller":")" + common::podToHex(trades[i].seller) + "\",";
      result += R"("amount":)" + std::to_string(trades[i].amount) + ",";
      result += R"("price":)" + std::to_string(trades[i].price) + ",";
      result += R"("settled":)" + std::string(trades[i].settled ? "true" : "false") + ",";
      result += R"("timestamp":)" + std::to_string(trades[i].timestamp) + "}";
      if (i < trades.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodDexGetAllTrades(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");
    size_t limit = params.contains("limit") ? static_cast<size_t>(params("limit").getInteger()) : 100;
    auto trades = m_dexEngine->getAllTrades(limit);
    std::string result = "[";
    for (size_t i = 0; i < trades.size(); ++i)
    {
      result += "{";
      result += R"("id":)" + std::to_string(trades[i].id) + ",";
      result += R"("buyer":")" + common::podToHex(trades[i].buyer) + "\",";
      result += R"("seller":")" + common::podToHex(trades[i].seller) + "\",";
      result += R"("baseTokenId":)" + std::to_string(trades[i].baseTokenId) + ",";
      result += R"("quoteTokenId":)" + std::to_string(trades[i].quoteTokenId) + ",";
      result += R"("amount":)" + std::to_string(trades[i].amount) + ",";
      result += R"("price":)" + std::to_string(trades[i].price) + ",";
      result += R"("settled":)" + std::string(trades[i].settled ? "true" : "false") + ",";
      result += R"("timestamp":)" + std::to_string(trades[i].timestamp) + "}";
      if (i < trades.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodDexSubmitOrder(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");

    BoltDex::Order order;
    order.type = params("type").getString() == "buy" ? BoltDex::OrderType::Buy : BoltDex::OrderType::Sell;
    common::podFromHex(params("owner").getString(), order.owner);
    order.baseTokenId = static_cast<uint64_t>(params("baseTokenId").getInteger());
    order.quoteTokenId = static_cast<uint64_t>(params("quoteTokenId").getInteger());
    order.amount = static_cast<uint64_t>(params("amount").getInteger());
    order.price = static_cast<uint64_t>(params("price").getInteger());

    if (order.amount == 0)
      throw std::runtime_error("Order amount must be greater than zero");
    if (order.price == 0)
      throw std::runtime_error("Order price must be greater than zero");

    // Read the next ID before submission — submitOrder increments it internally
    uint64_t nextId = m_dexEngine->getNextOrderId();

    if (!m_dexEngine->submitOrder(order))
      throw std::runtime_error("Insufficient escrow balance");

    // The order that was just created has ID = (nextId before increment)
    uint64_t assignedId = nextId;

    return R"({"success":true,"orderId":)" + std::to_string(assignedId) +
           R"(,"type":")" + std::string(order.type == BoltDex::OrderType::Buy ? "buy" : "sell") + R"(")"
                                                                                                  R"(,"baseTokenId":)" +
           std::to_string(order.baseTokenId) +
           R"(,"quoteTokenId":)" + std::to_string(order.quoteTokenId) +
           R"(,"amount":)" + std::to_string(order.amount) +
           R"(,"price":)" + std::to_string(order.price) + "}";
  }

  std::string SidechainRpcServer::methodDexCancelOrder(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");

    uint64_t orderId = static_cast<uint64_t>(params("orderId").getInteger());
    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);

    if (!m_dexEngine->cancelOrder(orderId, owner))
      throw std::runtime_error("Order not found or not owned by sender");

    return R"({"success":true,"orderId":)" + std::to_string(orderId) + "}";
  }

  std::string SidechainRpcServer::methodDexDeposit(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");
    crypto::PublicKey dexPub = m_dexEngine->getDexPublicKey();
    return R"({"dexAddress":")" + common::podToHex(dexPub) + R"("})";
  }

  std::string SidechainRpcServer::methodDexWithdraw(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");

    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);
    uint64_t tokenId = static_cast<uint64_t>(params("tokenId").getInteger());
    uint64_t amount = static_cast<uint64_t>(params("amount").getInteger());

    if (amount == 0)
      throw std::runtime_error("Withdrawal amount must be greater than zero");

    if (!m_dexEngine->withdraw(owner, tokenId, amount))
      throw std::runtime_error("Insufficient escrow balance");

    return R"({"success":true,"owner":")" + common::podToHex(owner) +
           R"(","tokenId":)" + std::to_string(tokenId) +
           R"(,"amount":)" + std::to_string(amount) + "}";
  }

  std::string SidechainRpcServer::methodDexGetEscrowBalance(const common::JsonValue &params)
  {
    if (!m_dexEngine)
      throw std::runtime_error("DEX engine not enabled");
    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);
    uint64_t tokenId = static_cast<uint64_t>(params("tokenId").getInteger());
    return std::to_string(m_dexEngine->getEscrowBalance(owner, tokenId));
  }

  std::string SidechainRpcServer::methodAmmGetPools(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    auto pools = m_ammEngine->getPools();
    std::string result = "[";
    for (size_t i = 0; i < pools.size(); ++i)
    {
      result += "{";
      result += R"("poolId":)" + std::to_string(pools[i].poolId) + ",";
      result += R"("tokenIdA":)" + std::to_string(pools[i].tokenIdA) + ",";
      result += R"("tokenIdB":)" + std::to_string(pools[i].tokenIdB) + ",";
      result += R"("reserveA":)" + std::to_string(pools[i].reserveA) + ",";
      result += R"("reserveB":)" + std::to_string(pools[i].reserveB) + ",";
      result += R"("totalLiquidity":)" + std::to_string(pools[i].totalLiquidity) + ",";
      result += R"("feeBasisPoints":)" + std::to_string(pools[i].feeBasisPoints);
      result += "}";
      if (i < pools.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }

  std::string SidechainRpcServer::methodAmmGetQuote(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    uint64_t poolId = static_cast<uint64_t>(params("poolId").getInteger());
    uint64_t tokenIdIn = static_cast<uint64_t>(params("tokenIdIn").getInteger());
    uint64_t amountIn = static_cast<uint64_t>(params("amountIn").getInteger());

    uint64_t amountOut = m_ammEngine->getAmountOut(poolId, tokenIdIn, amountIn);

    return R"({"poolId":)" + std::to_string(poolId) +
           R"(,"tokenIdIn":)" + std::to_string(tokenIdIn) +
           R"(,"amountIn":)" + std::to_string(amountIn) +
           R"(,"amountOut":)" + std::to_string(amountOut) + "}";
  }

  std::string SidechainRpcServer::methodAmmCreatePool(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    uint64_t tokenIdA = static_cast<uint64_t>(params("tokenIdA").getInteger());
    uint64_t tokenIdB = static_cast<uint64_t>(params("tokenIdB").getInteger());
    uint64_t amountA = static_cast<uint64_t>(params("amountA").getInteger());
    uint64_t amountB = static_cast<uint64_t>(params("amountB").getInteger());
    crypto::PublicKey creator;
    common::podFromHex(params("creator").getString(), creator);
    uint16_t feeBps = params.contains("feeBasisPoints")
                          ? static_cast<uint16_t>(params("feeBasisPoints").getInteger())
                          : 30;

    Transaction tx;
    tx.type = TransactionType::AmmCreatePool;
    tx.from = creator;
    tx.to = creator;
    tx.amount = amountA;
    tx.tokenId = tokenIdA;
    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;
    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));

    // Extra: tokenIdB:amountB:feeBasisPoints
    std::string extra = std::to_string(tokenIdB) + ":" + std::to_string(amountB) + ":" + std::to_string(feeBps);
    tx.extra.assign(extra.begin(), extra.end());

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Transaction rejected");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) + R"("})";
  }

  std::string SidechainRpcServer::methodAmmAddLiquidity(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    uint64_t poolId = static_cast<uint64_t>(params("poolId").getInteger());
    uint64_t amountA = static_cast<uint64_t>(params("amountA").getInteger());
    uint64_t amountB = static_cast<uint64_t>(params("amountB").getInteger());
    crypto::PublicKey provider;
    common::podFromHex(params("provider").getString(), provider);

    Transaction tx;
    tx.type = TransactionType::AmmAddLiquidity;
    tx.from = provider;
    tx.to = provider;
    tx.amount = amountA;
    tx.tokenId = 0; // Token A is identified by the pool
    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;
    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));

    // Extra: poolId:amountB
    std::string extra = std::to_string(poolId) + ":" + std::to_string(amountB);
    tx.extra.assign(extra.begin(), extra.end());

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Transaction rejected");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) + R"("})";
  }

  std::string SidechainRpcServer::methodAmmRemoveLiquidity(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    uint64_t positionId = static_cast<uint64_t>(params("positionId").getInteger());
    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);

    Transaction tx;
    tx.type = TransactionType::AmmRemoveLiquidity;
    tx.from = owner;
    tx.to = owner;
    tx.amount = 0;
    tx.tokenId = 0;
    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;
    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));

    std::string extra = std::to_string(positionId);
    tx.extra.assign(extra.begin(), extra.end());

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Transaction rejected");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) + R"("})";
  }

  std::string SidechainRpcServer::methodAmmSwap(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    uint64_t poolId = static_cast<uint64_t>(params("poolId").getInteger());
    uint64_t tokenIdIn = static_cast<uint64_t>(params("tokenIdIn").getInteger());
    uint64_t amountIn = static_cast<uint64_t>(params("amountIn").getInteger());
    uint64_t minAmountOut = static_cast<uint64_t>(params("minAmountOut").getInteger());
    crypto::PublicKey from;
    common::podFromHex(params("from").getString(), from);

    Transaction tx;
    tx.type = TransactionType::AmmSwap;
    tx.from = from;
    tx.to = from;
    tx.amount = amountIn;
    tx.tokenId = tokenIdIn;
    tx.fee = m_testnet ? SidechainConfig::TESTNET_FEE : SidechainConfig::DEFAULT_FEE;
    tx.feeTokenId = 0;
    tx.signature = crypto::Signature{};
    tx.timestamp = static_cast<uint64_t>(std::time(nullptr));

    // Extra: poolId:minAmountOut
    std::string extra = std::to_string(poolId) + ":" + std::to_string(minAmountOut);
    tx.extra.assign(extra.begin(), extra.end());

    cn::BinaryArray txBytes = cn::toBinaryArray(tx);
    crypto::cn_fast_hash(txBytes.data(), txBytes.size(), tx.txHash);

    if (!m_validator.submitTransaction(tx))
      throw std::runtime_error("Transaction rejected");

    return R"({"success":true,"txHash":")" + common::podToHex(tx.txHash) + R"("})";
  }

  std::string SidechainRpcServer::methodAmmGetPositions(const common::JsonValue &params)
  {
    if (!m_ammEngine)
      throw std::runtime_error("AMM engine not enabled");

    crypto::PublicKey owner;
    common::podFromHex(params("owner").getString(), owner);

    auto positions = m_ammEngine->getPositions(owner);
    std::string result = "[";
    for (size_t i = 0; i < positions.size(); ++i)
    {
      result += "{";
      result += R"("positionId":)" + std::to_string(positions[i].positionId) + ",";
      result += R"("poolId":)" + std::to_string(positions[i].poolId) + ",";
      result += R"("liquidity":)" + std::to_string(positions[i].liquidity);
      result += "}";
      if (i < positions.size() - 1)
        result += ",";
    }
    result += "]";
    return result;
  }
}