// Config.cpp — wallet configuration parsing
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "Config.h"
#include <iostream>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

bool parseArgs(int argc, char *argv[], Config &cfg)
{
  po::options_description desc("Conceal Bolt - Terminal Wallet");
  desc.add_options()("help,h", "Show help")("data-dir", po::value<std::string>(), "Path to blockchain data directory (for scanning)")("daemon-host", po::value<std::string>()->default_value("127.0.0.1"), "Daemon RPC host")("daemon-port", po::value<uint16_t>()->default_value(16000), "Daemon RPC port")("view-key", po::value<std::string>(), "64-char hex private view key")("spend-key", po::value<std::string>(), "64-char hex private spend key (optional for view-only)")("threads", po::value<unsigned int>()->default_value(2), "Scan threads (0=auto)")("skip-scan", po::bool_switch(), "Skip blockchain scan (use if already scanned)")("sidechain-host", po::value<std::string>()->default_value("127.0.0.1"), "Sidechain RPC host")("sidechain-port", po::value<uint16_t>()->default_value(8080), "Sidechain RPC port")("sidechain-testnet", po::bool_switch(), "Connect to sidechain in testnet mode")("dex-host", po::value<std::string>()->default_value("127.0.0.1"), "DEX RPC host")("dex-port", po::value<uint16_t>()->default_value(8090), "DEX RPC port");

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

    if (vm.count("data-dir"))
      cfg.dataDir = vm["data-dir"].as<std::string>();
    cfg.daemonHost = vm["daemon-host"].as<std::string>();
    cfg.daemonPort = vm["daemon-port"].as<uint16_t>();
    if (vm.count("view-key"))
      cfg.viewKeyHex = vm["view-key"].as<std::string>();
    if (vm.count("spend-key"))
      cfg.spendKeyHex = vm["spend-key"].as<std::string>();
    cfg.scanThreads = vm["threads"].as<unsigned int>();
    cfg.skipScan = vm["skip-scan"].as<bool>();
    cfg.sidechainHost = vm["sidechain-host"].as<std::string>();
    cfg.sidechainPort = vm["sidechain-port"].as<uint16_t>();
    cfg.sidechainTestnet = vm["sidechain-testnet"].as<bool>();
    cfg.dexHost = vm["dex-host"].as<std::string>();
    cfg.dexPort = vm["dex-port"].as<uint16_t>();
  }
  catch (const std::exception &e)
  {
    std::cerr << "Error: " << e.what() << std::endl;
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