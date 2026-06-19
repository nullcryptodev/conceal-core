// BoltHttpServer.cpp — Hybrid HTTP server with fiber I/O, WebSocket, and SSE
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltHttpServer.h"
#include "BoltSocket.hpp"
#include "FiberExecutor.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <iostream>
#include <sstream>
#include <chrono>

namespace BoltHttp
{
  namespace
  {
    void setNonBlocking(int fd)
    {
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    void addEpoll(int epollFd, int fd)
    {
      epoll_event ev{};
      ev.events = EPOLLIN | EPOLLET;
      ev.data.fd = fd;
      epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &ev);
    }

    bool parseRequest(const std::string &raw, Request &req)
    {
      std::istringstream stream(raw);
      std::string line;
      std::getline(stream, line);
      if (line.empty() || line.back() == '\r')
        line.pop_back();
      if (line.empty())
        return false;

      std::istringstream requestLine(line);
      requestLine >> req.method >> req.url;

      while (std::getline(stream, line) && line != "\r" && !line.empty())
      {
        if (line.back() == '\r')
          line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos)
        {
          std::string key = line.substr(0, colon);
          std::string value = line.substr(colon + 1);
          if (!value.empty() && value[0] == ' ')
            value = value.substr(1);
          req.headers[key] = value;
        }
      }

      auto it = req.headers.find("Content-Length");
      if (it != req.headers.end())
      {
        size_t contentLength = std::stoul(it->second);
        size_t bodyStart = raw.find("\r\n\r\n");
        if (bodyStart != std::string::npos)
        {
          bodyStart += 4;
          if (raw.size() >= bodyStart + contentLength)
          {
            std::string body = raw.substr(bodyStart, contentLength);
            req.body.assign(body.begin(), body.end());
          }
        }
      }

