// DexMenus.cpp — DEX menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "DexMenus.h"
#include "JsonHelpers.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include <iostream>
#include <sstream>

void dexOrderBookMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== DEX Order Book ===" << std::endl
            << std::endl;

  uint64_t baseTokenId, quoteTokenId;
  std::cout << "Base token ID: ";
  std::cin >> baseTokenId;
  std::cout << "Quote token ID (0 = SCCX): ";
  std::cin >> quoteTokenId;
  std::cin.ignore();

  // Show token info for context
  std::string baseSymbol = getTokenSymbol(baseTokenId);
  uint8_t baseDecimals = getTokenDecimals(baseTokenId);
  std::string quoteSymbol = getTokenSymbol(quoteTokenId);
  uint8_t quoteDecimals = getTokenDecimals(quoteTokenId);
  std::string baseProvenance = getTokenProvenance(baseTokenId);
  std::string quoteProvenance = getTokenProvenance(quoteTokenId);

  std::cout << std::endl
            << "Pair: " << baseSymbol;
  if (!baseProvenance.empty())
    std::cout << " [" << baseProvenance << "]";
  std::cout << " / " << quoteSymbol;
  if (!quoteProvenance.empty())
    std::cout << " [" << quoteProvenance << "]";
  std::cout << std::endl
            << std::endl;

  std::ostringstream params;
  params << R"({"baseTokenId":)" << baseTokenId
         << R"(,"quoteTokenId":)" << quoteTokenId << "}";

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "dex_getOrders", params.str());

  std::cout << "Sells (" << baseSymbol << " > " << quoteSymbol << "):" << std::endl;
  size_t pos = 0;
  bool foundSells = false;
  while ((pos = result.find("\"type\":\"sell\"", pos)) != std::string::npos)
  {
    size_t objStart = result.rfind("{", pos);
    if (objStart == std::string::npos)
    {
      pos++;
      continue;
    }
    size_t objEnd = result.find("}", pos);
    if (objEnd == std::string::npos)
    {
      pos++;
      continue;
    }
    std::string obj = result.substr(objStart, objEnd - objStart + 1);

    uint64_t id = extractJsonNumber(obj, "id");
    uint64_t amount = extractJsonNumber(obj, "amount");
    uint64_t price = extractJsonNumber(obj, "price");
    std::string owner = extractJsonString(obj, "owner");

    std::cout << "  #" << id << " | Price: " << formatAmount(price, quoteDecimals)
              << " " << quoteSymbol << " | Amount: " << formatAmount(amount, baseDecimals)
              << " " << baseSymbol << " | Owner: " << formatHash(owner) << std::endl;
    foundSells = true;
    pos = objEnd + 1;
  }
  if (!foundSells)
    std::cout << "  No sell orders." << std::endl;

  std::cout << std::endl
            << "Buys (" << baseSymbol << " ← " << quoteSymbol << "):" << std::endl;
  pos = 0;
  bool foundBuys = false;
  while ((pos = result.find("\"type\":\"buy\"", pos)) != std::string::npos)
  {
    size_t objStart = result.rfind("{", pos);
    if (objStart == std::string::npos)
    {
      pos++;
      continue;
    }
    size_t objEnd = result.find("}", pos);
    if (objEnd == std::string::npos)
    {
      pos++;
      continue;
    }
    std::string obj = result.substr(objStart, objEnd - objStart + 1);

    uint64_t id = extractJsonNumber(obj, "id");
    uint64_t amount = extractJsonNumber(obj, "amount");
    uint64_t price = extractJsonNumber(obj, "price");
    std::string owner = extractJsonString(obj, "owner");

    std::cout << "  #" << id << " | Price: " << formatAmount(price, quoteDecimals)
              << " " << quoteSymbol << " | Amount: " << formatAmount(amount, baseDecimals)
              << " " << baseSymbol << " | Owner: " << formatHash(owner) << std::endl;
    foundBuys = true;
    pos = objEnd + 1;
  }
  if (!foundBuys)
    std::cout << "  No buy orders." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void dexSubmitOrderMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Place DEX Order ===" << std::endl
            << std::endl;

  // Show available tokens for reference
  std::cout << "Available tokens:" << std::endl;
  const auto &cache = getTokenCache();
  std::cout << "  0 = SCCX (native gas token)" << std::endl;
  for (const auto &entry : cache)
  {
    uint64_t id = entry.first;
    const TokenInfoCache &info = entry.second;
    if (id == 0)
      continue;
    std::cout << "  " << id << " = " << info.symbol << " (" << info.name << ")";
    if (!info.sourceChain.empty())
    {
      std::cout << " [" << info.sourceChain;
      if (info.verified)
        std::cout << " · verified";
      std::cout << "]";
    }
    std::cout << std::endl;
  }
  std::cout << std::endl;

  std::string type;
  uint64_t baseTokenId, quoteTokenId, amount, price;

  std::cout << "Order type (buy/sell): ";
  std::getline(std::cin, type);
  if (type != "buy" && type != "sell")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Base token ID (token to buy/sell): ";
  std::cin >> baseTokenId;
  std::cout << "Quote token ID (0 = SCCX): ";
  std::cin >> quoteTokenId;
  std::cout << "Amount: ";
  std::cin >> amount;
  std::cout << "Price (in quote token): ";
  std::cin >> price;
  std::cin.ignore();

  // Show order summary with token symbols and decimals
  std::string baseSymbol = getTokenSymbol(baseTokenId);
  uint8_t baseDecimals = getTokenDecimals(baseTokenId);
  std::string quoteSymbol = getTokenSymbol(quoteTokenId);
  uint8_t quoteDecimals = getTokenDecimals(quoteTokenId);

  uint64_t totalCost = amount * price;
  std::cout << std::endl
            << "Order Summary:" << std::endl;
  std::cout << "  " << (type == "buy" ? "Buy" : "Sell") << " "
            << formatAmount(amount, baseDecimals) << " " << baseSymbol
            << " @ " << formatAmount(price, quoteDecimals) << " " << quoteSymbol << std::endl;
  std::cout << "  Total: " << formatAmount(totalCost, quoteDecimals) << " " << quoteSymbol << std::endl
            << std::endl;

  std::cout << "Confirm? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{"
         << R"("type":")" << type << R"(")"
         << R"(,"owner":")" << spendPubHex << R"(")"
         << R"(,"baseTokenId":)" << baseTokenId
         << R"(,"quoteTokenId":)" << quoteTokenId
         << R"(,"amount":)" << amount
         << R"(,"price":)" << price
         << "}";

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "dex_submitOrder", params.str());
  std::cout << std::endl
            << "Result: " << result << std::endl;

  if (result.find("true") != std::string::npos)
    std::cout << "Order placed successfully!" << std::endl;
  else
    std::cout << "Order failed. Check your escrow balance." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void dexTradeHistoryMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== DEX Trade History ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "dex_getAllTrades", R"({"limit":20})");

  size_t pos = 0;
  int count = 0;
  while ((pos = result.find("\"id\":", pos)) != std::string::npos)
  {
    size_t objStart = result.rfind("{", pos);
    if (objStart == std::string::npos)
    {
      pos++;
      continue;
    }
    size_t objEnd = result.find("}", pos);
    if (objEnd == std::string::npos)
    {
      pos++;
      continue;
    }
    std::string obj = result.substr(objStart, objEnd - objStart + 1);

    uint64_t id = extractJsonNumber(obj, "id");
    uint64_t amount = extractJsonNumber(obj, "amount");
    uint64_t price = extractJsonNumber(obj, "price");
    uint64_t baseTokenId = extractJsonNumber(obj, "baseTokenId");
    uint64_t quoteTokenId = extractJsonNumber(obj, "quoteTokenId");
    std::string buyer = extractJsonString(obj, "buyer");
    std::string seller = extractJsonString(obj, "seller");
    std::string settled = extractJsonString(obj, "settled");
    uint64_t timestamp = extractJsonNumber(obj, "timestamp");

    std::string baseSymbol = getTokenSymbol(baseTokenId);
    uint8_t baseDecimals = getTokenDecimals(baseTokenId);
    std::string quoteSymbol = getTokenSymbol(quoteTokenId);
    uint8_t quoteDecimals = getTokenDecimals(quoteTokenId);

    std::cout << "  Trade #" << id;
    std::cout << " | Amount: " << formatAmount(amount, baseDecimals) << " " << baseSymbol;
    std::cout << " | Price: " << formatAmount(price, quoteDecimals) << " " << quoteSymbol;
    std::cout << " | Pair: " << baseSymbol << "/" << quoteSymbol;
    std::cout << " | " << (settled == "true" ? "Settled" : "Pending");
    if (timestamp > 0)
    {
      time_t t = static_cast<time_t>(timestamp);
      std::string ts = std::ctime(&t);
      if (!ts.empty() && ts.back() == '\n')
        ts.pop_back();
      std::cout << " | " << ts;
    }
    std::cout << std::endl;
    count++;
    pos = objEnd + 1;
  }

  if (count == 0)
    std::cout << "  No trades yet." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void dexCancelOrderMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Cancel DEX Order ===" << std::endl
            << std::endl;

  uint64_t orderId;
  std::cout << "Order ID to cancel: ";
  std::cin >> orderId;
  std::cin.ignore();

  std::ostringstream params;
  params << R"({"orderId":)" << orderId
         << R"(,"owner":")" << spendPubHex << R"("})";

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "dex_cancelOrder", params.str());
  std::cout << std::endl
            << "Result: " << result << std::endl;

  if (result.find("true") != std::string::npos)
    std::cout << "Order cancelled. Funds returned to escrow." << std::endl;
  else
    std::cout << "Cancellation failed. Check the order ID and owner." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void dexDepositAddressMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== DEX Deposit Address ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "dex_deposit", "{}");
  std::string address = extractJsonString(result, "dexAddress");

  if (address.empty())
  {
    std::cout << "Could not retrieve DEX address. Is the DEX running?" << std::endl;
  }
  else
  {
    std::cout << "Send tokens to this address to fund your DEX account:" << std::endl;
    std::cout << "  " << address << std::endl;
    std::cout << std::endl;
    std::cout << "Use S4 (Send Token) to transfer tokens to this address." << std::endl;
    std::cout << "Then check your escrow balance with D5." << std::endl;
    std::cout << std::endl;
    std::cout << "For CCX-backed tokens, send CCX to this address on the main chain" << std::endl;
    std::cout << "to trigger the bridge deposit and mint backed tokens." << std::endl;
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void dexEscrowBalanceMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== DEX Escrow Balance ===" << std::endl
            << std::endl;

  auto fetchEscrow = [&](uint64_t tokenId) -> uint64_t
  {
    std::ostringstream params;
    params << R"({"owner":")" << spendPubHex << R"(","tokenId":)" << tokenId << "}";
    std::string result = sidechainCall(cfg.dexHost, cfg.dexPort, "dex_getEscrowBalance", params.str());
    uint64_t balance = 0;
    try
    {
      balance = std::stoull(result);
    }
    catch (...)
    {
    }
    return balance;
  };

  // SCCX escrow
  uint64_t sccxEscrow = fetchEscrow(0);
  std::cout << "SCCX Escrow: " << formatAmount(sccxEscrow, 6) << std::endl;

  // All known tokens
  const auto &cache = getTokenCache();
  if (cache.size() <= 1)
  {
    std::cout << "  (No other tokens. Use S2 to refresh.)" << std::endl;
  }
  else
  {
    std::cout << std::endl;
    for (const auto &entry : cache)
    {
      uint64_t tokenId = entry.first;
      const TokenInfoCache &info = entry.second;
      if (tokenId == 0)
        continue;

      uint64_t escrow = fetchEscrow(tokenId);
      std::string symbol = getTokenSymbol(tokenId);
      uint8_t decimals = getTokenDecimals(tokenId);

      std::cout << symbol;
      if (!info.name.empty() && info.name != symbol)
        std::cout << " (" << info.name << ")";
      std::cout << " Escrow: " << formatAmount(escrow, decimals) << std::endl;

      // Show backing and provenance info
      if (info.backingModel > 0)
      {
        std::cout << "    " << getBackingModelName(info.backingModel);
        if (info.backingRatio > 0)
          std::cout << " · " << info.backingRatio << "% backed";
        if (info.lockedCCXAmount > 0)
          std::cout << " · " << formatAmount(info.lockedCCXAmount, 6) << " CCX locked";
        std::cout << std::endl;
      }

      if (!info.sourceChain.empty())
      {
        std::cout << "    Source: " << info.sourceChain;
        if (info.verified)
          std::cout << " [verified]";
        std::cout << std::endl;
      }

      std::cout << std::endl;
    }
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}