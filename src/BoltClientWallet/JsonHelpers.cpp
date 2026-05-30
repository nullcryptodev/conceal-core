// JsonHelpers.cpp — JSON parsing utilities and token info cache
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "JsonHelpers.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "Rpc/HttpClient.h"
#include <System/Dispatcher.h>
#include <sstream>
#include <ctime>
#include <iostream>

std::string extractJsonString(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\":\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
  {
    search = "\"" + key + "\":";
    pos = json.find(search);
    if (pos == std::string::npos)
      return "";
    pos += search.length();
    size_t end = json.find_first_of(",}]", pos);
    if (end == std::string::npos)
      return "";
    std::string val = json.substr(pos, end - pos);
    if (!val.empty() && val.front() == '"' && val.back() == '"')
      val = val.substr(1, val.size() - 2);
    return val;
  }
  pos += search.length();
  size_t end = json.find("\"", pos);
  if (end == std::string::npos)
    return "";
  return json.substr(pos, end - pos);
}

uint64_t extractJsonNumber(const std::string &json, const std::string &key)
{
  std::string search = "\"" + key + "\":";
  size_t pos = json.find(search);
  if (pos == std::string::npos)
    return 0;
  pos += search.length();
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
    pos++;
  std::string num;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
  {
    num += json[pos];
    pos++;
  }
  if (num.empty())
    return 0;
  try
  {
    return std::stoull(num);
  }
  catch (...)
  {
    return 0;
  }
}

std::string sidechainCall(const std::string &host, uint16_t port,
                          const std::string &method, const std::string &params)
{
  platform_system::Dispatcher dispatcher;
  cn::HttpClient client(dispatcher, host, port);

  cn::HttpRequest req;
  cn::HttpResponse res;

  std::string body = R"({"jsonrpc":"2.0","id":1,"method":")" + method +
                     R"(","params":)" + params + "}";

  req.setUrl("/json_rpc");
  req.setBody(body);
  req.addHeader("Content-Type", "application/json");

  try
  {
    client.request(req, res);
    return res.getBody();
  }
  catch (...)
  {
    return "{}";
  }
}

std::string formatAmount(uint64_t amount, uint8_t decimals)
{
  if (decimals == 0)
    return std::to_string(amount);
  std::string s = std::to_string(amount);
  while (s.length() <= decimals)
    s = "0" + s;
  size_t dotPos = s.length() - decimals;
  std::string result = s.substr(0, dotPos) + "." + s.substr(dotPos);
  while (result.back() == '0' && result[result.size() - 2] != '.')
    result.pop_back();
  return result;
}

// Token cache for decimal lookups
static std::unordered_map<uint64_t, TokenInfoCache> g_tokenCache;

