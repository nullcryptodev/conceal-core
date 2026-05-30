// SidechainMenus.cpp — sidechain, AMM, vesting, and reward pool menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainMenus.h"
#include "TxHistory.h"
#include "JsonHelpers.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include <iostream>
#include <sstream>

// Sidechain token menus
void sidechainTokensMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Sidechain Tokens ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getStatus", "{}");
  uint64_t height = extractJsonNumber(result, "height");
  uint64_t tokenCount = extractJsonNumber(result, "tokenCount");
  uint64_t pendingTx = extractJsonNumber(result, "pendingTransactions");

  std::cout << "Network Summary:" << std::endl;
  std::cout << "  Height: " << height << std::endl;
  std::cout << "  Tokens: " << tokenCount << std::endl;
  std::cout << "  Pending: " << pendingTx << std::endl;
  std::cout << std::endl;

  result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokens", "{}");

  std::cout << "Tokens:" << std::endl;
  size_t arrayStart = result.find("\"result\":[");
  if (arrayStart == std::string::npos)
  {
    std::cout << "  None found." << std::endl;
  }
  else
  {
    arrayStart += 10;
    std::string arrayContent = result.substr(arrayStart);

    size_t pos = 0;
    int count = 0;
    while ((pos = arrayContent.find("\"id\":", pos)) != std::string::npos)
    {
      uint64_t id = extractJsonNumber(arrayContent.substr(pos), "id");
      std::string name = extractJsonString(arrayContent.substr(pos), "name");
      std::string symbol = extractJsonString(arrayContent.substr(pos), "symbol");
      uint64_t supply = extractJsonNumber(arrayContent.substr(pos), "totalSupply");
      uint64_t maxSupply = extractJsonNumber(arrayContent.substr(pos), "maxSupply");
      uint64_t decimals = extractJsonNumber(arrayContent.substr(pos), "decimals");
      uint64_t model = extractJsonNumber(arrayContent.substr(pos), "backingModel");
      uint64_t backingRatio = extractJsonNumber(arrayContent.substr(pos), "backingRatio");
      uint64_t lockedCCX = extractJsonNumber(arrayContent.substr(pos), "lockedCCXAmount");
      std::string sourceChain = extractJsonString(arrayContent.substr(pos), "sourceChain");
      bool verified = extractJsonString(arrayContent.substr(pos), "verified") == "true";

      if (!name.empty() && !symbol.empty())
      {
        std::string modelName = getBackingModelName(static_cast<uint8_t>(model));
        std::cout << "  " << symbol << " (" << name << ")" << std::endl;
        std::cout << "    ID: " << id << " | Supply: " << formatAmount(supply, decimals)
                  << " | Max: " << formatAmount(maxSupply, decimals)
                  << " | Decimals: " << decimals << " | " << modelName << std::endl;

        if (model > 0)
        {
          std::cout << "    Backing Ratio: " << backingRatio << "%";
          if (lockedCCX > 0)
            std::cout << " | Locked CCX: " << formatAmount(lockedCCX, 6);
          std::cout << std::endl;
        }

        if (!sourceChain.empty())
        {
          std::cout << "    Source: " << sourceChain;
          if (verified)
            std::cout << " [verified]";
          std::cout << std::endl;
        }
      }
      pos++;
      count++;
    }
    if (count == 0)
      std::cout << "  None found." << std::endl;
  }

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainCreateTokenMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Create Token ===" << std::endl
            << std::endl;
  std::cout << "Backing models:" << std::endl;
  std::cout << "  0 = Unbacked" << std::endl;
  std::cout << "  1 = Fully Backed" << std::endl;
  std::cout << "  2 = Hybrid" << std::endl;
  std::cout << std::endl;

  std::string name, symbol;
  uint64_t initialSupply;
  int backingModel;
  int decimals = 6;
  uint64_t backingRatio = 100;
  uint64_t lockedCCXAmount = 0;

  std::cout << "Token name: ";
  std::getline(std::cin, name);
  if (name.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Token symbol: ";
  std::getline(std::cin, symbol);
  if (symbol.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::cout << "Initial supply (atomic units): ";
  std::cin >> initialSupply;
  if (initialSupply == 0)
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.ignore();
    std::cin.get();
    return;
  }

  std::cout << "Decimals (default 6): ";
  std::cin >> decimals;
  if (decimals > 18)
    decimals = 6;

  std::cout << "Backing model (0=Unbacked, 1=Fully Backed, 2=Hybrid): ";
  std::cin >> backingModel;
  std::cin.ignore();

  if (backingModel < 0 || backingModel > 2)
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  if (backingModel == 1 || backingModel == 2)
  {
    std::cout << std::endl
              << "Backing configuration:" << std::endl;
    if (backingModel == 2)
    {
      std::cout << "Backing ratio (% - default 100): ";
      std::string ratioStr;
      std::getline(std::cin, ratioStr);
      if (!ratioStr.empty())
        backingRatio = std::stoull(ratioStr);
    }
    std::cout << "Locked CCX amount (atomic units, 0 to skip): ";
    std::cin >> lockedCCXAmount;
    std::cin.ignore();
    if (lockedCCXAmount == 0)
    {
      std::cout << "Warning: Creating a backed token without locked CCX." << std::endl;
      std::cout << "Continue? (y/n): ";
      std::string cont;
      std::getline(std::cin, cont);
      if (cont != "y" && cont != "Y")
      {
        std::cout << "Cancelled." << std::endl;
        std::cin.get();
        return;
      }
    }
  }

  std::cout << std::endl
            << "Confirm:" << std::endl;
  std::cout << "  Name: " << name << std::endl;
  std::cout << "  Symbol: " << symbol << std::endl;
  std::cout << "  Supply: " << initialSupply << std::endl;
  std::cout << "  Decimals: " << decimals << std::endl;
  std::cout << "  Model: " << getBackingModelName(static_cast<uint8_t>(backingModel)) << std::endl;
  if (backingModel > 0)
  {
    std::cout << "  Backing Ratio: " << backingRatio << "%" << std::endl;
    std::cout << "  Locked CCX: " << formatAmount(lockedCCXAmount, 6) << std::endl;
  }
  std::cout << "Create? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::string nameHex = common::toHex(common::asBinaryArray(name));
  std::string symbolHex = common::toHex(common::asBinaryArray(symbol));

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\",\"nameHex\":\"" << nameHex
         << "\",\"symbolHex\":\"" << symbolHex << "\",\"initialSupply\":" << initialSupply
         << ",\"backingModel\":" << backingModel << ",\"decimals\":" << decimals;
  if (backingModel > 0)
    params << ",\"backingRatio\":" << backingRatio << ",\"lockedCCXAmount\":" << lockedCCXAmount;
  params << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createToken", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
  {
    std::cout << "Token " << symbol << " created!" << std::endl;
    loadTokenCache(cfg.sidechainHost, cfg.sidechainPort);
  }
  else
    std::cout << "Failed to create token." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainTransferMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency)
{
  clearScreen();
  std::cout << "=== Send Tokens ===" << std::endl
            << std::endl;

  std::string to;
  uint64_t amount, tokenId;

  std::cout << "Send to (address or public key): ";
  std::getline(std::cin, to);
  if (to.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::string toPubHex = addressToHexPubKey(to, currency);

  std::cout << "Amount (atomic units): ";
  std::cin >> amount;
  if (amount == 0)
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.ignore();
    std::cin.get();
    return;
  }

  std::cout << std::endl
            << "Available tokens:" << std::endl;
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
        std::cout << " . verified";
      std::cout << "]";
    }
    if (info.backingModel > 0)
      std::cout << " . " << getBackingModelName(info.backingModel);
    std::cout << std::endl;
  }

  std::cout << std::endl
            << "Token to send: ";
  std::cin >> tokenId;
  std::cin.ignore();

  std::string tokenSymbol = getTokenSymbol(tokenId);
  uint8_t decimals = getTokenDecimals(tokenId);
  std::cout << "Sending " << formatAmount(amount, decimals) << " " << tokenSymbol << "..." << std::endl;

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\",\"to\":\"" << toPubHex
         << "\",\"amount\":" << amount << ",\"tokenId\":" << tokenId << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "transfer", params.str());
  std::string txHash;
  size_t hashStart = result.find("\"result\":\"");
  if (hashStart != std::string::npos)
  {
    hashStart += 10;
    size_t hashEnd = result.find("\"", hashStart);
    if (hashEnd != std::string::npos)
      txHash = result.substr(hashStart, hashEnd - hashStart);
  }

  if (!txHash.empty() && txHash != "false")
  {
    std::cout << "Sent " << formatAmount(amount, decimals) << " " << tokenSymbol << "!" << std::endl;
    std::cout << "Transaction: " << formatHash(txHash) << std::endl;
    TxEntry entry;
    entry.type = "Transfer";
    entry.tokenSymbol = tokenSymbol;
    entry.tokenId = tokenId;
    entry.amount = amount;
    entry.to = toPubHex;
    entry.timestamp = getTimestamp();
    entry.txHash = txHash;
    addTxToHistory(entry);
  }
  else
    std::cout << "Transfer failed. Check your balance and fee amount." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainBalanceMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Balances ===" << std::endl
            << std::endl;

  std::ostringstream nativeParams;
  nativeParams << "{\"address\":\"" << spendPubHex << "\",\"tokenId\":0}";
  std::string nativeBalJson = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokenBalance", nativeParams.str());
  uint64_t sccxBalance = extractJsonNumber(nativeBalJson, "result");
  std::cout << "SCCX: " << formatAmount(sccxBalance, 6) << std::endl
            << std::endl;

  const auto &cache = getTokenCache();
  bool shownAny = false;
  for (const auto &entry : cache)
  {
    uint64_t id = entry.first;
    const TokenInfoCache &info = entry.second;
    if (id == 0)
      continue;

    std::ostringstream tokParams;
    tokParams << "{\"address\":\"" << spendPubHex << "\",\"tokenId\":" << id << "}";
    std::string tokBalJson = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokenBalance", tokParams.str());
    uint64_t tokBalance = extractJsonNumber(tokBalJson, "result");

    std::cout << info.symbol << " (" << info.name << ")" << std::endl;
    std::cout << "  Balance: " << formatAmount(tokBalance, info.decimals)
              << " | " << getBackingModelName(info.backingModel) << std::endl;
    if (!info.sourceChain.empty())
    {
      std::cout << "  Source: " << info.sourceChain;
      if (info.verified)
        std::cout << " [verified]";
      std::cout << std::endl;
    }
    if (info.backingModel > 0)
    {
      std::cout << "  Backing: " << info.backingRatio << "%";
      if (info.lockedCCXAmount > 0)
        std::cout << " | Locked: " << formatAmount(info.lockedCCXAmount, 6) << " CCX";
      std::cout << std::endl;
    }
    std::cout << std::endl;
    shownAny = true;
  }
  if (!shownAny)
    std::cout << "No tokens found." << std::endl;

  std::cout << "Press enter to return..." << std::endl;
  std::cin.get();
}

void sidechainStatusMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Network Status ===" << std::endl
            << std::endl;
  std::string status = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getStatus", "{}");
  if (status.empty() || status == "{}")
  {
    std::cout << "Not connected." << std::endl;
  }
  else
  {
    uint64_t height = extractJsonNumber(status, "height");
    uint64_t tokenCount = extractJsonNumber(status, "tokenCount");
    uint64_t pendingTx = extractJsonNumber(status, "pendingTransactions");
    std::cout << "Height: " << height << std::endl;
    std::cout << "Tokens: " << tokenCount << std::endl;
    std::cout << "Pending transactions: " << pendingTx << std::endl;
    std::cout << std::endl;
    std::cout << "Connected to: " << cfg.sidechainHost << ":" << cfg.sidechainPort << std::endl;
    std::cout << "Mode: " << (cfg.sidechainTestnet ? "Testnet" : "Mainnet") << std::endl;
  }
  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainQuickCreateMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Quick Create Token ===" << std::endl
            << std::endl;

  std::string name = "test" + std::to_string(time(nullptr) % 10000);
  std::string symbol = "T" + std::to_string(time(nullptr) % 1000);
  uint64_t initialSupply = 1000;
  int backingModel = 0;
  uint8_t decimals = 6;

  std::cout << "Creating token:" << std::endl;
  std::cout << "  Name: " << name << std::endl;
  std::cout << "  Symbol: " << symbol << std::endl;
  std::cout << "  Supply: " << initialSupply << std::endl;
  std::cout << "  Decimals: " << static_cast<int>(decimals) << std::endl;
  std::cout << "  Model: " << getBackingModelName(static_cast<uint8_t>(backingModel)) << std::endl
            << std::endl;

  std::string nameHex = common::toHex(common::asBinaryArray(name));
  std::string symbolHex = common::toHex(common::asBinaryArray(symbol));

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\",\"nameHex\":\"" << nameHex
         << "\",\"symbolHex\":\"" << symbolHex << "\",\"initialSupply\":" << initialSupply
         << ",\"backingModel\":" << backingModel << ",\"decimals\":" << static_cast<int>(decimals) << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createToken", params.str());
  std::string txHash;
  size_t hashStart = result.find("\"result\":\"");
  if (hashStart != std::string::npos)
  {
    hashStart += 10;
    size_t hashEnd = result.find("\"", hashStart);
    if (hashEnd != std::string::npos)
      txHash = result.substr(hashStart, hashEnd - hashStart);
  }

  if (!txHash.empty() && txHash != "false")
  {
    std::cout << "Token " << symbol << " created!" << std::endl;
    TxEntry entry;
    entry.type = "CreateToken";
    entry.tokenSymbol = symbol;
    entry.tokenId = 0;
    entry.amount = initialSupply;
    entry.to = spendPubHex;
    entry.timestamp = getTimestamp();
    entry.txHash = txHash;
    addTxToHistory(entry);
    loadTokenCache(cfg.sidechainHost, cfg.sidechainPort);
  }
  else
    std::cout << "Failed to create token." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void sidechainTxHistoryMenu()
{
  clearScreen();
  std::cout << "=== Transaction History ===" << std::endl
            << std::endl;
  if (txHistory.empty())
  {
    std::cout << "No transactions yet." << std::endl;
  }
  else
  {
    for (size_t i = 0; i < txHistory.size(); ++i)
    {
      const auto &tx = txHistory[i];
      std::cout << (i + 1) << ". " << tx.type << " - " << tx.tokenSymbol;
      std::cout << " | Amount: " << formatAmount(tx.amount);
      std::cout << " | Hash: " << formatHash(tx.txHash);
      std::cout << " | " << tx.timestamp;
      std::cout << std::endl;
    }
  }
  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

// AMM Menus
void ammPoolsMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== AMM Liquidity Pools ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_getPools", "{}");

  size_t pos = 0;
  int count = 0;
  while ((pos = result.find("\"poolId\":", pos)) != std::string::npos)
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

    uint64_t poolId = extractJsonNumber(obj, "poolId");
    uint64_t tokenIdA = extractJsonNumber(obj, "tokenIdA");
    uint64_t tokenIdB = extractJsonNumber(obj, "tokenIdB");
    uint64_t reserveA = extractJsonNumber(obj, "reserveA");
    uint64_t reserveB = extractJsonNumber(obj, "reserveB");
    uint64_t totalLiquidity = extractJsonNumber(obj, "totalLiquidity");
    uint64_t feeBps = extractJsonNumber(obj, "feeBasisPoints");

    std::string symbolA = getTokenSymbol(tokenIdA);
    std::string symbolB = getTokenSymbol(tokenIdB);
    uint8_t decA = getTokenDecimals(tokenIdA);
    uint8_t decB = getTokenDecimals(tokenIdB);

    std::cout << "Pool #" << poolId << ": " << symbolA << " / " << symbolB << std::endl;
    std::cout << "  Reserve " << symbolA << ": " << formatAmount(reserveA, decA) << std::endl;
    std::cout << "  Reserve " << symbolB << ": " << formatAmount(reserveB, decB) << std::endl;
    std::cout << "  Total Liquidity: " << totalLiquidity << std::endl;
    std::cout << "  Fee: " << (feeBps / 100.0) << "%" << std::endl;

    if (reserveA > 0 && reserveB > 0)
    {
      uint64_t price = (reserveB * 1000000) / reserveA;
      std::cout << "  Price: 1 " << symbolA << " = " << formatAmount(price, decB) << " " << symbolB << std::endl;
    }
    std::cout << std::endl;
    count++;
    pos = objEnd + 1;
  }

  if (count == 0)
    std::cout << "No pools yet. Create one with A1." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void ammCreatePoolMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Create AMM Pool ===" << std::endl
            << std::endl;

  uint64_t tokenIdA, tokenIdB, amountA, amountB;
  uint16_t feeBps = 30;

  std::cout << "Token A ID: ";
  std::cin >> tokenIdA;
  std::cout << "Token B ID (0 = SCCX): ";
  std::cin >> tokenIdB;
  std::cout << "Amount A: ";
  std::cin >> amountA;
  std::cout << "Amount B: ";
  std::cin >> amountB;
  std::cout << "Fee in basis points (default 30 = 0.3%): ";
  std::cin >> feeBps;
  std::cin.ignore();

  std::string symbolA = getTokenSymbol(tokenIdA);
  std::string symbolB = getTokenSymbol(tokenIdB);
  uint8_t decA = getTokenDecimals(tokenIdA);
  uint8_t decB = getTokenDecimals(tokenIdB);

  std::cout << std::endl
            << "Creating pool: " << symbolA << " / " << symbolB << std::endl;
  std::cout << "  " << formatAmount(amountA, decA) << " " << symbolA << std::endl;
  std::cout << "  " << formatAmount(amountB, decB) << " " << symbolB << std::endl;
  std::cout << "  Fee: " << (feeBps / 100.0) << "%" << std::endl;
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
  params << "{\"tokenIdA\":" << tokenIdA
         << ",\"tokenIdB\":" << tokenIdB
         << ",\"amountA\":" << amountA
         << ",\"amountB\":" << amountB
         << ",\"creator\":\"" << spendPubHex << "\""
         << ",\"feeBasisPoints\":" << feeBps << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_createPool", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Pool created!" << std::endl;
  else
    std::cout << "Pool creation failed. Check your balances." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void ammAddLiquidityMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Add Liquidity ===" << std::endl
            << std::endl;

  uint64_t poolId, amountA, amountB;
  std::cout << "Pool ID: ";
  std::cin >> poolId;
  std::cout << "Amount A: ";
  std::cin >> amountA;
  std::cout << "Amount B: ";
  std::cin >> amountB;
  std::cin.ignore();

  std::cout << "Confirm add liquidity to pool #" << poolId << "? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"poolId\":" << poolId
         << ",\"amountA\":" << amountA
         << ",\"amountB\":" << amountB
         << ",\"provider\":\"" << spendPubHex << "\"}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_addLiquidity", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Liquidity added!" << std::endl;
  else
    std::cout << "Failed. Check your balances and pool ratio." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void ammRemoveLiquidityMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Remove Liquidity ===" << std::endl
            << std::endl;

  // Show user's positions first
  std::ostringstream posParams;
  posParams << "{\"owner\":\"" << spendPubHex << "\"}";
  std::string posResult = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_getPositions", posParams.str());

  std::cout << "Your positions:" << std::endl;
  size_t ppos = 0;
  bool found = false;
  while ((ppos = posResult.find("\"positionId\":", ppos)) != std::string::npos)
  {
    size_t objStart = posResult.rfind("{", ppos);
    if (objStart == std::string::npos)
    {
      ppos++;
      continue;
    }
    size_t objEnd = posResult.find("}", ppos);
    if (objEnd == std::string::npos)
    {
      ppos++;
      continue;
    }
    std::string obj = posResult.substr(objStart, objEnd - objStart + 1);

    uint64_t positionId = extractJsonNumber(obj, "positionId");
    uint64_t displayPoolId = extractJsonNumber(obj, "poolId");
    uint64_t liquidity = extractJsonNumber(obj, "liquidity");
    std::cout << "  Position #" << positionId << " | Pool #" << displayPoolId
              << " | LP: " << liquidity << std::endl;
    found = true;
    ppos = objEnd + 1;
  }
  if (!found)
    std::cout << "  No positions found." << std::endl;
  std::cout << std::endl;

  uint64_t positionId;
  std::cout << "Position ID to remove: ";
  std::cin >> positionId;
  std::cin.ignore();

  std::cout << "Remove position #" << positionId << "? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"positionId\":" << positionId << ",\"owner\":\"" << spendPubHex << "\"}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_removeLiquidity", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Liquidity removed!" << std::endl;
  else
    std::cout << "Failed. Check the position ID and owner." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void ammSwapMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== AMM Swap ===" << std::endl
            << std::endl;

  uint64_t poolId, tokenIdIn, amountIn;
  double slippagePercent = 1.0;

  std::cout << "Pool ID: ";
  std::cin >> poolId;
  std::cout << "Token to send (ID): ";
  std::cin >> tokenIdIn;
  std::cout << "Amount to send: ";
  std::cin >> amountIn;
  std::cout << "Slippage tolerance % (default 1.0): ";
  std::cin >> slippagePercent;
  std::cin.ignore();

  // Get quote first
  std::ostringstream quoteParams;
  quoteParams << "{\"poolId\":" << poolId
              << ",\"tokenIdIn\":" << tokenIdIn
              << ",\"amountIn\":" << amountIn << "}";

  std::string quoteResult = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_getQuote", quoteParams.str());
  uint64_t amountOut = extractJsonNumber(quoteResult, "amountOut");

  if (amountOut == 0)
  {
    std::cout << "Could not get quote. Check pool ID and token." << std::endl;
    std::cout << "\nPress enter to return..." << std::endl;
    std::cin.get();
    return;
  }

  uint64_t minAmountOut = static_cast<uint64_t>(amountOut * (100.0 - slippagePercent) / 100.0);
  std::string symbolIn = getTokenSymbol(tokenIdIn);
  uint8_t decIn = getTokenDecimals(tokenIdIn);

  std::cout << std::endl
            << "Quote: " << formatAmount(amountIn, decIn) << " " << symbolIn
            << " -> " << formatAmount(amountOut, 6) << " (minimum " << formatAmount(minAmountOut, 6) << ")" << std::endl;
  std::cout << "Confirm swap? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"poolId\":" << poolId
         << ",\"tokenIdIn\":" << tokenIdIn
         << ",\"amountIn\":" << amountIn
         << ",\"minAmountOut\":" << minAmountOut
         << ",\"from\":\"" << spendPubHex << "\"}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_swap", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Swap complete!" << std::endl;
  else
    std::cout << "Swap failed. Check slippage and balance." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void ammPositionsMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Your AMM Positions ===" << std::endl
            << std::endl;

  std::ostringstream params;
  params << "{\"owner\":\"" << spendPubHex << "\"}";
  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "amm_getPositions", params.str());

  size_t pos = 0;
  int count = 0;
  while ((pos = result.find("\"positionId\":", pos)) != std::string::npos)
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

    uint64_t positionId = extractJsonNumber(obj, "positionId");
    uint64_t displayPoolId = extractJsonNumber(obj, "poolId");
    uint64_t liquidity = extractJsonNumber(obj, "liquidity");

    std::cout << "Position #" << positionId << " | Pool #" << displayPoolId
              << " | LP Tokens: " << liquidity << std::endl;
    count++;
    pos = objEnd + 1;
  }

  if (count == 0)
    std::cout << "No positions. Add liquidity to a pool with A2." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

