// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "BoltRpcServer.h"
#include "WalletManager.h"
#include "Common/StringTools.h"
#include "Common/JsonValue.h"
#include "CryptoNoteCore/Currency.h"
#include "BoltHttp/BoltHttpClient.h"

#include <cstring>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#define closesocket close
#endif

namespace BoltRPC
{

  // ─── Helper: Simple HTTP response builder ──────────────────────────────────

  namespace
  {

    std::string buildHttpResponse(int statusCode,
                                  const std::string &contentType,
                                  const std::string &body,
                                  const std::string &corsDomain)
    {
      std::ostringstream response;
      response << "HTTP/1.1 " << statusCode << " "
               << (statusCode == 200 ? "OK" : "Error") << "\r\n";
      response << "Content-Type: " << contentType << "\r\n";
      response << "Content-Length: " << body.size() << "\r\n";
      response << "Connection: close\r\n";
      if (!corsDomain.empty())
      {
        response << "Access-Control-Allow-Origin: " << corsDomain << "\r\n";
        response << "Access-Control-Allow-Headers: Origin, X-Requested-With, Content-Type, Accept\r\n";
        response << "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n";
      }
      response << "\r\n";
      response << body;
      return response.str();
    }

    std::string extractJsonBody(const std::string &httpRequest)
    {
      size_t bodyStart = httpRequest.find("\r\n\r\n");
      size_t headerEnd = 4;
      if (bodyStart == std::string::npos)
      {
        bodyStart = httpRequest.find("\n\n");
        headerEnd = 2;
      }
      if (bodyStart == std::string::npos)
        return "";

      return httpRequest.substr(bodyStart + headerEnd);
    }

    common::JsonValue makeJsonRpcErrorEnvelope(int code, const std::string &message,
                                               const common::JsonValue &id = common::JsonValue())
    {
      common::JsonValue response(common::JsonValue::OBJECT);
      response.insert("jsonrpc", std::string("2.0"));
      if (!id.isNil())
        response.insert("id", id);

      common::JsonValue errObj(common::JsonValue::OBJECT);
      errObj.insert("code", static_cast<common::JsonValue::Integer>(code));
      errObj.insert("message", message);
      response.insert("error", errObj);
      return response;
    }

    bool isRpcErrorEnvelope(const common::JsonValue &v)
    {
      return v.isObject() && v.contains("error") && v("error").isObject();
    }

    common::JsonValue normalizeParams(const common::JsonValue &request)
    {
      common::JsonValue params(common::JsonValue::OBJECT);
      if (!request.isObject() || !request.contains("params"))
        return params;
      const common::JsonValue &p = request("params");
      if (p.isObject())
        return p;
      return params;
    }

  } // anonymous namespace

  // ─── Constructor / Destructor ──────────────────────────────────────────────

  BoltRpcServer::BoltRpcServer(uint16_t port,
                               cn::INode &node,
                               const cn::Currency &currency,
                               const std::string &dataDir,
                               const std::string &daemonHost,
                               uint16_t daemonPort)
      : m_port(port),
        m_node(node),
        m_currency(currency),
        m_dataDir(dataDir),
        m_daemonHost(daemonHost),
        m_daemonPort(daemonPort),
        m_walletManager(new WalletManager(node, currency, dataDir, daemonHost, daemonPort))
  {
    m_walletManager->setDaemonRpcCallback(
        [this](const std::string &method, const std::string &paramsJson) -> std::string
        {
          std::string body = R"({"jsonrpc":"2.0","id":1,"method":")" + method + R"(","params":)" + paramsJson + "}";
          BoltHttp::HttpClient client(m_daemonHost, m_daemonPort);
          const int timeoutSec = (method == "get_wallet_snapshot") ? 600 : 60;
          BoltHttp::HttpClientResponse response = client.post("/json_rpc", body, timeoutSec);
          if (!response.body.empty())
            return response.body;
          if (!response.error.empty())
          {
            return R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":")" +
                   response.error + R"(("}})";
          }
          return R"({"jsonrpc":"2.0","id":1,"error":{"code":-1,"message":"empty HTTP response from daemon"}})";
        });

    registerMethods();
  }

  BoltRpcServer::~BoltRpcServer()
  {
    stop();
  }

  // ─── Lifecycle ─────────────────────────────────────────────────────────────

  bool BoltRpcServer::start()
  {
    if (m_running.exchange(true))
      return true;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
      m_running.store(false);
      return false;
    }
