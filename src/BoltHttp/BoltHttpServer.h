// BoltHttpServer.h — Hybrid HTTP server with fiber I/O, WebSocket, and SSE
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltHttpRequest.h"
#include "BoltHttpResponse.h"
#include "BoltWebSocket.h"
#include "BoltSse.h"
#include "IExecutor.h"
#include "ThreadPool.h"

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <memory>

namespace platform_system
{
  class Dispatcher;
}

namespace BoltHttp
{
  using RequestHandler = std::function<void(const Request &, Response &)>;
  using WebSocketConnectHandler = std::function<void(WebSocket &)>;
  using SseConnectHandler = std::function<void(SseConnection &)>;

  enum class WorkClass
  {
    Auto, // Server decides based on request path (default)
    Fast, // Always run on fiber (balance queries, status)
    Heavy // Always dispatch to thread pool (crypto, scanning)
  };

  class Server
  {
  public:
    explicit Server(platform_system::Dispatcher *dispatcher = nullptr,
                    size_t threadCount = 1);
    ~Server();

    void start(const std::string &bindIp, uint16_t port);
    void stop();

    // Register HTTP request handler
    void onRequest(RequestHandler handler, WorkClass cls = WorkClass::Auto);

    // Register WebSocket upgrade handler (for /websocket or similar paths)
    void onWebSocket(WebSocketConnectHandler handler);

    // Register SSE stream handler (for /events or similar paths)
    void onSse(SseConnectHandler handler);

    // Access the SSE broadcaster for pushing events
    SseBroadcaster &sseBroadcaster() { return m_sseBroadcaster; }

    // Add a path pattern that should always be treated as heavy work
    void markHeavy(const std::string &pathPrefix);
    // Add a path pattern that should always be treated as fast work
    void markFast(const std::string &pathPrefix);

  private:
    void acceptLoop();
    void workerLoop(int epollFd);
    void handleClient(int clientFd);

    // Auto-classify a request based on URL and method
    WorkClass classifyRequest(const Request &req) const;

    platform_system::Dispatcher *m_dispatcher = nullptr;
    std::shared_ptr<ThreadPool> m_threadPool;
    std::shared_ptr<IExecutor> m_executor;

    int m_serverSocket = -1;
    int m_epollFd = -1;
    std::atomic<bool> m_running{false};
    size_t m_threadCount;
    std::thread m_acceptThread;
    std::vector<std::thread> m_workers;

    std::mutex m_clientsMutex;
    std::unordered_map<int, std::string> m_clientBuffers;

    RequestHandler m_handler;
    WorkClass m_defaultClass = WorkClass::Auto;

    // WebSocket and SSE handlers
    WebSocketConnectHandler m_wsHandler;
    SseConnectHandler m_sseHandler;
    SseBroadcaster m_sseBroadcaster;
    std::unordered_map<int, WebSocket *> m_wsConnections;

    // Path-based classification tables
    std::vector<std::string> m_heavyPaths;
    std::vector<std::string> m_fastPaths;

    // Default classification tables
    static const std::vector<std::string> DEFAULT_HEAVY_PATHS;
    static const std::vector<std::string> DEFAULT_FAST_PATHS;
  };
}