// Vesting Menus
void vestingCreateMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency)
{
  clearScreen();
  std::cout << "=== Create Vesting Schedule ===" << std::endl
            << std::endl;

  std::string beneficiary;
  uint64_t totalAllocated, tokenId;
  uint64_t cliffDays, vestingDays;
  bool revocable = false;

  std::cout << "Beneficiary (address or public key): ";
  std::getline(std::cin, beneficiary);
  if (beneficiary.empty())
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::string beneficiaryHex = addressToHexPubKey(beneficiary, currency);

  std::cout << "Token ID: ";
  std::cin >> tokenId;
  std::cout << "Total amount to vest: ";
  std::cin >> totalAllocated;
  std::cout << "Cliff period (days before first release): ";
  std::cin >> cliffDays;
  std::cout << "Vesting period (days total): ";
  std::cin >> vestingDays;
  std::cout << "Revocable? (1=yes, 0=no): ";
  std::cin >> revocable;
  std::cin.ignore();

  uint64_t now = static_cast<uint64_t>(std::time(nullptr));
  uint64_t cliffTimestamp = now + (cliffDays * 86400);
  uint64_t vestingEndTimestamp = now + (vestingDays * 86400);

  std::string symbol = getTokenSymbol(tokenId);
  uint8_t decimals = getTokenDecimals(tokenId);

  std::cout << std::endl
            << "Schedule:" << std::endl;
  std::cout << "  Token: " << symbol << std::endl;
  std::cout << "  Amount: " << formatAmount(totalAllocated, decimals) << std::endl;
  std::cout << "  Beneficiary: " << formatHash(beneficiaryHex) << std::endl;
  std::cout << "  Cliff: " << cliffDays << " days" << std::endl;
  std::cout << "  Vesting: " << vestingDays << " days" << std::endl;
  std::cout << "  Revocable: " << (revocable ? "yes" : "no") << std::endl;
  std::cout << "Create? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\""
         << ",\"to\":\"" << beneficiaryHex << "\""
         << ",\"amount\":" << totalAllocated
         << ",\"tokenId\":" << tokenId
         << ",\"cliffTimestamp\":" << cliffTimestamp
         << ",\"vestingEndTimestamp\":" << vestingEndTimestamp
         << ",\"revocable\":" << (revocable ? "true" : "false") << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createVesting", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Vesting schedule created!" << std::endl;
  else
    std::cout << "Failed. Check your balance and parameters." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void vestingListMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Your Vesting Schedules ===" << std::endl
            << std::endl;

  std::ostringstream params;
  params << "{\"owner\":\"" << spendPubHex << "\"}";
  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getVestingSchedules", params.str());

  size_t pos = 0;
  int count = 0;
  while ((pos = result.find("\"scheduleId\":", pos)) != std::string::npos)
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

    uint64_t scheduleId = extractJsonNumber(obj, "scheduleId");
    std::string beneficiaryHex = extractJsonString(obj, "beneficiary");
    uint64_t tokenId = extractJsonNumber(obj, "tokenId");
    uint64_t totalAllocated = extractJsonNumber(obj, "totalAllocated");
    uint64_t releasedAmount = extractJsonNumber(obj, "releasedAmount");
    std::string status = extractJsonString(obj, "status");
    bool revocable = extractJsonString(obj, "revocable") == "true";

    std::string symbol = getTokenSymbol(tokenId);
    uint8_t decimals = getTokenDecimals(tokenId);

    std::cout << "Schedule #" << scheduleId << " | " << symbol << " | " << status << std::endl;
    std::cout << "  Total: " << formatAmount(totalAllocated, decimals)
              << " | Released: " << formatAmount(releasedAmount, decimals) << std::endl;
    std::cout << "  Beneficiary: " << formatHash(beneficiaryHex) << std::endl;
    if (revocable)
      std::cout << "  Revocable" << std::endl;
    std::cout << std::endl;
    count++;
    pos = objEnd + 1;
  }

  if (count == 0)
    std::cout << "No vesting schedules found." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void vestingRevokeMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Revoke Vesting Schedule ===" << std::endl
            << std::endl;

  uint64_t scheduleId;
  std::cout << "Schedule ID to revoke: ";
  std::cin >> scheduleId;
  std::cin.ignore();

  std::cout << "Revoke schedule #" << scheduleId << "? Unvested tokens return to you. (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\",\"scheduleId\":" << scheduleId << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "revokeVesting", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Vesting revoked. Unvested tokens returned." << std::endl;
  else
    std::cout << "Failed. Check that you are the creator and the schedule is revocable." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