#endif

    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
      m_running.store(false);
      return false;
    }

    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_serverSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
      closesocket(m_serverSocket);
      m_serverSocket = -1;
      m_running.store(false);
      return false;
    }

    if (listen(m_serverSocket, 5) < 0)
    {
      closesocket(m_serverSocket);
      m_serverSocket = -1;
      m_running.store(false);
      return false;
    }

    m_httpThread = std::thread(&BoltRpcServer::httpListenLoop, this);
    return true;
  }

  void BoltRpcServer::stop()
  {
    m_running.store(false);
    if (m_serverSocket >= 0)
    {
      closesocket(m_serverSocket);
      m_serverSocket = -1;
    }
    if (m_httpThread.joinable())
      m_httpThread.join();
#ifdef _WIN32
    WSACleanup();
#endif
  }

  bool BoltRpcServer::isRunning() const
  {
    return m_running.load();
  }

  void BoltRpcServer::setCorsDomain(const std::string &domain)
  {
    m_corsDomain = domain;
  }

  // ─── HTTP Listen Loop ─────────────────────────────────────────────────────

  void BoltRpcServer::httpListenLoop()
  {
    while (m_running.load())
    {
#ifndef _WIN32
      pollfd pfd{};
      pfd.fd = m_serverSocket;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, 500) <= 0)
        continue;
#endif

      sockaddr_in clientAddr;
      socklen_t clientLen = sizeof(clientAddr);
      int clientSocket = accept(m_serverSocket,
                                reinterpret_cast<sockaddr *>(&clientAddr),
                                &clientLen);

      if (clientSocket < 0)
        continue;

#ifndef _WIN32
      timeval rcvTimeout{};
      rcvTimeout.tv_sec = 5;
      rcvTimeout.tv_usec = 0;
      setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &rcvTimeout, sizeof(rcvTimeout));
