// BoltHttpClient.cpp — Simple HTTP client implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltHttpClient.h"
#include "BoltSocket.hpp"
#include "Common/WindowsMacroUndef.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <chrono>
#include <thread>

namespace BoltHttp
{
  HttpClient::HttpClient(const std::string &host, uint16_t port)
      : m_host(host), m_port(port)
  {
  }

  HttpClient::~HttpClient()
  {
    if (m_socket >= 0)
    {
      boltShutdown(m_socket, BOLT_SHUT_RDWR);
      boltClose(m_socket);
    }
  }

  bool HttpClient::connect(int timeoutMs)
  {
    m_socket = boltSocket(AF_INET, SOCK_STREAM | BOLT_SOCK_NONBLOCK, 0);
    if (m_socket < 0)
      return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    if (boltInetPton(AF_INET, m_host.c_str(), &addr.sin_addr) != 1)
    {
      boltClose(m_socket);
      m_socket = -1;
      return false;
    }

    int result = boltConnect(m_socket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (result < 0 && boltErrno() != BOLT_EINPROGRESS)
    {
      boltClose(m_socket);
      m_socket = -1;
      return false;
    }

    if (result < 0)
    {
      const int sec = std::max(1, timeoutMs / 1000);
      const int usec = (timeoutMs % 1000) * 1000;
      result = boltSelectWrite(m_socket, sec, usec);
      if (result <= 0)
      {
        boltClose(m_socket);
        m_socket = -1;
        return false;
      }

      if (boltGetSocketError(m_socket) != 0)
      {
        boltClose(m_socket);
        m_socket = -1;
        return false;
      }
    }

    if (boltSetNonBlocking(m_socket, false) != 0)
    {
      boltClose(m_socket);
      m_socket = -1;
      return false;
    }

    return true;
  }

  HttpClientResponse HttpClient::post(const std::string &path, const std::string &body,
                                      const char *contentType,
                                      int connectTimeoutMs, int recvTimeoutMs)
  {
    HttpClientResponse resp;

    if (!connect(connectTimeoutMs))
    {
      resp.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
      return resp;
    }

    boltSetRecvTimeoutMs(m_socket, std::max(1, recvTimeoutMs));

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << m_host << "\r\n"
            << "Content-Type: " << contentType << "\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;

    const std::string reqStr = request.str();
    if (boltSend(m_socket, reqStr.c_str(), reqStr.size(), 0) <= 0)
    {
      resp.error = "Failed to send request";
      return resp;
    }

    char buf[4096];
    std::string raw;
    while (true)
    {
      const int n = boltRecv(m_socket, buf, sizeof(buf) - 1, 0);
      if (n <= 0)
        break;
      buf[n] = '\0';
      raw += buf;
    }

    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
      resp.error = "Invalid HTTP response";
      return resp;
    }

    const std::string headers = raw.substr(0, headerEnd);
    std::istringstream headerStream(headers);
    std::string statusLine;
    std::getline(headerStream, statusLine);

    const size_t codeStart = statusLine.find(' ');
    if (codeStart != std::string::npos)
    {
      const std::string codeStr = statusLine.substr(codeStart + 1, 3);
      resp.statusCode = std::stoi(codeStr);
    }

    resp.body = raw.substr(headerEnd + 4);
    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);

    return resp;
  }

  HttpClientResponse HttpClient::get(const std::string &path,
                                      int connectTimeoutMs, int recvTimeoutMs)
  {
    HttpClientResponse resp;

    if (!connect(connectTimeoutMs))
    {
      resp.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
      return resp;
    }

    boltSetRecvTimeoutMs(m_socket, std::max(1, recvTimeoutMs));

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << m_host << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";

    const std::string reqStr = request.str();
    if (boltSend(m_socket, reqStr.c_str(), reqStr.size(), 0) <= 0)
    {
      resp.error = "Failed to send request";
      return resp;
    }

    char buf[4096];
    std::string raw;
    while (true)
    {
      const int n = boltRecv(m_socket, buf, sizeof(buf) - 1, 0);
      if (n <= 0)
        break;
      buf[n] = '\0';
      raw += buf;
    }

    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
      resp.error = "Invalid HTTP response";
      return resp;
    }

    const std::string headers = raw.substr(0, headerEnd);
    std::istringstream headerStream(headers);
    std::string statusLine;
    std::getline(headerStream, statusLine);

    const size_t codeStart = statusLine.find(' ');
    if (codeStart != std::string::npos)
    {
      const std::string codeStr = statusLine.substr(codeStart + 1, 3);
      resp.statusCode = std::stoi(codeStr);
    }

    resp.body = raw.substr(headerEnd + 4);
    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);

    return resp;
  }
}
