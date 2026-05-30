// BoltHttpRequest.h — Minimal HTTP request parser
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace BoltHttp
{
  struct Request
  {
    std::string method;
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;
  };
}