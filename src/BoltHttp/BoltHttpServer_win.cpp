// BoltHttpServer_win.cpp — stub (epoll server is Linux-only for now)
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs

#include "BoltHttpServer.h"
#include "FiberExecutor.h"
#include <iostream>

namespace BoltHttp
{
  const std::vector<std::string> Server::DEFAULT_HEAVY_PATHS = {
      "/sendrawtransaction", "/submitblock", "/getblocktemplate",
      "createToken", "mintToken", "burnToken", "transfer",
      "dex_submitOrder", "dex_cancelOrder", "sendFusionTransaction",
      "createDeposit", "withdrawDeposit"};

  const std::vector<std::string> Server::DEFAULT_FAST_PATHS = {
      "/getinfo", "/getheight", "/getblockcount", "/getblockhash",
      "getBalance", "getTokenBalance", "getTokens", "getStatus",
      "getPendingTransactions", "getValidators", "getTransactions",
      "getAssetRegistry", "getBridgeStatus",
      "dex_getOrders", "dex_getTrades", "dex_getAllTrades",
      "dex_deposit", "dex_getEscrowBalance", "faucet"};

  Server::Server(platform_system::Dispatcher *dispatcher, size_t threadCount)
      : m_dispatcher(dispatcher),
        m_threadCount(threadCount),
        m_heavyPaths(DEFAULT_HEAVY_PATHS),
        m_fastPaths(DEFAULT_FAST_PATHS)
  {
    m_threadPool = std::make_shared<ThreadPool>(threadCount);
    if (m_dispatcher)
      m_executor = std::make_shared<FiberExecutor>(*m_dispatcher, m_threadPool);
    else
      m_executor = m_threadPool;
  }

  Server::~Server()
  {
    stop();
  }

  void Server::start(const std::string &bindIp, uint16_t port)
  {
    std::cerr << "BoltHttp: HTTP server is not supported on this platform (" << bindIp << ":" << port << ")\n";
  }

  void Server::stop() {}

  void Server::onRequest(RequestHandler handler, WorkClass cls)
  {
    m_handler = std::move(handler);
    m_defaultClass = cls;
  }

  void Server::onWebSocket(WebSocketConnectHandler handler) { m_wsHandler = std::move(handler); }
  void Server::onSse(SseConnectHandler handler) { m_sseHandler = std::move(handler); }

  void Server::markHeavy(const std::string &pathPrefix) { m_heavyPaths.push_back(pathPrefix); }
  void Server::markFast(const std::string &pathPrefix) { m_fastPaths.push_back(pathPrefix); }

  WorkClass Server::classifyRequest(const Request &req) const
  {
    return m_dispatcher ? WorkClass::Fast : WorkClass::Heavy;
  }

  void Server::acceptLoop() {}
  void Server::workerLoop(int epollFd) { (void)epollFd; }
  void Server::handleClient(int clientFd) { (void)clientFd; }
} // namespace BoltHttp
