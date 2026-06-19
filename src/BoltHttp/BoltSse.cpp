// BoltSse.cpp — Server-Sent Events implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltSse.h"
#include "BoltSocket.hpp"
#include <algorithm>

namespace BoltHttp
{

  // Build the HTTP 200 response that starts an SSE stream
  std::string buildSseHandshakeResponse()
  {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/event-stream\r\n"
           "Cache-Control: no-cache\r\n"
           "Connection: keep-alive\r\n"
           "Access-Control-Allow-Origin: *\r\n"
           "\r\n";
  }

  // SseConnection
  SseConnection::SseConnection(int clientFd)
      : m_fd(clientFd) {}

  SseConnection::~SseConnection()
  {
    close();
  }

  void SseConnection::close()
  {
    if (m_fd >= 0)
    {
      boltShutdown(m_fd, BOLT_SHUT_RDWR);
      boltClose(m_fd);
      m_fd = -1;
    }
  }

  bool SseConnection::sendEvent(const std::string &type, const std::string &data)
  {
    if (m_fd < 0)
      return false;

    // SSE format: "event: type\ndata: data\n\n"
    std::string frame;
    if (!type.empty())
      frame += "event: " + type + "\n";
    frame += "data: " + data + "\n\n";

    const int sent = boltSend(m_fd, frame.c_str(), frame.size(), BOLT_MSG_NOSIGNAL);
    return sent == static_cast<int>(frame.size());
  }

  bool SseConnection::sendComment(const std::string &comment)
  {
    if (m_fd < 0)
      return false;

    std::string frame = ": " + comment + "\n\n";
    const int sent = boltSend(m_fd, frame.c_str(), frame.size(), BOLT_MSG_NOSIGNAL);
    return sent == static_cast<int>(frame.size());
  }

  // SseBroadcaster
  SseBroadcaster::~SseBroadcaster()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto *conn : m_connections)
      delete conn;
    m_connections.clear();
  }

  void SseBroadcaster::addConnection(SseConnection *conn)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connections.push_back(conn);
  }

  void SseBroadcaster::removeConnection(int fd)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connections.erase(
        std::remove_if(m_connections.begin(), m_connections.end(),
                       [fd](SseConnection *c)
                       { return c->fd() == fd; }),
        m_connections.end());
  }

  void SseBroadcaster::broadcast(const std::string &type, const std::string &data)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<SseConnection *> dead;
    for (auto *conn : m_connections)
    {
      if (conn->isAlive())
      {
        if (!conn->sendEvent(type, data))
          dead.push_back(conn);
      }
      else
      {
        dead.push_back(conn);
      }
    }
    for (auto *d : dead)
    {
      m_connections.erase(
          std::remove(m_connections.begin(), m_connections.end(), d),
          m_connections.end());
      delete d;
    }
  }

  void SseBroadcaster::pingAll()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto *conn : m_connections)
    {
      if (conn->isAlive())
        conn->sendComment("keep-alive");
    }
  }

  size_t SseBroadcaster::connectionCount() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_connections.size();
  }

} // namespace BoltHttp