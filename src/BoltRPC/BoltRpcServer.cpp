// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include "BoltRpcServer.h"
#include "WalletManager.h"
#include "MultiWalletManager.h"
#include "SyncManager.h"
#include "Common/StringTools.h"
#include "Common/JsonValue.h"
#include "CryptoNoteCore/Currency.h"
#include "BoltHttp/BoltHttpClient.h"

#include <cstring>
#include <sstream>
#include <thread>
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#define closesocket close
#endif

namespace BoltRPC
{
  // Helper: Simple HTTP response builder
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
      if (bodyStart != std::string::npos)
      {
        return httpRequest.substr(bodyStart + 4);
      }

      bodyStart = httpRequest.find("\n\n");
      if (bodyStart != std::string::npos)
      {
        return httpRequest.substr(bodyStart + 2);
      }

      return "";
    }

    // Build a JSON-RPC error response string directly
    std::string buildJsonRpcError(int code, const std::string &message, const std::string &idJson)
    {
      std::ostringstream ss;
      ss << R"({"jsonrpc":"2.0","id":)" << idJson
         << R"(,"error":{"code":)" << code
         << R"(,"message":")" << message << R"("}})";
      return ss.str();
    }

  } // anonymous namespace

  // Constructor — Single Wallet
  BoltRpcServer::BoltRpcServer(uint16_t port,
                               cn::INode &node,
                               const cn::Currency &currency,
                               const std::string &dataDir,
                               const std::string &daemonHost,
                               uint16_t daemonPort)
      : m_port(port),
        m_node(&node),
        m_currency(currency),
        m_dataDir(dataDir),
        m_daemonHost(daemonHost),
        m_daemonPort(daemonPort),
        m_multiWalletMode(false),
        m_walletManager(new WalletManager(node, currency, dataDir, daemonHost, daemonPort)),
        m_multiWalletManager(nullptr)
  {
    m_walletManager->setDaemonRpcCallback(
        [this](const std::string &method, const std::string &paramsJson) -> std::string
        {
          std::string body = R"({"jsonrpc":"2.0","id":1,"method":")" + method + R"(","params":)" + paramsJson + "}";
          BoltHttp::HttpClient client(m_daemonHost, m_daemonPort);
          BoltHttp::HttpClientResponse response = client.post("/json_rpc", body);
          return response.body;
        });

    registerMethods();
  }

  // Constructor — Multi Wallet
  BoltRpcServer::BoltRpcServer(uint16_t port,
                               MultiWalletManager &multiWallet,
                               const cn::Currency &currency,
                               const std::string &dataDir,
                               const std::string &daemonHost,
                               uint16_t daemonPort)
      : m_port(port),
        m_node(nullptr),
        m_currency(currency),
        m_dataDir(dataDir),
        m_daemonHost(daemonHost),
        m_daemonPort(daemonPort),
        m_multiWalletMode(true),
        m_walletManager(nullptr),
        m_multiWalletManager(&multiWallet)
  {
    registerMultiMethods();
  }

  BoltRpcServer::~BoltRpcServer()
  {
    stop();
  }

  // Lifecycle
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

    std::thread(&BoltRpcServer::httpListenLoop, this).detach();
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

  // HTTP Listen Loop
  void BoltRpcServer::httpListenLoop()
  {
    while (m_running.load())
    {
      sockaddr_in clientAddr;
      socklen_t clientLen = sizeof(clientAddr);
      int clientSocket = accept(m_serverSocket,
                                reinterpret_cast<sockaddr *>(&clientAddr),
                                &clientLen);

      if (clientSocket < 0)
      {
        if (m_running.load())
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }

      char buffer[65536];
      std::memset(buffer, 0, sizeof(buffer));
      ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);

      std::string response;
      if (bytesRead > 0)
      {
        std::string request(buffer, bytesRead);

        try
        {
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
        catch (const std::exception &e)
        {
          std::string errBody = buildJsonRpcError(JSON_RPC_INTERNAL_ERROR,
                                                  std::string("Internal error: ") + e.what(), "null");
          response = buildHttpResponse(200, "application/json", errBody, m_corsDomain);
        }
        catch (...)
        {
          std::string errBody = buildJsonRpcError(JSON_RPC_INTERNAL_ERROR,
                                                  "Unknown internal error", "null");
          response = buildHttpResponse(200, "application/json", errBody, m_corsDomain);
        }
      }
      else
      {
        response = buildHttpResponse(400, "text/plain", "Bad Request", m_corsDomain);
      }

      send(clientSocket, response.data(), response.size(), 0);
      closesocket(clientSocket);
    }
  }

  // Request Handling
  void BoltRpcServer::handleJsonRpc(const std::string &requestBody,
                                    std::string &responseBody)
  {
    std::string idStr = "null";

    try
    {
      if (requestBody.empty())
      {
        responseBody = buildJsonRpcError(JSON_RPC_PARSE_ERROR, "Empty request body", "null");
        return;
      }

      common::JsonValue request;
      try
      {
        request = common::JsonValue::fromString(requestBody);
      }
      catch (...)
      {
        responseBody = buildJsonRpcError(JSON_RPC_PARSE_ERROR, "Parse error", "null");
        return;
      }

      if (!request.isObject())
      {
        responseBody = buildJsonRpcError(JSON_RPC_PARSE_ERROR, "Request must be a JSON object", "null");
        return;
      }

      try
      {
        if (request.contains("id"))
        {
          common::JsonValue idVal = request("id");
          idStr = idVal.toString();
        }
      }
      catch (...)
      {
      }

      std::string method;
      try
      {
        method = request("method").getString();
      }
      catch (...)
      {
        responseBody = buildJsonRpcError(JSON_RPC_PARSE_ERROR, "Missing method", idStr);
        return;
      }

      common::JsonValue params;
      try
      {
        if (request.contains("params"))
          params = request("params");
      }
      catch (...)
      {
      }

      auto handler = findMethod(method);
      if (!handler)
      {
        responseBody = buildJsonRpcError(JSON_RPC_METHOD_NOT_FOUND,
                                         "Method not found: " + method, idStr);
        return;
      }

      common::JsonValue result;
      try
      {
        result = handler(params);
      }
      catch (const std::exception &e)
      {
        responseBody = buildJsonRpcError(JSON_RPC_INTERNAL_ERROR,
                                         std::string("Handler error: ") + e.what(), idStr);
        return;
      }
      catch (...)
      {
        responseBody = buildJsonRpcError(JSON_RPC_INTERNAL_ERROR,
                                         "Unknown handler error", idStr);
        return;
      }

      std::ostringstream ss;
      ss << R"({"jsonrpc":"2.0","id":)" << idStr << R"(,"result":)" << result.toString() << "}";
      responseBody = ss.str();
    }
    catch (const std::exception &e)
    {
      responseBody = buildJsonRpcError(JSON_RPC_INTERNAL_ERROR,
                                       std::string("Internal error: ") + e.what(), idStr);
    }
    catch (...)
    {
      responseBody = buildJsonRpcError(JSON_RPC_INTERNAL_ERROR,
                                       "Unknown internal error", idStr);
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

    responseBody = buildJsonRpcError(-32600, "Invalid Request", "null");
  }

  // Method Registration — Single Wallet
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
    m_methods["listWallets"] = [this](const common::JsonValue &p)
    { return rpc_listWallets(p); };
    m_methods["setActiveWallet"] = [this](const common::JsonValue &p)
    { return rpc_setActiveWallet(p); };
  }

  // Method Registration — Multi Wallet
  void BoltRpcServer::registerMultiMethods()
  {
    // Query methods (require active wallet)
    m_methods["getStatus"] = [this](const common::JsonValue &p)
    { return rpc_multi_getStatus(p); };
    m_methods["getBalance"] = [this](const common::JsonValue &p)
    { return rpc_multi_getBalance(p); };
    m_methods["getAddress"] = [this](const common::JsonValue &p)
    { return rpc_multi_getAddress(p); };
    m_methods["getOutputs"] = [this](const common::JsonValue &p)
    { return rpc_multi_getOutputs(p); };
    m_methods["getTransactions"] = [this](const common::JsonValue &p)
    { return rpc_multi_getTransactions(p); };

    // Wallet lifecycle
    m_methods["createWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_createWallet(p); };
    m_methods["importWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_importWallet(p); };
    m_methods["loadWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_loadWallet(p); };
    m_methods["unloadWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_unloadWallet(p); };
    m_methods["deleteWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_deleteWallet(p); };
    m_methods["listWallets"] = [this](const common::JsonValue &p)
    { return rpc_multi_listWallets(p); };

    // Transactions
    m_methods["sendTransaction"] = [this](const common::JsonValue &p)
    { return rpc_multi_sendTransaction(p); };
    m_methods["sendDeposit"] = [this](const common::JsonValue &p)
    { return rpc_multi_sendDeposit(p); };
    m_methods["sendWithdrawal"] = [this](const common::JsonValue &p)
    { return rpc_multi_sendWithdrawal(p); };

    // Sync / Export / Admin
    m_methods["syncNow"] = [this](const common::JsonValue &p)
    { return rpc_multi_syncNow(p); };
    m_methods["exportKeys"] = [this](const common::JsonValue &p)
    { return rpc_multi_exportKeys(p); };
    m_methods["exportState"] = [this](const common::JsonValue &p)
    { return rpc_multi_exportState(p); };
    m_methods["changePassword"] = [this](const common::JsonValue &p)
    { return rpc_multi_changePassword(p); };
    m_methods["importFromStateFile"] = [this](const common::JsonValue &p)
    { return rpc_multi_importFromStateFile(p); };

    // Legacy single-wallet aliases
    m_methods["unlockWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_loadWallet(p); };
    m_methods["lockWallet"] = [this](const common::JsonValue &p)
    { return rpc_multi_unloadWallet(p); };
  }

  RpcMethod BoltRpcServer::findMethod(const std::string &name) const
  {
    auto it = m_methods.find(name);
    if (it != m_methods.end())
      return it->second;
    return nullptr;
  }

  // Wallet ID Extraction Helper
  std::string BoltRpcServer::extractWalletId(const common::JsonValue &params) const
  {
    if (params.isObject() && params.contains("wallet_id"))
      return params("wallet_id").getString();

    if (m_multiWalletManager && m_multiWalletManager->hasActiveWallet())
      return m_multiWalletManager->activeWalletId();

    return "";
  }

  // Single-Wallet RPC Methods
  common::JsonValue BoltRpcServer::rpc_getStatus(const common::JsonValue &params)
  {
    WalletStatus status = m_walletManager->getStatus();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("locked", common::JsonValue(static_cast<int64_t>(status.locked ? 1 : 0)));
    result.insert("synced", common::JsonValue(static_cast<int64_t>(status.synced ? 1 : 0)));
    result.insert("blockHeight", common::JsonValue(static_cast<int64_t>(status.blockHeight)));
    result.insert("balance", common::JsonValue(static_cast<int64_t>(status.balance)));
    result.insert("unlockedBalance", common::JsonValue(static_cast<int64_t>(status.unlockedBalance)));
    result.insert("outputCount", common::JsonValue(static_cast<int64_t>(status.outputCount)));
    result.insert("transactionCount", common::JsonValue(static_cast<int64_t>(status.transactionCount)));

    if (status.syncProgress.phase != SyncProgress::IDLE)
    {
      common::JsonValue sync(common::JsonValue::OBJECT);
      sync.insert("phase", common::JsonValue(static_cast<int64_t>(status.syncProgress.phase)));
      sync.insert("totalBlocks", common::JsonValue(static_cast<int64_t>(status.syncProgress.totalBlocks)));
      sync.insert("processedBlocks", common::JsonValue(static_cast<int64_t>(status.syncProgress.processedBlocks)));
      sync.insert("candidatesFound", common::JsonValue(static_cast<int64_t>(status.syncProgress.candidatesFound)));
      sync.insert("ownedOutputs", common::JsonValue(static_cast<int64_t>(status.syncProgress.ownedOutputs)));
      sync.insert("currentHeight", common::JsonValue(static_cast<int64_t>(status.syncProgress.currentHeight)));
      if (!status.syncProgress.errorMessage.empty())
        sync.insert("errorMessage", common::JsonValue(status.syncProgress.errorMessage));
      result.insert("syncProgress", sync);
    }

    return result;
  }

  common::JsonValue BoltRpcServer::rpc_getBalance(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("balance", common::JsonValue(static_cast<int64_t>(m_walletManager->getBalance())));
    result.insert("unlockedBalance", common::JsonValue(static_cast<int64_t>(m_walletManager->getUnlockedBalance())));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_getAddress(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", common::JsonValue(m_walletManager->getAddress()));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_getOutputs(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    bool unspentOnly = true;
    if (params.isObject() && params.contains("unspentOnly"))
      unspentOnly = (params("unspentOnly").getInteger() != 0);

    auto outputs = m_walletManager->getOutputs(unspentOnly);

    common::JsonValue result(common::JsonValue::OBJECT);
    common::JsonValue arr(common::JsonValue::ARRAY);
    for (const auto &out : outputs)
    {
      common::JsonValue entry(common::JsonValue::OBJECT);
      entry.insert("blockHeight", common::JsonValue(static_cast<int64_t>(out.blockHeight)));
      entry.insert("txHash", common::JsonValue(common::podToHex(out.txHash)));
      entry.insert("amount", common::JsonValue(static_cast<int64_t>(out.amount)));
      entry.insert("outputIndex", common::JsonValue(static_cast<int64_t>(out.outputIndex)));
      entry.insert("outputKey", common::JsonValue(common::podToHex(out.outputKey)));
      entry.insert("txPublicKey", common::JsonValue(common::podToHex(out.txPublicKey)));
      entry.insert("spent", common::JsonValue(static_cast<int64_t>(out.spent ? 1 : 0)));
      entry.insert("isDeposit", common::JsonValue(static_cast<int64_t>(out.isDeposit ? 1 : 0)));
      entry.insert("term", common::JsonValue(static_cast<int64_t>(out.term)));
      arr.pushBack(entry);
    }
    result.insert("outputs", arr);
    result.insert("count", common::JsonValue(static_cast<int64_t>(outputs.size())));

    return result;
  }

  common::JsonValue BoltRpcServer::rpc_getTransactions(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    uint32_t offset = 0;
    uint32_t limit = 50;

    if (params.isObject())
    {
      if (params.contains("offset"))
        offset = static_cast<uint32_t>(params("offset").getInteger());
      if (params.contains("limit"))
        limit = static_cast<uint32_t>(params("limit").getInteger());
    }

    auto txs = m_walletManager->getTransactions(offset, limit);

    common::JsonValue result(common::JsonValue::OBJECT);
    common::JsonValue arr(common::JsonValue::ARRAY);
    for (const auto &tx : txs)
    {
      common::JsonValue entry(common::JsonValue::OBJECT);
      entry.insert("txHash", common::JsonValue(common::podToHex(tx.txHash)));
      entry.insert("blockHeight", common::JsonValue(static_cast<int64_t>(tx.blockHeight)));
      entry.insert("timestamp", common::JsonValue(static_cast<int64_t>(tx.timestamp)));
      entry.insert("fee", common::JsonValue(static_cast<int64_t>(tx.fee)));
      entry.insert("totalSent", common::JsonValue(static_cast<int64_t>(tx.totalSent)));
      entry.insert("totalReceived", common::JsonValue(static_cast<int64_t>(tx.totalReceived)));
      entry.insert("type", common::JsonValue(static_cast<int64_t>(tx.type)));
      entry.insert("confirmed", common::JsonValue(static_cast<int64_t>(tx.confirmed ? 1 : 0)));
      entry.insert("paymentId", common::JsonValue(tx.paymentId));
      arr.pushBack(entry);
    }
    result.insert("transactions", arr);

    return result;
  }

  common::JsonValue BoltRpcServer::rpc_createWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter"));
      return err;
    }

    std::string password = params("password").getString();
    if (password.size() < 8)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Password must be at least 8 characters"));
      return err;
    }

    if (m_walletManager->hasExistingWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_ALREADY_EXISTS, "Wallet already exists"));
      return err;
    }

    if (!m_walletManager->generateNewWallet(password))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INTERNAL_ERROR, "Failed to create wallet"));
      return err;
    }

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", common::JsonValue(m_walletManager->getAddress()));
    result.insert("message", common::JsonValue(std::string("Wallet created successfully")));

    m_walletManager->startSync([this](const WalletStatus &status) {});

    return result;
  }

  common::JsonValue BoltRpcServer::rpc_importWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter"));
      return err;
    }
    if (!params.contains("viewKey"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'viewKey' parameter"));
      return err;
    }
    if (!params.contains("spendKey"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'spendKey' parameter"));
      return err;
    }

    std::string password = params("password").getString();
    std::string viewKey = params("viewKey").getString();
    std::string spendKey = params("spendKey").getString();

    if (m_walletManager->hasExistingWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_ALREADY_EXISTS, "Wallet already exists"));
      return err;
    }

    if (!m_walletManager->importFromKeys(viewKey, spendKey, password))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INTERNAL_ERROR, "Failed to import wallet"));
      return err;
    }

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", common::JsonValue(m_walletManager->getAddress()));
    result.insert("message", common::JsonValue(std::string("Wallet imported successfully")));

    m_walletManager->startSync([this](const WalletStatus &status) {});

    return result;
  }

  common::JsonValue BoltRpcServer::rpc_unlockWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter"));
      return err;
    }

    if (!m_walletManager->hasExistingWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_NOT_FOUND, "No wallet found. Create or import first."));
      return err;
    }

    std::string password = params("password").getString();
    if (!m_walletManager->unlock(password))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(INVALID_PASSWORD, "Invalid password"));
      return err;
    }

    m_walletManager->startSync([this](const WalletStatus &status) {});

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("address", common::JsonValue(m_walletManager->getAddress()));
    result.insert("balance", common::JsonValue(static_cast<int64_t>(m_walletManager->getBalance())));
    result.insert("message", common::JsonValue(std::string("Wallet unlocked")));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_lockWallet(const common::JsonValue &params)
  {
    m_walletManager->lock();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("message", common::JsonValue(std::string("Wallet locked")));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_sendTransaction(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    if (!params.isObject() || !params.contains("address"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'address'"));
      return err;
    }
    if (!params.contains("amount"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'amount'"));
      return err;
    }

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
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INTERNAL_ERROR, result.errorMessage));
      return err;
    }

    common::JsonValue res(common::JsonValue::OBJECT);
    res.insert("txHash", common::JsonValue(result.txHash));
    res.insert("fee", common::JsonValue(static_cast<int64_t>(result.fee)));
    return res;
  }

  common::JsonValue BoltRpcServer::rpc_sendDeposit(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    if (!params.isObject() || !params.contains("amount"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'amount'"));
      return err;
    }
    if (!params.contains("term"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'term'"));
      return err;
    }

    DepositRequest req;
    req.amount = static_cast<uint64_t>(params("amount").getInteger());
    req.term = static_cast<uint32_t>(params("term").getInteger());

    if (params.contains("fee"))
      req.fee = static_cast<uint64_t>(params("fee").getInteger());

    TransferResult result = m_walletManager->sendDeposit(req);

    if (!result.success)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INTERNAL_ERROR, result.errorMessage));
      return err;
    }

    common::JsonValue res(common::JsonValue::OBJECT);
    res.insert("txHash", common::JsonValue(result.txHash));
    res.insert("fee", common::JsonValue(static_cast<int64_t>(result.fee)));
    return res;
  }

  common::JsonValue BoltRpcServer::rpc_sendWithdrawal(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    if (!params.isObject() || !params.contains("amount"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'amount'"));
      return err;
    }
    if (!params.contains("depositId"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'depositId'"));
      return err;
    }

    WithdrawalRequest req;
    req.amount = static_cast<uint64_t>(params("amount").getInteger());
    req.depositId = static_cast<uint64_t>(params("depositId").getInteger());

    if (params.contains("fee"))
      req.fee = static_cast<uint64_t>(params("fee").getInteger());

    TransferResult result = m_walletManager->sendWithdrawal(req);

    if (!result.success)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INTERNAL_ERROR, result.errorMessage));
      return err;
    }

    common::JsonValue res(common::JsonValue::OBJECT);
    res.insert("txHash", common::JsonValue(result.txHash));
    res.insert("fee", common::JsonValue(static_cast<int64_t>(result.fee)));
    return res;
  }

  common::JsonValue BoltRpcServer::rpc_syncNow(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    m_walletManager->syncNow();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("message", common::JsonValue(std::string("Sync triggered")));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_exportKeys(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    std::string keys = m_walletManager->exportKeys();
    if (keys.empty())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INTERNAL_ERROR, "Failed to export keys"));
      return err;
    }

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("keys", common::JsonValue(keys));
    result.insert("warning", common::JsonValue(std::string("Keep these keys secure. Anyone with these keys can access your funds.")));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_exportState(const common::JsonValue &params)
  {
    if (m_walletManager->getStatus().locked)
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(WALLET_LOCKED, "Wallet is locked"));
      return err;
    }

    std::string path = m_walletManager->exportState();

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("path", common::JsonValue(path));
    result.insert("message", common::JsonValue(std::string("State file path. Copy this file to backup or migrate your wallet.")));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_listWallets(const common::JsonValue &params)
  {
    auto wallets = m_walletManager->listWalletFiles();

    common::JsonValue result(common::JsonValue::OBJECT);
    common::JsonValue arr(common::JsonValue::ARRAY);
    for (const auto &w : wallets)
    {
      common::JsonValue entry(common::JsonValue::OBJECT);
      entry.insert("name", common::JsonValue(w));
      arr.pushBack(entry);
    }
    result.insert("wallets", arr);
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_setActiveWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("walletName"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'walletName' parameter"));
      return err;
    }

    std::string walletName = params("walletName").getString();
    m_walletManager->setActiveWallet(walletName);

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("activeWallet", common::JsonValue(walletName));
    result.insert("hasWallet", common::JsonValue(static_cast<int64_t>(m_walletManager->hasExistingWallet() ? 1 : 0)));
    result.insert("message", common::JsonValue(std::string("Active wallet set. Use unlockWallet to open.")));
    return result;
  }

  // Multi-Wallet RPC Methods
  common::JsonValue BoltRpcServer::rpc_multi_getStatus(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->getStatus();
  }

  common::JsonValue BoltRpcServer::rpc_multi_getBalance(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->getBalance();
  }

  common::JsonValue BoltRpcServer::rpc_multi_getAddress(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->getAddress();
  }

  common::JsonValue BoltRpcServer::rpc_multi_getOutputs(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->getOutputs(params);
  }

  common::JsonValue BoltRpcServer::rpc_multi_getTransactions(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->getTransactions(params);
  }

  common::JsonValue BoltRpcServer::rpc_multi_createWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("wallet_id"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'wallet_id' parameter"));
      return err;
    }
    if (!params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'password' parameter"));
      return err;
    }

    std::string walletId = params("wallet_id").getString();
    std::string password = params("password").getString();

    return m_multiWalletManager->createWallet(walletId, password);
  }

  common::JsonValue BoltRpcServer::rpc_multi_importWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("wallet_id"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'wallet_id' parameter"));
      return err;
    }
    if (!params.contains("viewKey") || !params.contains("spendKey") || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'viewKey', 'spendKey', or 'password'"));
      return err;
    }

    return m_multiWalletManager->importWallet(
        params("wallet_id").getString(),
        params("viewKey").getString(),
        params("spendKey").getString(),
        params("password").getString());
  }

  common::JsonValue BoltRpcServer::rpc_multi_loadWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("wallet_id") || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'wallet_id' or 'password'"));
      return err;
    }

    return m_multiWalletManager->loadWallet(
        params("wallet_id").getString(),
        params("password").getString());
  }

  common::JsonValue BoltRpcServer::rpc_multi_unloadWallet(const common::JsonValue &params)
  {
    return m_multiWalletManager->unloadWallet();
  }

  common::JsonValue BoltRpcServer::rpc_multi_deleteWallet(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("wallet_id") || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'wallet_id' or 'password'"));
      return err;
    }

    return m_multiWalletManager->deleteWallet(
        params("wallet_id").getString(),
        params("password").getString());
  }

  common::JsonValue BoltRpcServer::rpc_multi_sendTransaction(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->sendTransfer(params);
  }

  common::JsonValue BoltRpcServer::rpc_multi_sendDeposit(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->sendDeposit(params);
  }

  common::JsonValue BoltRpcServer::rpc_multi_sendWithdrawal(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded. Use loadWallet first."));
      return err;
    }

    return m_multiWalletManager->sendWithdrawal(params);
  }

  common::JsonValue BoltRpcServer::rpc_multi_syncNow(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded."));
      return err;
    }

    common::JsonValue result(common::JsonValue::OBJECT);
    result.insert("message", common::JsonValue(std::string("Sync is automatic in multi-wallet mode. Wallet is syncing in background.")));
    return result;
  }

  common::JsonValue BoltRpcServer::rpc_multi_exportKeys(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded."));
      return err;
    }

    return m_multiWalletManager->exportKeys();
  }

  common::JsonValue BoltRpcServer::rpc_multi_exportState(const common::JsonValue &params)
  {
    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded."));
      return err;
    }

    return m_multiWalletManager->exportState();
  }

  common::JsonValue BoltRpcServer::rpc_multi_listWallets(const common::JsonValue &params)
  {
    return m_multiWalletManager->listWallets();
  }

  common::JsonValue BoltRpcServer::rpc_multi_changePassword(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("old_password") || !params.contains("new_password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'old_password' or 'new_password'"));
      return err;
    }

    if (!m_multiWalletManager->hasActiveWallet())
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(NO_WALLET_LOADED, "No wallet loaded."));
      return err;
    }

    return m_multiWalletManager->changePassword(
        params("old_password").getString(),
        params("new_password").getString());
  }

  common::JsonValue BoltRpcServer::rpc_multi_importFromStateFile(const common::JsonValue &params)
  {
    if (!params.isObject() || !params.contains("wallet_id") || !params.contains("file_path") || !params.contains("password"))
    {
      common::JsonValue err(common::JsonValue::OBJECT);
      err.insert("error", makeErrorObj(JSON_RPC_INVALID_PARAMS, "Missing 'wallet_id', 'file_path', or 'password'"));
      return err;
    }

    return m_multiWalletManager->importFromStateFile(
        params("wallet_id").getString(),
        params("file_path").getString(),
        params("password").getString());
  }

  // Helper
  common::JsonValue BoltRpcServer::makeErrorObj(int code, const std::string &message) const
  {
    common::JsonValue errObj(common::JsonValue::OBJECT);
    errObj.insert("code", common::JsonValue(static_cast<int64_t>(code)));
    errObj.insert("message", common::JsonValue(message));
    return errObj;
  }

} // namespace BoltRPC