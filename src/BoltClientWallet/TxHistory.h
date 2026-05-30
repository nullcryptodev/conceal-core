// TxHistory.h — transaction history tracking
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "Config.h"

struct TxEntry
{
  std::string txHash;
  std::string type;
  std::string tokenSymbol;
  uint64_t tokenId = 0;
  uint64_t amount = 0;
  std::string to;
  std::string timestamp;
};

extern std::vector<TxEntry> txHistory;

std::vector<TxEntry> loadTransactionHistory(const Config &cfg, const std::string &spendPubHex);
void addTxToHistory(const TxEntry &entry);