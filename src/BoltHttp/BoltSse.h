// BoltSse.h — Server-Sent Events stream for BoltHttp
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace BoltHttp
{
  // A single SSE connection to one client
  class SseConnection
  {
  public:
    explicit SseConnection(int clientFd);
    ~SseConnection();

    // Send an event with optional event type and data
    bool sendEvent(const std::string &type, const std::string &data);
    // Send a comment (keep-alive ping)
    bool sendComment(const std::string &comment);
    // Close the connection
    void close();
    // Check if connection is still alive
    bool isAlive() const { return m_fd >= 0; }
    // Get the file descriptor
    int fd() const { return m_fd; }

  private:
    int m_fd;
  };

  // Callback for when a client subscribes to SSE
  using SseSubscribeHandler = std::function<void(SseConnection &conn)>;

  // Manages multiple SSE connections and broadcasts to all of them
  class SseBroadcaster
  {
  public:
    SseBroadcaster() = default;
    ~SseBroadcaster();

    // Add a new SSE connection (takes ownership)
    void addConnection(SseConnection *conn);
    // Remove a closed connection
    void removeConnection(int fd);
    // Broadcast an event to all connected clients
    void broadcast(const std::string &type, const std::string &data);
    // Send keep-alive ping to all clients
    void pingAll();
    // Number of active connections
    size_t connectionCount() const;

  private:
    std::vector<SseConnection *> m_connections;
    mutable std::mutex m_mutex;
  };

  // Build the HTTP headers to initiate an SSE stream
  std::string buildSseHandshakeResponse();

} // namespace BoltHttp