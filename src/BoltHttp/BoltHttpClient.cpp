// BoltHttpClient.cpp — Simple HTTP client implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltHttpClient.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <string>

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
      ::shutdown(m_socket, SHUT_RDWR);
      close(m_socket);
    }
  }

  namespace
  {

    size_t parseContentLength(const std::string &headers)
    {
      size_t pos = 0;
      while (pos < headers.size())
      {
        size_t lineEnd = headers.find("\r\n", pos);
        if (lineEnd == std::string::npos)
          break;
        std::string line = headers.substr(pos, lineEnd - pos);
        const std::string prefix = "Content-Length:";
        if (line.size() >= prefix.size() &&
            line.compare(0, prefix.size(), prefix) == 0)
        {
          size_t i = prefix.size();
          while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
          return static_cast<size_t>(std::stoull(line.substr(i)));
        }
        pos = lineEnd + 2;
      }
      return 0;
    }

  } // namespace

  bool HttpClient::connect(int recvTimeoutSec)
  {
    m_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_socket < 0)
      return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

    if (::connect(m_socket, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      close(m_socket);
      m_socket = -1;
      return false;
    }

    struct timeval tv;
    tv.tv_sec = recvTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(m_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    return true;
  }

  HttpClientResponse HttpClient::readHttpResponse(int socket)
  {
    HttpClientResponse resp;
    char buf[65536];
    std::string raw;
    while (true)
    {
      ssize_t n = recv(socket, buf, sizeof(buf), 0);
      if (n < 0)
      {
        if (!raw.empty())
          break;
        resp.error = "Failed to read HTTP response";
        return resp;
      }
      if (n == 0)
        break;
      raw.append(buf, static_cast<size_t>(n));
    }

    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
      resp.error = "Invalid HTTP response";
      return resp;
    }

    std::string headers = raw.substr(0, headerEnd);
    std::istringstream headerStream(headers);
    std::string statusLine;
    std::getline(headerStream, statusLine);

    size_t codeStart = statusLine.find(' ');
    if (codeStart != std::string::npos)
    {
      std::string codeStr = statusLine.substr(codeStart + 1, 3);
      resp.statusCode = std::stoi(codeStr);
    }

    const size_t bodyStart = headerEnd + 4;
    const size_t contentLength = parseContentLength(headers);
    if (contentLength > 0)
    {
      if (raw.size() < bodyStart + contentLength)
      {
        resp.error = "Incomplete HTTP response (expected " + std::to_string(contentLength) +
                     " bytes, got " + std::to_string(raw.size() - bodyStart) +
                     "). Try increasing daemon RPC timeout or check network.";
        return resp;
      }
      resp.body = raw.substr(bodyStart, contentLength);
    }
    else
    {
      resp.body = raw.substr(bodyStart);
    }

    resp.success = (resp.statusCode >= 200 && resp.statusCode < 300);
    return resp;
  }

  HttpClientResponse HttpClient::post(const std::string &path, const std::string &body,
                                      int recvTimeoutSec)
  {
    HttpClientResponse resp;

    if (!connect(recvTimeoutSec))
    {
      resp.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
      return resp;
    }

    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: " << m_host << "\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n"
            << "\r\n"
            << body;

    std::string reqStr = request.str();
    ssize_t sent = send(m_socket, reqStr.c_str(), reqStr.size(), 0);
    if (sent <= 0)
    {
      resp.error = "Failed to send request";
      return resp;
    }

    resp = readHttpResponse(m_socket);
    return resp;
  }

  HttpClientResponse HttpClient::get(const std::string &path, int recvTimeoutSec)
  {
    HttpClientResponse resp;

    if (!connect(recvTimeoutSec))
    {
      resp.error = "Failed to connect to " + m_host + ":" + std::to_string(m_port);
      return resp;
    }

    std::ostringstream request;
    request << "GET " << path << " HTTP/1.1\r\n"
            << "Host: " << m_host << "\r\n"
            << "Connection: close\r\n"
            << "\r\n";

    std::string reqStr = request.str();
    ssize_t sent = send(m_socket, reqStr.c_str(), reqStr.size(), 0);
    if (sent <= 0)
    {
      resp.error = "Failed to send request";
      return resp;
    }

    return readHttpResponse(m_socket);
  }
}