void loadTokenCache(const std::string &host, uint16_t port)
{
  g_tokenCache.clear();

  // SCCX is always token 0 with 6 decimals
  TokenInfoCache sccx;
  sccx.symbol = "SCCX";
  sccx.name = "SCCX";
  sccx.decimals = 6;
  sccx.backingModel = 0;
  sccx.verified = true;
  sccx.sourceChain = "conceal";
  sccx.sourceAsset = "native";
  g_tokenCache[0] = sccx;

  std::string result = sidechainCall(host, port, "getTokens", "{}");

  // Find the result array
  size_t arrayStart = result.find("\"result\":[");
  if (arrayStart == std::string::npos)
    return;
  arrayStart += 9; // skip past "result":[

  // Parse each complete JSON object { ... }
  size_t pos = arrayStart;
  while ((pos = result.find("{\"id\":", pos)) != std::string::npos)
  {
    // Find the matching closing brace for this object
    size_t objEnd = pos;
    int braceDepth = 0;
    bool inString = false;
    while (objEnd < result.size())
    {
      char c = result[objEnd];
      if (c == '"' && (objEnd == pos || result[objEnd - 1] != '\\'))
        inString = !inString;
      else if (!inString)
      {
        if (c == '{')
          braceDepth++;
        else if (c == '}')
        {
          braceDepth--;
          if (braceDepth == 0)
            break;
        }
      }
      objEnd++;
    }

    if (braceDepth != 0 || objEnd >= result.size())
      break; // malformed JSON, bail

    // Extract the complete object string
    std::string obj = result.substr(pos, objEnd - pos + 1);

    // Parse fields from this single object
    uint64_t id = extractJsonNumber(obj, "id");
    std::string symbol = extractJsonString(obj, "symbol");
    std::string name = extractJsonString(obj, "name");
    uint64_t decimals = extractJsonNumber(obj, "decimals");
    uint64_t backingModel = extractJsonNumber(obj, "backingModel");
    uint64_t backingRatio = extractJsonNumber(obj, "backingRatio");
    uint64_t lockedCCXAmount = extractJsonNumber(obj, "lockedCCXAmount");

    // Parse provenance if available
    std::string sourceChain = extractJsonString(obj, "sourceChain");
    std::string sourceAsset = extractJsonString(obj, "sourceAsset");
    bool verified = extractJsonString(obj, "verified") == "true";

    // Only cache if we got a valid ID and symbol
    if (id > 0 && !symbol.empty())
    {
      TokenInfoCache info;
      info.symbol = symbol;
      info.name = name;
      info.decimals = static_cast<uint8_t>(decimals > 0 ? decimals : 6);
      info.backingModel = static_cast<uint8_t>(backingModel);
      info.backingRatio = backingRatio;
      info.lockedCCXAmount = lockedCCXAmount;
      info.verified = verified;
      info.sourceChain = sourceChain;
      info.sourceAsset = sourceAsset;
      g_tokenCache[id] = info;
    }

    pos = objEnd + 1; // move past this object for the next iteration
  }

  std::cout << "Loaded " << g_tokenCache.size() << " token(s)" << std::endl;
}

TokenInfoCache getTokenInfo(uint64_t tokenId)
{
  auto it = g_tokenCache.find(tokenId);
  if (it != g_tokenCache.end())
    return it->second;

  TokenInfoCache unknown;
  unknown.symbol = "Token #" + std::to_string(tokenId);
  unknown.name = "Unknown";
  unknown.decimals = 6;
  unknown.backingModel = 0;
  return unknown;
}

std::string getTokenSymbol(uint64_t tokenId)
{
  return getTokenInfo(tokenId).symbol;
}

uint8_t getTokenDecimals(uint64_t tokenId)
{
  return getTokenInfo(tokenId).decimals;
}

std::string getBackingModelName(uint8_t model)
{
  switch (model)
  {
  case 1:
    return "Fully Backed";
  case 2:
    return "Hybrid";
  default:
    return "Unbacked";
  }
}

std::string getTokenProvenance(uint64_t tokenId)
{
  auto info = getTokenInfo(tokenId);
  if (info.sourceChain.empty())
    return "";

  std::string provenance = info.sourceChain;
  if (!info.sourceAsset.empty() && info.sourceAsset != "native")
    provenance += ":" + info.sourceAsset;

  if (info.verified)
    provenance += " (verified)";

  return provenance;
}

std::string formatAmountWithDecimals(uint64_t amount, uint64_t tokenId,
                                     const std::string &sidechainHost, uint16_t sidechainPort)
{
  return formatAmount(amount, getTokenDecimals(tokenId));
}

void clearScreen() { std::cout << "\033[2J\033[1;1H" << std::flush; }

std::string formatHash(const std::string &hash, size_t len)
{
  if (hash.size() <= len)
    return hash;
  return hash.substr(0, len) + "...";
}

std::string getTimestamp()
{
  auto t = std::time(nullptr);
  std::string ts = std::ctime(&t);
  if (!ts.empty() && ts.back() == '\n')
    ts.pop_back();
  return ts;
}

std::string addressToHexPubKey(const std::string &input, cn::Currency &currency)
{
  if (input.size() == 64 && input.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
    return input;

  try
  {
    cn::AccountPublicAddress addr;
    if (currency.parseAccountAddressString(input, addr))
      return common::podToHex(addr.spendPublicKey);
  }
  catch (...)
  {
  }

  return input;
}

const std::unordered_map<uint64_t, TokenInfoCache> &getTokenCache()
{
  return g_tokenCache;
}