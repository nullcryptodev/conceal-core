// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

// conceal-coldsync – Offline wallet pre-syncer
// Creates an encrypted wallet state file by scanning the MDBX blockchain directly
// using the two-pass filter for fast output recognition.
// No daemon connection required.

#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "BoltRPC/StateManager.h"
#include "BoltRPC/WalletManager.h"
#include "Storage/MDBXBlockchainStorage.h"

#include "Common/Util.h"
#include "Common/PathHelpers.h"
#include "Common/StringTools.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "CryptoNoteConfig.h"

namespace po = boost::program_options;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct Config
{
  std::string dataDir;
  std::string containerFile;
  std::string password;
  std::string viewKeyHex;
  std::string spendKeyHex;
  bool scanBlocks = true;
  std::string progressFile;
  unsigned int numThreads = 1;
  uint32_t startBlock = 0;
  uint32_t endBlock = 0;
  bool forceOverwrite = false;
};

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Cold Storage Wallet Initializer for Conceal");
  desc.add_options()("help,h", "Show this help message")("data-dir", po::value<std::string>()->required(),
                                                         "Path to conceal data directory (contains mdbx_blocks)")("container", po::value<std::string>()->default_value(""),
                                                                                                                  "Output wallet container path (auto-generated if not specified)")("password", po::value<std::string>()->required(),
                                                                                                                                                                                    "Password to encrypt the container")("view-key", po::value<std::string>()->required(),
                                                                                                                                                                                                                         "64-char hex private view key")("spend-key", po::value<std::string>()->default_value(""),
                                                                                                                                                                                                                                                         "64-char hex private spend key (optional, omit for view-only)")("scan-blockchain", po::value<bool>()->default_value(true),
                                                                                                                                                                                                                                                                                                                         "Scan blockchain (true) or create empty container (false)")("progress-file", po::value<std::string>()->default_value(""),
                                                                                                                                                                                                                                                                                                                                                                                     "File to write scan progress (for GUI)")("threads", po::value<unsigned int>()->default_value(1),
                                                                                                                                                                                                                                                                                                                                                                                                                              "Number of scan threads (0=auto)")("start-block", po::value<uint32_t>()->default_value(0),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                 "Start scanning from this block height")("end-block", po::value<uint32_t>()->default_value(0),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          "Stop scanning at this block height (0=scan all)")("force", po::value<bool>()->default_value(false),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             "Overwrite existing container file if present");

  po::variables_map vm;
  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);
    if (vm.count("help"))
    {
      std::cout << desc << std::endl;
      return false;
    }
    po::notify(vm);

    cfg.dataDir = vm["data-dir"].as<std::string>();
    cfg.containerFile = vm["container"].as<std::string>();
    cfg.password = vm["password"].as<std::string>();
    cfg.viewKeyHex = vm["view-key"].as<std::string>();
    cfg.spendKeyHex = vm["spend-key"].as<std::string>();
    cfg.scanBlocks = vm["scan-blockchain"].as<bool>();
    cfg.progressFile = vm["progress-file"].as<std::string>();
    cfg.numThreads = vm["threads"].as<unsigned int>();
    cfg.startBlock = vm["start-block"].as<uint32_t>();
    cfg.endBlock = vm["end-block"].as<uint32_t>();
    cfg.forceOverwrite = vm["force"].as<bool>();
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl
              << desc << std::endl;
    return false;
  }

  if (cfg.viewKeyHex.size() != 64)
  {
    std::cerr << "Error: view-key must be 64 hex characters" << std::endl;
    return false;
  }
  if (!cfg.spendKeyHex.empty() && cfg.spendKeyHex.size() != 64)
  {
    std::cerr << "Error: spend-key must be 64 hex characters" << std::endl;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
  try
  {
    Config cfg;
    if (!parseArgs(argc, argv, cfg))
      return 1;

    // Parse keys
    crypto::SecretKey viewKey, spendKey;
    if (!common::podFromHex(cfg.viewKeyHex, viewKey))
    {
      std::cerr << "Error: Invalid view key hex" << std::endl;
      return 1;
    }

    bool hasSpendKey = !cfg.spendKeyHex.empty();
    if (hasSpendKey && !common::podFromHex(cfg.spendKeyHex, spendKey))
    {
      std::cerr << "Error: Invalid spend key hex" << std::endl;
      return 1;
    }

    // Derive public keys
    crypto::PublicKey viewPub, spendPub;
    if (!crypto::secret_key_to_public_key(viewKey, viewPub))
    {
      std::cerr << "Error: Failed to derive public view key" << std::endl;
      return 1;
    }
    if (hasSpendKey)
    {
      if (!crypto::secret_key_to_public_key(spendKey, spendPub))
      {
        std::cerr << "Error: Failed to derive public spend key" << std::endl;
        return 1;
      }
    }

    // Setup currency and address
    logging::ConsoleLogger consoleLogger;
    cn::Currency currency = cn::CurrencyBuilder(consoleLogger).currency();
    cn::AccountPublicAddress address = {spendPub, viewPub};
    std::string addressStr = currency.accountAddressAsString(address);
    std::cout << "Wallet address: " << addressStr << std::endl;

    // Determine container path
    std::string containerPath;
    if (!cfg.containerFile.empty())
    {
      containerPath = cfg.containerFile;
    }
    else
    {
      std::string shortAddr = addressStr.substr(0, 12);
      std::string walletType = hasSpendKey ? "full" : "view";
      containerPath = "bolt_" + walletType + "_" + shortAddr + ".state";
    }

    // Check for existing container
    {
      std::ifstream testFile(containerPath);
      if (testFile.good() && !cfg.forceOverwrite)
      {
        std::cerr << "Error: Container already exists: " << containerPath << std::endl;
        std::cerr << "Use --force to overwrite, or --container to specify a different path." << std::endl;
        return 1;
      }
    }

    // ── Scan blockchain via BoltSync::Scanner ───────────────────────────
    std::vector<BoltRPC::OutputInfo> outputs;
    uint32_t scannedHeight = 0;

    if (cfg.scanBlocks && !cfg.dataDir.empty())
    {
      std::string dbPath = PathHelpers::appendPath(cfg.dataDir, "mdbx_blocks");
      std::cout << "Opening MDBX blockchain at " << dbPath << std::endl;

      CryptoNote::MDBXBlockchainStorage storage(dbPath);

      uint32_t topHeight = storage.topBlockHeight();
      if (topHeight == 0)
      {
        std::cerr << "Error: Blockchain is empty" << std::endl;
        return 1;
      }

      if (cfg.endBlock == 0 || cfg.endBlock > topHeight)
        cfg.endBlock = topHeight;

      std::cout << "Scanning blocks " << cfg.startBlock << " to " << cfg.endBlock
                << " (chain height: " << topHeight << ")" << std::endl;

      // Use BoltSync::Scanner — does two-pass filter + ECDH derivation
      // using the local MDBX directly. The scanner already has the filter
      // integrated (BlockDeserializer.cpp checks filter records before ECDH).
      BoltSync::Scanner scanner(
          viewKey, spendPub,
          hasSpendKey ? &spendKey : nullptr);

      BoltSync::ScanConfig scanConfig;
      scanConfig.dataDir = cfg.dataDir;
      scanConfig.numThreads = cfg.numThreads;
      scanConfig.startBlock = cfg.startBlock;
      scanConfig.endBlock = cfg.endBlock;
      scanConfig.progressFile = cfg.progressFile;

      BoltSync::ScanState state;

      std::cout << "Starting scan with "
                << (cfg.numThreads == 0 ? "auto" : std::to_string(cfg.numThreads))
                << " threads..." << std::endl;

      if (!scanner.scan(scanConfig, state))
      {
        std::cerr << "Error: Scan failed" << std::endl;
        return 1;
      }

      scannedHeight = state.scannedTopHeight;

      // Convert BoltSync::FoundOutput → BoltRPC::OutputInfo
      uint64_t totalReceived = 0, totalSpent = 0;
      outputs.reserve(state.results.size());
      for (const auto &fo : state.results)
      {
        BoltRPC::OutputInfo info;
        info.blockHeight = fo.blockHeight;
        info.txHash = fo.txHash;
        info.amount = fo.amount;
        info.outputIndex = fo.outputIndex;
        info.outputKey = fo.outputKey;
        info.txPublicKey = fo.txPublicKey;
        info.spent = fo.spent;
        info.isDeposit = fo.isDeposit;
        info.term = fo.term;
        outputs.push_back(info);

        totalReceived += info.amount;
        if (info.spent)
          totalSpent += info.amount;
      }

      std::cout << "Scan complete." << std::endl;
      std::cout << "  Blocks processed: " << state.blocksProcessed << std::endl;
      std::cout << "  Owned outputs: " << outputs.size() << std::endl;
      std::cout << "  Received: " << totalReceived << std::endl;
      std::cout << "  Spent: " << totalSpent << std::endl;
      std::cout << "  Balance: " << (totalReceived - totalSpent) << std::endl;
    }

    // ── Save wallet state via StateManager ──────────────────────────────
    BoltRPC::StateManager stateManager(containerPath);

    BoltRPC::WalletState state;
    state.lastHeight = scannedHeight;
    state.balance = 0;
    state.unlockedBalance = 0;

    for (const auto &out : outputs)
    {
      state.ownedOutputs.push_back(out);
      if (!out.spent)
        state.balance += out.amount;
    }

    if (!stateManager.save(state))
    {
      std::cerr << "Error: Failed to save wallet state" << std::endl;
      return 1;
    }

    std::cout << std::endl
              << "========================================" << std::endl
              << "Wallet container created successfully!" << std::endl
              << "========================================" << std::endl
              << "Path: " << containerPath << std::endl
              << "Address: " << addressStr << std::endl
              << "Type: " << (hasSpendKey ? "Full wallet" : "View-only wallet") << std::endl
              << "Outputs: " << outputs.size() << std::endl
              << "Balance: " << state.balance << std::endl
              << "Synced height: " << scannedHeight << std::endl
              << "========================================" << std::endl;

    return 0;
  }
  catch (const std::exception &e)
  {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    return 2;
  }
}