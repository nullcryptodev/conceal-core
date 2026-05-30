// SidechainMenus.h — sidechain and AMM menu functions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Config.h"
#include <string>

namespace cn
{
  class Currency;
}

// Sidechain
void sidechainTokensMenu(const Config &cfg);
void sidechainCreateTokenMenu(const Config &cfg, const std::string &spendPubHex);
void sidechainTransferMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency);
void sidechainBalanceMenu(const Config &cfg, const std::string &spendPubHex);
void sidechainStatusMenu(const Config &cfg);
void sidechainQuickCreateMenu(const Config &cfg, const std::string &spendPubHex);
void sidechainTxHistoryMenu();

// AMM
void ammPoolsMenu(const Config &cfg);
void ammCreatePoolMenu(const Config &cfg, const std::string &spendPubHex);
void ammAddLiquidityMenu(const Config &cfg, const std::string &spendPubHex);
void ammRemoveLiquidityMenu(const Config &cfg, const std::string &spendPubHex);
void ammSwapMenu(const Config &cfg, const std::string &spendPubHex);
void ammPositionsMenu(const Config &cfg, const std::string &spendPubHex);

// Vesting
void vestingCreateMenu(const Config &cfg, const std::string &spendPubHex, cn::Currency &currency);
void vestingListMenu(const Config &cfg, const std::string &spendPubHex);
void vestingRevokeMenu(const Config &cfg, const std::string &spendPubHex);

// Reward Pools
void rewardPoolCreateMenu(const Config &cfg, const std::string &spendPubHex);
void rewardPoolStakeMenu(const Config &cfg, const std::string &spendPubHex);
void rewardPoolUnstakeMenu(const Config &cfg, const std::string &spendPubHex);
void rewardPoolClaimMenu(const Config &cfg, const std::string &spendPubHex);
void rewardPoolListMenu(const Config &cfg);