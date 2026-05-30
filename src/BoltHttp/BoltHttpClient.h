// BoltHttpClient.h — Simple HTTP client for sidechain/DEX RPC calls
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <cstdint>

namespace BoltHttp
{
  struct HttpClientResponse
  {
    bool success = false;
    int statusCode = 0;
    std::string body;
    std::string error;
  };

  class HttpClient
  {
  public:
    HttpClient(const std::string &host, uint16_t port);
    ~HttpClient();

    HttpClientResponse post(const std::string &path, const std::string &body);
    HttpClientResponse get(const std::string &path);

  private:
    bool connect();
    std::string m_host;
    uint16_t m_port;
    int m_socket = -1;
  };
}