// Reward Pool Menus
void rewardPoolCreateMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Create Reward Pool ===" << std::endl
            << std::endl;

  uint64_t tokenId, rewardAmount, endDays;
  uint16_t rewardRateBps;

  std::cout << "Token ID: ";
  std::cin >> tokenId;
  std::cout << "Reward amount to deposit: ";
  std::cin >> rewardAmount;
  std::cout << "Annual rate in basis points (500 = 5%): ";
  std::cin >> rewardRateBps;
  std::cout << "Duration in days (0 = until depleted): ";
  std::cin >> endDays;
  std::cin.ignore();

  uint64_t endTimestamp = 0;
  if (endDays > 0)
    endTimestamp = static_cast<uint64_t>(std::time(nullptr)) + (endDays * 86400);

  std::string symbol = getTokenSymbol(tokenId);
  uint8_t decimals = getTokenDecimals(tokenId);

  std::cout << std::endl
            << "Pool:" << std::endl;
  std::cout << "  Token: " << symbol << std::endl;
  std::cout << "  Rewards: " << formatAmount(rewardAmount, decimals) << std::endl;
  std::cout << "  Rate: " << (rewardRateBps / 100.0) << "% APR" << std::endl;
  std::cout << "  Duration: " << (endDays > 0 ? std::to_string(endDays) + " days" : "until depleted") << std::endl;
  std::cout << "Create? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\""
         << ",\"tokenId\":" << tokenId
         << ",\"rewardAmount\":" << rewardAmount
         << ",\"rewardRateBps\":" << rewardRateBps
         << ",\"endTimestamp\":" << endTimestamp << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "createRewardPool", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Reward pool created!" << std::endl;
  else
    std::cout << "Failed. Check your balance." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void rewardPoolStakeMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Stake in Reward Pool ===" << std::endl
            << std::endl;

  uint64_t poolId, amount;
  std::cout << "Pool ID: ";
  std::cin >> poolId;
  std::cout << "Amount to stake: ";
  std::cin >> amount;
  std::cin.ignore();

  std::cout << "Stake " << amount << " in pool #" << poolId << "? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\""
         << ",\"poolId\":" << poolId
         << ",\"amount\":" << amount << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "stake", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Staked!" << std::endl;
  else
    std::cout << "Failed. Check pool exists and you have the tokens." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void rewardPoolUnstakeMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Unstake from Reward Pool ===" << std::endl
            << std::endl;

  uint64_t entryId;
  std::cout << "Stake entry ID: ";
  std::cin >> entryId;
  std::cin.ignore();

  std::cout << "Unstake entry #" << entryId << "? (y/n): ";
  std::string confirm;
  std::getline(std::cin, confirm);
  if (confirm != "y" && confirm != "Y")
  {
    std::cout << "Cancelled." << std::endl;
    std::cin.get();
    return;
  }

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\",\"entryId\":" << entryId << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "unstake", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Unstaked! Tokens and rewards returned." << std::endl;
  else
    std::cout << "Failed. Check entry ID." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void rewardPoolClaimMenu(const Config &cfg, const std::string &spendPubHex)
{
  clearScreen();
  std::cout << "=== Claim Rewards ===" << std::endl
            << std::endl;

  uint64_t entryId;
  std::cout << "Stake entry ID: ";
  std::cin >> entryId;
  std::cin.ignore();

  std::ostringstream params;
  params << "{\"from\":\"" << spendPubHex << "\",\"entryId\":" << entryId << "}";

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "claimReward", params.str());
  std::cout << std::endl;
  if (result.find("true") != std::string::npos)
    std::cout << "Rewards claimed!" << std::endl;
  else
    std::cout << "Failed. Check entry ID." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}

