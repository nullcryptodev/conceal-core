// TxHistory.cpp — transaction history tracking
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "TxHistory.h"
#include "JsonHelpers.h"
#include <sstream>
#include <ctime>

std::vector<TxEntry> txHistory;

std::vector<TxEntry> loadTransactionHistory(const Config &cfg, const std::string &spendPubHex)
{
  std::vector<TxEntry> history;

  std::ostringstream params;
  params << R"({"address":")" << spendPubHex << R"("})";
  std::string result = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTransactions", params.str());

  size_t pos = 0;
  while ((pos = result.find("\"txHash\":\"", pos)) != std::string::npos)
  {
    TxEntry entry;
    entry.txHash = extractJsonString(result.substr(pos), "txHash");
    entry.type = extractJsonString(result.substr(pos), "type");
    entry.tokenId = extractJsonNumber(result.substr(pos), "tokenId");
    entry.amount = extractJsonNumber(result.substr(pos), "amount");
    entry.to = extractJsonString(result.substr(pos), "to");

    if (entry.type == "CreateToken")
      entry.tokenSymbol = "Token Creation";
    else if (entry.tokenId == 0)
      entry.tokenSymbol = "SCCX";
    else
      entry.tokenSymbol = "Token #" + std::to_string(entry.tokenId);

    uint64_t timestamp = extractJsonNumber(result.substr(pos), "timestamp");
    if (timestamp > 0)
    {
      time_t t = static_cast<time_t>(timestamp);
      std::string ts = std::ctime(&t);
      if (!ts.empty() && ts.back() == '\n')
        ts.pop_back();
      entry.timestamp = ts;
    }
    else
    {
      entry.timestamp = "Unknown";
    }

    history.push_back(entry);
    pos++;
  }

  return history;
}

void addTxToHistory(const TxEntry &entry)
{
  txHistory.push_back(entry);
}