#endif

      char buffer[65536];
      std::memset(buffer, 0, sizeof(buffer));
      ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

      std::string response;
      if (bytesRead > 0)
      {
        std::string request(buffer, bytesRead);

        if (request.find("OPTIONS") == 0)
        {
          response = buildHttpResponse(200, "text/plain", "", m_corsDomain);
        }
        else if (request.find("POST /json_rpc") != std::string::npos)
        {
          std::string body = extractJsonBody(request);
          std::string responseBody;
          handleJsonRpc(body, responseBody);
          response = buildHttpResponse(200, "application/json",
                                       responseBody, m_corsDomain);
        }
        else if (request.find("POST /") != std::string::npos ||
                 request.find("GET /") != std::string::npos)
        {
          std::string body = extractJsonBody(request);
          std::string responseBody;
          handleRequest(body, responseBody);
          response = buildHttpResponse(200, "application/json",
                                       responseBody, m_corsDomain);
        }
        else
        {
          response = buildHttpResponse(404, "text/plain",
                                       "Not Found", m_corsDomain);
        }
      }

      send(clientSocket, response.data(), response.size(), 0);
      closesocket(clientSocket);
    }
  }

  // ─── Request Handling ─────────────────────────────────────────────────────

  void BoltRpcServer::handleJsonRpc(const std::string &requestBody,
                                    std::string &responseBody)
  {
    try
    {
      common::JsonValue request;
      try
      {
        request = common::JsonValue::fromString(requestBody);
      }
      catch (...)
      {
        responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_PARSE_ERROR, "Parse error").toString();
        return;
      }

      if (!request.isObject())
      {
        responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_INVALID_REQUEST, "Invalid Request").toString();
        return;
      }

      if (!request.contains("method") || !request("method").isString())
      {
        responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_INVALID_REQUEST, "Missing or invalid method").toString();
        return;
      }

      std::string method = request("method").getString();
      common::JsonValue params = normalizeParams(request);
      common::JsonValue id;
      if (request.contains("id"))
        id = request("id");

      common::JsonValue baseResponse(common::JsonValue::OBJECT);
      baseResponse.insert("jsonrpc", std::string("2.0"));
      if (!id.isNil())
        baseResponse.insert("id", id);

      auto handler = findMethod(method);
      if (!handler)
      {
        responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_METHOD_NOT_FOUND,
                                                "Method not found: " + method, id)
                           .toString();
        return;
      }

      common::JsonValue result = handler(params);

      if (!result.isObject())
      {
        responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_INTERNAL_ERROR,
                                                "Invalid handler result", id)
                           .toString();
        return;
      }

      if (isRpcErrorEnvelope(result))
        baseResponse.insert("error", result("error"));
      else
        baseResponse.insert("result", result);

      responseBody = baseResponse.toString();
    }
    catch (const std::exception &e)
    {
      responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_INTERNAL_ERROR,
                                              std::string("Internal error: ") + e.what())
                           .toString();
    }
    catch (...)
    {
      responseBody = makeJsonRpcErrorEnvelope(JSON_RPC_INTERNAL_ERROR, "Internal error").toString();
    }
  }

  void BoltRpcServer::handleRequest(const std::string &requestBody,
                                    std::string &responseBody)
  {
    if (requestBody.find("\"method\"") != std::string::npos)
    {
      handleJsonRpc(requestBody, responseBody);
      return;
    }

    responseBody = makeJsonRpcErrorEnvelope(-32600, "Invalid Request").toString();
  }

  // ─── Method Registration ──────────────────────────────────────────────────

  void BoltRpcServer::registerMethods()
  {
    m_methods["getStatus"] = [this](const common::JsonValue &p)
    { return rpc_getStatus(p); };
    m_methods["getBalance"] = [this](const common::JsonValue &p)
    { return rpc_getBalance(p); };
    m_methods["getAddress"] = [this](const common::JsonValue &p)
    { return rpc_getAddress(p); };
    m_methods["getOutputs"] = [this](const common::JsonValue &p)
    { return rpc_getOutputs(p); };
    m_methods["getTransactions"] = [this](const common::JsonValue &p)
    { return rpc_getTransactions(p); };
    m_methods["createWallet"] = [this](const common::JsonValue &p)
    { return rpc_createWallet(p); };
    m_methods["importWallet"] = [this](const common::JsonValue &p)
    { return rpc_importWallet(p); };
    m_methods["unlockWallet"] = [this](const common::JsonValue &p)
    { return rpc_unlockWallet(p); };
    m_methods["lockWallet"] = [this](const common::JsonValue &p)
    { return rpc_lockWallet(p); };
    m_methods["sendTransaction"] = [this](const common::JsonValue &p)
    { return rpc_sendTransaction(p); };
    m_methods["sendDeposit"] = [this](const common::JsonValue &p)
    { return rpc_sendDeposit(p); };
    m_methods["sendWithdrawal"] = [this](const common::JsonValue &p)
    { return rpc_sendWithdrawal(p); };
    m_methods["syncNow"] = [this](const common::JsonValue &p)
    { return rpc_syncNow(p); };
    m_methods["exportKeys"] = [this](const common::JsonValue &p)
    { return rpc_exportKeys(p); };
    m_methods["exportState"] = [this](const common::JsonValue &p)
    { return rpc_exportState(p); };
  }

  RpcMethod BoltRpcServer::findMethod(const std::string &name) const
  {
    auto it = m_methods.find(name);
    if (it != m_methods.end())
      return it->second;
    return nullptr;
  }

  // ─── RPC Method Implementations ───────────────────────────────────────────

  common::JsonValue BoltRpcServer::rpc_getStatus(const common::JsonValue &params)
  {
    WalletStatus status = m_walletManager->getStatus();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("locked", common::JsonValue::Integer(status.locked ? 1 : 0));
    result.insert("synced", common::JsonValue::Integer(status.synced ? 1 : 0));
    result.insert("blockHeight", common::JsonValue::Integer(status.blockHeight));
    result.insert("balance", common::JsonValue::Integer(status.balance));
    result.insert("unlockedBalance", common::JsonValue::Integer(status.unlockedBalance));
    result.insert("outputCount", common::JsonValue::Integer(status.outputCount));
    result.insert("transactionCount", common::JsonValue::Integer(status.transactionCount));

    if (status.syncProgress.phase != SyncProgress::IDLE || m_walletManager->isSyncRunning())
    {
      common::JsonValue sync(common::JsonValue::OBJECT);
      sync.insert("phase", common::JsonValue::Integer(static_cast<int>(status.syncProgress.phase)));
      sync.insert("totalKeys", common::JsonValue::Integer(status.syncProgress.totalKeys));
      sync.insert("processedKeys", common::JsonValue::Integer(status.syncProgress.processedKeys));
      sync.insert("totalOutputs", common::JsonValue::Integer(status.syncProgress.totalOutputs));
      sync.insert("processedOutputs", common::JsonValue::Integer(status.syncProgress.processedOutputs));
      sync.insert("ownedOutputs", common::JsonValue::Integer(status.syncProgress.ownedOutputs));
      sync.insert("currentHeight", common::JsonValue::Integer(status.syncProgress.currentHeight));
      if (!status.syncProgress.errorMessage.empty())
        sync.insert("errorMessage", status.syncProgress.errorMessage);
      result.insert("syncProgress", sync);
    }

    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_getBalance(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("balance", common::JsonValue::Integer(m_walletManager->getBalance()));
    result.insert("unlockedBalance", common::JsonValue::Integer(m_walletManager->getUnlockedBalance()));
    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_getAddress(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", m_walletManager->getAddress());
    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_getOutputs(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    bool unspentOnly = true;
    if (params.contains("unspentOnly"))
      unspentOnly = (params("unspentOnly").getInteger() != 0);

    auto outputs = m_walletManager->getOutputs(unspentOnly);

    common::JsonValue result(common::JsonValue::OBJECT);
    common::JsonValue arr(common::JsonValue::ARRAY);
    for (const auto &out : outputs)
    {
      common::JsonValue entry(common::JsonValue::OBJECT);
      entry.insert("blockHeight", common::JsonValue::Integer(out.blockHeight));
      entry.insert("txHash", common::podToHex(out.txHash));
      entry.insert("amount", common::JsonValue::Integer(out.amount));
      entry.insert("outputIndex", common::JsonValue::Integer(out.outputIndex));
      entry.insert("outputKey", common::podToHex(out.outputKey));
      entry.insert("txPublicKey", common::podToHex(out.txPublicKey));
      entry.insert("spent", common::JsonValue::Integer(out.spent ? 1 : 0));
      entry.insert("isDeposit", common::JsonValue::Integer(out.isDeposit ? 1 : 0));
      entry.insert("term", common::JsonValue::Integer(out.term));
      arr.pushBack(entry);
    }
    result.insert("outputs", arr);
    result.insert("count", common::JsonValue::Integer(static_cast<int64_t>(outputs.size())));

    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_getTransactions(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    uint32_t offset = 0;
    uint32_t limit = 50;

    if (params.contains("offset"))
      offset = static_cast<uint32_t>(params("offset").getInteger());
    if (params.contains("limit"))
      limit = static_cast<uint32_t>(params("limit").getInteger());

    auto txs = m_walletManager->getTransactions(offset, limit);

    common::JsonValue result(common::JsonValue::OBJECT);
    common::JsonValue arr(common::JsonValue::ARRAY);
    for (const auto &tx : txs)
    {
      common::JsonValue entry(common::JsonValue::OBJECT);
      entry.insert("txHash", common::podToHex(tx.txHash));
      entry.insert("blockHeight", common::JsonValue::Integer(tx.blockHeight));
      entry.insert("timestamp", common::JsonValue::Integer(tx.timestamp));
      entry.insert("fee", common::JsonValue::Integer(tx.fee));
      entry.insert("totalSent", common::JsonValue::Integer(tx.totalSent));
      entry.insert("totalReceived", common::JsonValue::Integer(tx.totalReceived));
      entry.insert("type", common::JsonValue::Integer(static_cast<int>(tx.type)));
      entry.insert("confirmed", common::JsonValue::Integer(tx.confirmed ? 1 : 0));
      entry.insert("paymentId", tx.paymentId);
      arr.pushBack(entry);
    }
    result.insert("transactions", arr);

    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_createWallet(const common::JsonValue &params)
  {
    if (!params.contains("password"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter");

    std::string password = params("password").getString();
    if (password.size() < 8)
      return makeError(JSON_RPC_INVALID_PARAMS, "Password must be at least 8 characters");

    if (m_walletManager->hasExistingWallet())
      return makeError(WALLET_ALREADY_EXISTS, "Wallet already exists");

    if (!m_walletManager->generateNewWallet(password))
      return makeError(JSON_RPC_INTERNAL_ERROR, "Failed to create wallet");

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", m_walletManager->getAddress());
    result.insert("message", std::string("Wallet created successfully"));

    m_walletManager->startSync([this](const WalletStatus &status) {});

    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_importWallet(const common::JsonValue &params)
  {
    if (!params.contains("password"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter");
    if (!params.contains("viewKey"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'viewKey' parameter");
    if (!params.contains("spendKey"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'spendKey' parameter");

    std::string password = params("password").getString();
    std::string viewKey = params("viewKey").getString();
    std::string spendKey = params("spendKey").getString();

    if (m_walletManager->hasExistingWallet())
      return makeError(WALLET_ALREADY_EXISTS, "Wallet already exists");

    if (!m_walletManager->importFromKeys(viewKey, spendKey, password))
      return makeError(JSON_RPC_INTERNAL_ERROR, "Failed to import wallet");

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", m_walletManager->getAddress());
    result.insert("message", std::string("Wallet imported successfully"));

    m_walletManager->startSync([this](const WalletStatus &status) {});

    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_unlockWallet(const common::JsonValue &params)
  {
    if (!params.contains("password"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter");

    if (!m_walletManager->hasExistingWallet())
      return makeError(WALLET_NOT_FOUND, "No wallet found. Create or import first.");

    std::string password = params("password").getString();
    if (!m_walletManager->unlock(password))
      return makeError(INVALID_PASSWORD, "Invalid password");

    m_walletManager->startSync([this](const WalletStatus &status) {});

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", m_walletManager->getAddress());
    result.insert("balance", common::JsonValue::Integer(m_walletManager->getBalance()));
    result.insert("message", std::string("Wallet unlocked"));
    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_lockWallet(const common::JsonValue &params)
  {
    m_walletManager->lock();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("message", std::string("Wallet locked"));
    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_sendTransaction(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    if (!params.contains("address"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'address'");
    if (!params.contains("amount"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'amount'");

    TransferRequest req;
    req.address = params("address").getString();
    req.amount = static_cast<uint64_t>(params("amount").getInteger());

    if (params.contains("paymentId"))
      req.paymentId = params("paymentId").getString();
    if (params.contains("mixin"))
      req.mixin = static_cast<uint64_t>(params("mixin").getInteger());
    if (params.contains("fee"))
      req.fee = static_cast<uint64_t>(params("fee").getInteger());

    TransferResult result = m_walletManager->sendTransfer(req);

    if (!result.success)
      return makeError(JSON_RPC_INTERNAL_ERROR, result.errorMessage);

    common::JsonValue res(common::JsonValue::OBJECT);
    res.insert("txHash", result.txHash);
    res.insert("fee", common::JsonValue::Integer(result.fee));
    return makeResult(res);
  }

  common::JsonValue BoltRpcServer::rpc_sendDeposit(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    if (!params.contains("amount"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'amount'");
    if (!params.contains("term"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'term'");

    DepositRequest req;
    req.amount = static_cast<uint64_t>(params("amount").getInteger());
    req.term = static_cast<uint32_t>(params("term").getInteger());

    if (params.contains("fee"))
      req.fee = static_cast<uint64_t>(params("fee").getInteger());

    TransferResult result = m_walletManager->sendDeposit(req);

    if (!result.success)
      return makeError(JSON_RPC_INTERNAL_ERROR, result.errorMessage);

    common::JsonValue res(common::JsonValue::OBJECT);
    res.insert("txHash", result.txHash);
    res.insert("fee", common::JsonValue::Integer(result.fee));
    return makeResult(res);
  }

  common::JsonValue BoltRpcServer::rpc_sendWithdrawal(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    if (!params.contains("amount"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'amount'");
    if (!params.contains("depositId"))
      return makeError(JSON_RPC_INVALID_PARAMS, "Missing 'depositId'");

    WithdrawalRequest req;
    req.amount = static_cast<uint64_t>(params("amount").getInteger());
    req.depositId = static_cast<uint64_t>(params("depositId").getInteger());

    if (params.contains("fee"))
      req.fee = static_cast<uint64_t>(params("fee").getInteger());

    TransferResult result = m_walletManager->sendWithdrawal(req);

    if (!result.success)
      return makeError(JSON_RPC_INTERNAL_ERROR, result.errorMessage);

    common::JsonValue res(common::JsonValue::OBJECT);
    res.insert("txHash", result.txHash);
    res.insert("fee", common::JsonValue::Integer(result.fee));
    return makeResult(res);
  }

  common::JsonValue BoltRpcServer::rpc_syncNow(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    m_walletManager->syncNow();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("message", std::string("Sync triggered"));
    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_exportKeys(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    std::string keys = m_walletManager->exportKeys();
    if (keys.empty())
      return makeError(JSON_RPC_INTERNAL_ERROR, "Failed to export keys");

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("keys", keys);
    result.insert("warning", std::string("Keep these keys secure. Anyone with these keys can access your funds."));
    return makeResult(result);
  }

  common::JsonValue BoltRpcServer::rpc_exportState(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
      return makeError(WALLET_LOCKED, "Wallet is locked");

    std::string path = m_walletManager->exportState();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("path", path);
    result.insert("message", std::string("State file path. Copy this file to backup or migrate your wallet."));
    return makeResult(result);
  }

  // ─── Helpers ──────────────────────────────────────────────────────────────

  common::JsonValue BoltRpcServer::makeResult(const common::JsonValue &data) const
  {
    return data;
  }

  common::JsonValue BoltRpcServer::makeError(int code, const std::string &message) const
  {
    common::JsonValue error(common::JsonValue::OBJECT);
    common::JsonValue errObj(common::JsonValue::OBJECT);
    errObj.insert("code", static_cast<common::JsonValue::Integer>(code));
    errObj.insert("message", message);
    error.insert("error", errObj);
    return error;
  }

} // namespace BoltRPC