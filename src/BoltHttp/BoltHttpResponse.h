// BoltHttpResponse.h — Minimal HTTP response builder
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace BoltHttp
{
  struct Response
  {
    int status = 200;
    std::string statusMessage = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    void setBody(const std::string &text)
    {
      body.assign(text.begin(), text.end());
    }

    std::vector<uint8_t> toBytes() const
    {
      std::string raw = "HTTP/1.1 " + std::to_string(status) + " " + statusMessage + "\r\n";
      for (const auto &h : headers)
        raw += h.first + ": " + h.second + "\r\n";
      raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
      raw += "\r\n";
      std::vector<uint8_t> result(raw.begin(), raw.end());
      result.insert(result.end(), body.begin(), body.end());
      return result;
    }
  };
}