      return true;
    }
  }

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

  // Constructor
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

  // Start
  void Server::start(const std::string &bindIp, uint16_t port)
  {
    m_serverSocket = socket(AF_INET, SOCK_STREAM | BOLT_SOCK_NONBLOCK, 0);
    if (m_serverSocket < 0)
    {
      std::cerr << "BoltHttp: Failed to create socket" << std::endl;
      return;
    }

    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, bindIp.c_str(), &addr.sin_addr);

    if (bind(m_serverSocket, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      std::cerr << "BoltHttp: Failed to bind to " << bindIp << ":" << port << std::endl;
      close(m_serverSocket);
      return;
    }

    listen(m_serverSocket, SOMAXCONN);
    m_epollFd = epoll_create1(0);
    if (m_epollFd < 0)
    {
      std::cerr << "BoltHttp: Failed to create epoll" << std::endl;
      close(m_serverSocket);
      return;
    }

    addEpoll(m_epollFd, m_serverSocket);
    m_running = true;

    for (size_t i = 0; i < m_threadCount; ++i)
      m_workers.emplace_back(&Server::workerLoop, this, m_epollFd);

    m_acceptThread = std::thread(&Server::acceptLoop, this);

    std::cout << "BoltHttp: Listening on " << bindIp << ":" << port
              << " (" << m_threadCount << " workers"
              << (m_dispatcher ? ", fiber mode" : ", thread mode") << ")"
              << (m_wsHandler ? " +WebSocket" : "")
              << (m_sseHandler ? " +SSE" : "") << std::endl;
  }

  // Stop
  void Server::stop()
  {
    m_running = false;

    if (m_executor)
      m_executor->stop();

    if (m_serverSocket >= 0)
    {
      shutdown(m_serverSocket, SHUT_RDWR);
      close(m_serverSocket);
      m_serverSocket = -1;
    }
    if (m_epollFd >= 0)
    {
      close(m_epollFd);
      m_epollFd = -1;
    }

    if (m_acceptThread.joinable())
      m_acceptThread.join();

    // Clean up WebSocket connections
    {
      std::lock_guard<std::mutex> lock(m_clientsMutex);
      for (auto &pair : m_wsConnections)
      {
        pair.second->close();
        delete pair.second;
      }
      m_wsConnections.clear();
    }

    for (auto &t : m_workers)
    {
      if (t.joinable())
        t.detach();
    }
    m_workers.clear();
  }

  // Handler registration
  void Server::onRequest(RequestHandler handler, WorkClass cls)
  {
    m_handler = std::move(handler);
    m_defaultClass = cls;
  }
  void Server::onWebSocket(WebSocketConnectHandler handler) { m_wsHandler = std::move(handler); }
  void Server::onSse(SseConnectHandler handler) { m_sseHandler = std::move(handler); }

  void Server::markHeavy(const std::string &pathPrefix) { m_heavyPaths.push_back(pathPrefix); }
  void Server::markFast(const std::string &pathPrefix) { m_fastPaths.push_back(pathPrefix); }

  WorkClass Server::classifyRequest(const Request &req) const { return m_dispatcher ? WorkClass::Fast : WorkClass::Heavy; }

  // Accept loop
  void Server::acceptLoop()
  {
    while (m_running)
    {
      sockaddr_in clientAddr{};
      socklen_t clientLen = sizeof(clientAddr);
      int clientFd = accept4(m_serverSocket, (sockaddr *)&clientAddr, &clientLen, BOLT_SOCK_NONBLOCK);
      if (clientFd < 0)
        continue;

      setNonBlocking(clientFd);

      {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientBuffers[clientFd] = "";
      }

      addEpoll(m_epollFd, clientFd);
    }
  }

  // Worker loop
  void Server::workerLoop(int epollFd)
  {
    epoll_event events[64];

    while (m_running)
    {
      int nfds = epoll_wait(epollFd, events, 64, 100);
      if (nfds < 0)
      {
        if (errno == EINTR)
          continue;
        break;
      }

      for (int i = 0; i < nfds; ++i)
      {
        int fd = events[i].data.fd;
        if (fd == m_serverSocket)
          continue;

        // Check if this fd is a WebSocket connection
        {
          std::lock_guard<std::mutex> lock(m_clientsMutex);
          auto it = m_wsConnections.find(fd);
          if (it != m_wsConnections.end())
          {
            it->second->processIncoming();
            continue;
          }
        }

        handleClient(fd);
      }
    }
  }

  // Client handler — routes HTTP, WebSocket, and SSE
  void Server::handleClient(int clientFd)
  {
    char buffer[4096];
    std::string &accumulator = m_clientBuffers[clientFd];

    // Read all available data
    while (true)
    {
      ssize_t n = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
      if (n > 0)
      {
        buffer[n] = '\0';
        accumulator += buffer;
      }
      else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
      {
        shutdown(clientFd, SHUT_RDWR);
        close(clientFd);
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientBuffers.erase(clientFd);
        m_wsConnections.erase(clientFd);
        return;
      }
      else
        break;
    }

    // Wait for complete HTTP headers
    if (accumulator.find("\r\n\r\n") == std::string::npos)
      return;

    // WebSocket upgrade
    if (m_wsHandler &&
        accumulator.find("Upgrade: websocket") != std::string::npos &&
        accumulator.find("Sec-WebSocket-Key:") != std::string::npos)
    {
      std::string key;
      size_t keyPos = accumulator.find("Sec-WebSocket-Key: ");
      if (keyPos != std::string::npos)
      {
        keyPos += 19;
        size_t keyEnd = accumulator.find("\r\n", keyPos);
        if (keyEnd != std::string::npos)
          key = accumulator.substr(keyPos, keyEnd - keyPos);
      }

      if (!key.empty())
      {
        std::string handshake = buildWebSocketHandshakeResponse(key);
        send(clientFd, handshake.c_str(), handshake.size(), MSG_NOSIGNAL);

        // Create WebSocket and register in epoll for incoming frames
        WebSocket *ws = new WebSocket(clientFd);

        {
          std::lock_guard<std::mutex> lock(m_clientsMutex);
          m_clientBuffers.erase(clientFd);
          m_wsConnections[clientFd] = ws;
        }

        // Non-blocking: just registers callbacks, returns immediately
        if (m_wsHandler)
          m_wsHandler(*ws);

        return;
      }
    }

    // SSE stream
    if (m_sseHandler &&
        accumulator.find("Accept: text/event-stream") != std::string::npos)
    {
      std::string handshake = buildSseHandshakeResponse();
      send(clientFd, handshake.c_str(), handshake.size(), MSG_NOSIGNAL);

      {
        std::lock_guard<std::mutex> lock(m_clientsMutex);
        m_clientBuffers.erase(clientFd);
      }

      SseConnection *conn = new SseConnection(clientFd);
      if (m_sseHandler)
        m_sseHandler(*conn);

      return;
    }

    // Normal HTTP request
    Request req;
    if (parseRequest(accumulator, req))
    {
      Response resp;
      resp.headers["Content-Type"] = "application/json";
      resp.headers["Access-Control-Allow-Origin"] = "*";

      if (m_handler)
      {
        WorkClass cls = classifyRequest(req);
        if (m_defaultClass != WorkClass::Auto)
          cls = m_defaultClass;

        if (cls == WorkClass::Heavy && m_executor)
        {
          auto token = m_executor->dispatch([&]()
                                            { m_handler(req, resp); });
          token->wait();
        }
        else
        {
          if (m_executor)
            m_executor->runInline([&]()
                                  { m_handler(req, resp); });
          else
            m_handler(req, resp);
        }
      }

      std::vector<uint8_t> rawResp = resp.toBytes();
      send(clientFd, rawResp.data(), rawResp.size(), MSG_NOSIGNAL);
    }

    shutdown(clientFd, SHUT_RDWR);
    close(clientFd);
    std::lock_guard<std::mutex> lock(m_clientsMutex);
    m_clientBuffers.erase(clientFd);
  }
}