void rewardPoolListMenu(const Config &cfg)
{
  clearScreen();
  std::cout << "=== Reward Pools ===" << std::endl
            << std::endl;

  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getRewardPools", "{}");

  size_t pos = 0;
  int count = 0;
  while ((pos = result.find("\"poolId\":", pos)) != std::string::npos)
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

    uint64_t poolId = extractJsonNumber(obj, "poolId");
    uint64_t tokenId = extractJsonNumber(obj, "tokenId");
    uint64_t totalRewards = extractJsonNumber(obj, "totalRewards");
    uint64_t remainingRewards = extractJsonNumber(obj, "remainingRewards");
    uint64_t rewardRateBps = extractJsonNumber(obj, "rewardRateBps");
    uint64_t totalStaked = extractJsonNumber(obj, "totalStaked");

    std::string symbol = getTokenSymbol(tokenId);
    uint8_t decimals = getTokenDecimals(tokenId);

    std::cout << "Pool #" << poolId << " | " << symbol << std::endl;
    std::cout << "  Rewards: " << formatAmount(remainingRewards, decimals)
              << " / " << formatAmount(totalRewards, decimals) << std::endl;
    std::cout << "  Rate: " << (rewardRateBps / 100.0) << "% APR" << std::endl;
    std::cout << "  Total Staked: " << formatAmount(totalStaked, decimals) << std::endl;
    std::cout << std::endl;
    count++;
    pos = objEnd + 1;
  }

  if (count == 0)
    std::cout << "No reward pools yet." << std::endl;

  std::cout << "\nPress enter to return..." << std::endl;
  std::cin.get();
}