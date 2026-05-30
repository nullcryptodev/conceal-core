// DexMenus.h — DEX menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Config.h"
#include <string>

void dexOrderBookMenu(const Config &cfg);
void dexSubmitOrderMenu(const Config &cfg, const std::string &spendPubHex);
void dexTradeHistoryMenu(const Config &cfg);
void dexCancelOrderMenu(const Config &cfg, const std::string &spendPubHex);
void dexDepositAddressMenu(const Config &cfg);
void dexEscrowBalanceMenu(const Config &cfg, const std::string &spendPubHex);