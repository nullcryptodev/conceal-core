// conceal-bolt – Terminal UI wallet powered by BoltSync + BoltCore + Sidechain
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include "Config.h"
#include "TxHistory.h"
#include "JsonHelpers.h"
#include "SidechainMenus.h"
#include "DexMenus.h"

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltCore/BoltCore.h"

#include "Common/Util.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include <System/Dispatcher.h>
#include "NodeRpcProxy/NodeRpcProxy.h"

using namespace cn;

int main(int argc, char *argv[])
{
  Config cfg;
  if (!parseArgs(argc, argv, cfg))
    return 1;

  crypto::SecretKey viewKey;
  if (!BoltSync::hexToSecretKey(cfg.viewKeyHex, viewKey))
  {
    std::cerr << "Invalid view key" << std::endl;
    return 1;
  }
  crypto::SecretKey spendKey;
  bool hasSpendKey = !cfg.spendKeyHex.empty();
  if (hasSpendKey && !BoltSync::hexToSecretKey(cfg.spendKeyHex, spendKey))
  {
    std::cerr << "Invalid spend key" << std::endl;
    return 1;
  }

  crypto::PublicKey viewPub, spendPub;
  crypto::secret_key_to_public_key(viewKey, viewPub);
  if (hasSpendKey)
    crypto::secret_key_to_public_key(spendKey, spendPub);

  std::string spendPubHex = common::podToHex(spendPub);

  txHistory = loadTransactionHistory(cfg, spendPubHex);

  logging::LoggerManager logManager;
  logging::ConsoleLogger logger;
  Currency currency = CurrencyBuilder(logManager).currency();
  std::string addressStr = currency.accountAddressAsString({spendPub, viewPub});

  std::cout << "\nConceal Bolt Wallet" << std::endl;
  std::cout << "Address: " << addressStr << std::endl;

  if (cfg.sidechainTestnet)
    std::cout << "\nTestnet mode" << std::endl;

  platform_system::Dispatcher dispatcher;
  NodeRpcProxy node(dispatcher, cfg.daemonHost, cfg.daemonPort, logger);

  bool daemonConnected = false;
  if (!cfg.sidechainTestnet)
  {
    try
    {
      NodeInitObserver initObs;
      node.init([&initObs](std::error_code ec)
                { initObs.initCompleted(ec); });
      initObs.waitForInitEnd();
      daemonConnected = true;
    }
    catch (...)
    {
      daemonConnected = false;
    }
  }

  std::cout << "Sidechain: " << cfg.sidechainHost << ":" << cfg.sidechainPort
            << " [" << (cfg.sidechainTestnet ? "TESTNET" : "MAINNET") << "]" << std::endl;
  std::cout << "DEX: " << cfg.dexHost << ":" << cfg.dexPort << std::endl;

  // Load token metadata cache for symbol/decimals resolution
  loadTokenCache(cfg.sidechainHost, cfg.sidechainPort);

  std::unique_ptr<BoltCore::Wallet> walletPtr;
  std::vector<BoltSync::FoundOutput> allOutputs;
  std::vector<BoltCore::OutputInfo> outputInfos;

  uint32_t scannedHeight = 0;

  if (daemonConnected && !cfg.skipScan && !cfg.dataDir.empty())
  {
    std::cout << "\nScanning main chain...\n"
              << std::endl;
    BoltSync::Scanner scanner(viewKey, viewPub, hasSpendKey ? &spendKey : nullptr);
    BoltSync::ScanConfig scanCfg;
    scanCfg.dataDir = cfg.dataDir;
    scanCfg.numThreads = cfg.scanThreads;
    BoltSync::ScanState state;
    if (scanner.scan(scanCfg, state))
    {
      allOutputs = std::move(state.results);
      scannedHeight = state.scannedTopHeight;
      std::cout << "Scan complete. Found " << allOutputs.size() << " outputs.\n"
                << std::endl;
    }
  }

  if (daemonConnected)
  {
    for (const auto &fo : allOutputs)
    {
      BoltCore::OutputInfo info;
      info.blockHeight = fo.blockHeight;
      info.txHash = fo.txHash;
      info.outputIndex = fo.outputIndex;
      info.globalOutputIndex = fo.globalOutputIndex;
      info.hasGlobalOutputIndex = fo.hasGlobalOutputIndex;
      info.amount = fo.amount;
      info.outputKey = fo.outputKey;
      info.txPublicKey = fo.txPublicKey;
      info.keyImage = fo.keyImage;
      info.spent = fo.spent;
      info.isDeposit = false;
      info.term = 0;
      info.subAddress = addressStr;
      outputInfos.push_back(info);
    }
    walletPtr.reset(new BoltCore::Wallet(viewKey, spendKey, viewPub, spendPub, node, currency));
    walletPtr->loadOutputs(outputInfos, scannedHeight);
  }

  std::string input;
  while (true)
  {
    clearScreen();

    auto status = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getStatus", "{}");
    uint64_t height = extractJsonNumber(status, "height");
    uint64_t tokenCount = extractJsonNumber(status, "tokenCount");
    bool connected = !status.empty() && status != "{}";

    BoltCore::Balance balance{0, 0, 0, 0};
    if (walletPtr)
      balance = walletPtr->getBalance();

    bool daemonLive = false;
    if (daemonConnected)
    {
      try
      {
        daemonLive = node.getLastLocalBlockHeight() > 0;
      }
      catch (...)
      {
        daemonLive = false;
      }
    }

    uint64_t sccxBalance = 0;
    {
      std::ostringstream sccxParams;
      sccxParams << R"({"address":")" << spendPubHex << R"(","tokenId":0})";
      std::string sccxJson = sidechainCall(cfg.sidechainHost, cfg.sidechainPort, "getTokenBalance", sccxParams.str());
      sccxBalance = extractJsonNumber(sccxJson, "result");
    }

    std::cout << "=== Conceal Bolt Wallet ============" << std::endl;
    std::cout << "Address: " << addressStr << std::endl;
    std::cout << "Main Chain: [" << (daemonLive ? "ONLINE" : "OFFLINE") << "]" << std::endl;
    std::cout << "Sidechain: [" << cfg.sidechainHost << ":" << cfg.sidechainPort
              << " " << (cfg.sidechainTestnet ? "TESTNET" : "STAGING") << "]"
              << " [" << (connected ? "CONNECTED" : "UNREACHABLE") << "]" << std::endl;
    std::cout << "  Height: " << height << " | Tokens: " << tokenCount << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "CCX Balance: " << formatAmount(balance.actual)
              << " (pending " << formatAmount(balance.pending) << ")" << std::endl;
    std::cout << "SCCX Balance: " << formatAmount(sccxBalance, 6) << std::endl;
    std::cout << "------------------------------------" << std::endl;

    if (walletPtr)
    {
      std::cout << "1. Send CCX transfer" << std::endl;
      std::cout << "2. Create deposit" << std::endl;
      std::cout << "3. Withdraw deposit" << std::endl;
      std::cout << "4. Optimize (fusion)" << std::endl;
      std::cout << "5. Generate sub-address" << std::endl;
      std::cout << "------------------------------------" << std::endl;
    }
    std::cout << "S1. Sidechain status" << std::endl;
    std::cout << "S2. List sidechain tokens" << std::endl;
    std::cout << "S3. Create sidechain token" << std::endl;
    std::cout << "S4. Send sidechain token" << std::endl;
    std::cout << "S5. Token balances" << std::endl;
    std::cout << "S6. Quick create token" << std::endl;
    std::cout << "S7. Transaction history" << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "A1. AMM pools" << std::endl;
    std::cout << "A2. Create AMM pool" << std::endl;
    std::cout << "A3. Add liquidity" << std::endl;
    std::cout << "A4. Remove liquidity" << std::endl;
    std::cout << "A5. AMM swap" << std::endl;
    std::cout << "A6. AMM positions" << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "V1. Create vesting schedule" << std::endl;
    std::cout << "V2. List vesting schedules" << std::endl;
    std::cout << "V3. Revoke vesting" << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "R1. List reward pools" << std::endl;
    std::cout << "R2. Create reward pool" << std::endl;
    std::cout << "R3. Stake in pool" << std::endl;
    std::cout << "R4. Unstake from pool" << std::endl;
    std::cout << "R5. Claim rewards" << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "D0. DEX deposit address" << std::endl;
    std::cout << "D1. DEX order book" << std::endl;
    std::cout << "D2. Place DEX order" << std::endl;
    std::cout << "D3. DEX trade history" << std::endl;
    std::cout << "D4. Cancel DEX order" << std::endl;
    std::cout << "D5. DEX escrow balance" << std::endl;
    std::cout << "------------------------------------" << std::endl;
    std::cout << "0. Exit" << std::endl;
    std::cout << "Choice: ";
    std::getline(std::cin, input);

    if (walletPtr && input == "1")
    {
      std::string destAddr;
      uint64_t amount;
      std::cout << "Destination: ";
      std::getline(std::cin, destAddr);
      std::cout << "Amount: ";
      std::cin >> amount;
      std::cin.ignore();
      auto res = walletPtr->transfer(destAddr, amount);
      std::cout << (res.success ? "Sent! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter...";
      std::cin.get();
    }
    else if (walletPtr && input == "2")
    {
      uint64_t amount;
      uint32_t term;
      std::cout << "Amount: ";
      std::cin >> amount;
      std::cout << "Term: ";
      std::cin >> term;
      std::cin.ignore();
      auto res = walletPtr->createDeposit(amount, term);
      std::cout << (res.success ? "Deposit created! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter...";
      std::cin.get();
    }
    else if (walletPtr && input == "3")
    {
      uint64_t depositId;
      std::cout << "Deposit ID: ";
      std::cin >> depositId;
      std::cin.ignore();
      auto res = walletPtr->withdrawDeposit(depositId);
      std::cout << (res.success ? "Withdrawn! Tx: " + res.txHash : "Error: " + res.error) << std::endl;
      std::cout << "Press enter...";
      std::cin.get();
    }
    else if (walletPtr && input == "4")
    {
      auto est = walletPtr->estimateFusion(1000000, cn::parameters::MINIMUM_MIXIN);
      std::cout << "Fusion ready: " << est.fusionReadyCount << " outputs (total: " << est.totalOutputCount << ")" << std::endl;
      if (est.fusionReadyCount > 0)
      {
        std::string yn;
        std::cout << "Create fusion? (y/n): ";
        std::getline(std::cin, yn);
        if (yn == "y")
        {
          auto res = walletPtr->createFusion(1000000, cn::parameters::MINIMUM_MIXIN);
          std::cout << (res.success ? "Fusion tx: " + res.txHash : "Error: " + res.error) << std::endl;
        }
      }
      std::cout << "Press enter...";
      std::cin.get();
    }
    else if (walletPtr && input == "5")
    {
      auto sub = walletPtr->generateSubAddress();
      std::cout << "New sub-address: " << sub.address << std::endl;
      std::cout << "Press enter...";
      std::cin.get();
    }
    else if (input == "S1" || input == "s1")
      sidechainStatusMenu(cfg);
    else if (input == "S2" || input == "s2")
      sidechainTokensMenu(cfg);
    else if (input == "S3" || input == "s3")
      sidechainCreateTokenMenu(cfg, spendPubHex);
    else if (input == "S4" || input == "s4")
      sidechainTransferMenu(cfg, spendPubHex, currency);
    else if (input == "S5" || input == "s5")
      sidechainBalanceMenu(cfg, spendPubHex);
    else if (input == "S6" || input == "s6")
      sidechainQuickCreateMenu(cfg, spendPubHex);
    else if (input == "S7" || input == "s7")
      sidechainTxHistoryMenu();
    else if (input == "A1" || input == "a1")
      ammPoolsMenu(cfg);
    else if (input == "A2" || input == "a2")
      ammCreatePoolMenu(cfg, spendPubHex);
    else if (input == "A3" || input == "a3")
      ammAddLiquidityMenu(cfg, spendPubHex);
    else if (input == "A4" || input == "a4")
      ammRemoveLiquidityMenu(cfg, spendPubHex);
    else if (input == "A5" || input == "a5")
      ammSwapMenu(cfg, spendPubHex);
    else if (input == "A6" || input == "a6")
      ammPositionsMenu(cfg, spendPubHex);
    else if (input == "V1" || input == "v1")
      vestingCreateMenu(cfg, spendPubHex, currency);
    else if (input == "V2" || input == "v2")
      vestingListMenu(cfg, spendPubHex);
    else if (input == "V3" || input == "v3")
      vestingRevokeMenu(cfg, spendPubHex);
    else if (input == "R1" || input == "r1")
      rewardPoolListMenu(cfg);
    else if (input == "R2" || input == "r2")
      rewardPoolCreateMenu(cfg, spendPubHex);
    else if (input == "R3" || input == "r3")
      rewardPoolStakeMenu(cfg, spendPubHex);
    else if (input == "R4" || input == "r4")
      rewardPoolUnstakeMenu(cfg, spendPubHex);
    else if (input == "R5" || input == "r5")
      rewardPoolClaimMenu(cfg, spendPubHex);
    else if (input == "D0" || input == "d0")
      dexDepositAddressMenu(cfg);
    else if (input == "D1" || input == "d1")
      dexOrderBookMenu(cfg);
    else if (input == "D2" || input == "d2")
      dexSubmitOrderMenu(cfg, spendPubHex);
    else if (input == "D3" || input == "d3")
      dexTradeHistoryMenu(cfg);
    else if (input == "D4" || input == "d4")
      dexCancelOrderMenu(cfg, spendPubHex);
    else if (input == "D5" || input == "d5")
      dexEscrowBalanceMenu(cfg, spendPubHex);
    else if (input == "0")
      break;
  }

  return 0;
}