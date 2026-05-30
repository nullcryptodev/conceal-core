// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

#include <boost/program_options.hpp>

#include "BoltRPC/BoltRpcServer.h"
#include "BoltRPC/MultiWalletManager.h"
#include "Common/PathTools.h"
#include "Common/SignalHandler.h"
#include "Common/Util.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/LoggerManager.h"
#include "NodeClient/NodeClient.h"
#include "Rpc/HttpServer.h"
#include "version.h"

namespace po = boost::program_options;
using namespace logging;

// Configuration
struct BoltConfig
{
  uint16_t rpcPort = 8080;
  std::string dataDir = "./wallet_data";
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string corsDomain = "";
  bool testnet = false;
  bool generateWallet = false;
  std::string importKeys;
  bool autoUnlock = false;
  std::string password;
  bool offline = false;
  bool multiWallet = false;
  std::string walletsDir = ""; // Defaults to dataDir/wallets
};

// Signal handling
static std::atomic<bool> g_shutdown{false};

void signalHandler(int signal)
{
  if (signal == SIGINT || signal == SIGTERM)
  {
    std::cout << "\nShutting down...\n";
    g_shutdown.store(true);
  }
}

// Password prompt (no echo)
std::string promptPassword(const std::string &prompt)
{
  std::cout << prompt;
  std::cout.flush();

#ifdef _WIN32
  HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode;
  GetConsoleMode(hStdin, &mode);
  SetConsoleMode(hStdin, mode & ~ENABLE_ECHO_INPUT);
  std::string password;
  std::getline(std::cin, password);
  SetConsoleMode(hStdin, mode);
#else
  termios oldt, newt;
  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);
  std::string password;
  std::getline(std::cin, password);
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

  std::cout << std::endl;
  return password;
}

// Logger configuration
common::JsonValue buildLoggerConfiguration(logging::Level level, const std::string &logfile)
{
  common::JsonValue loggerConfiguration(common::JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  common::JsonValue &cfgLoggers = loggerConfiguration.insert("loggers", common::JsonValue::ARRAY);

  common::JsonValue &fileLogger = cfgLoggers.pushBack(common::JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(logging::TRACE));

  common::JsonValue &consoleLogger = cfgLoggers.pushBack(common::JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(logging::TRACE));
  consoleLogger.insert("pattern", "%T %L ");

  return loggerConfiguration;
}

// Main
int main(int argc, char *argv[])
{
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  BoltConfig config;

  // Parse command line
  for (int i = 1; i < argc; ++i)
  {
    std::string arg = argv[i];
    if (arg == "--port" && i + 1 < argc)
      config.rpcPort = static_cast<uint16_t>(std::stoi(argv[++i]));
    else if (arg == "--data-dir" && i + 1 < argc)
      config.dataDir = argv[++i];
    else if (arg == "--daemon-host" && i + 1 < argc)
      config.daemonHost = argv[++i];
    else if (arg == "--daemon-port" && i + 1 < argc)
      config.daemonPort = static_cast<uint16_t>(std::stoi(argv[++i]));
    else if (arg == "--cors" && i + 1 < argc)
      config.corsDomain = argv[++i];
    else if (arg == "--testnet")
      config.testnet = true;
    else if (arg == "--generate-wallet")
      config.generateWallet = true;
    else if (arg == "--import-keys" && i + 1 < argc)
      config.importKeys = argv[++i];
    else if (arg == "--unlock")
      config.autoUnlock = true;
    else if (arg == "--password" && i + 1 < argc)
      config.password = argv[++i];
    else if (arg == "--offline")
      config.offline = true;
    else if (arg == "--multi-wallet") {
      config.multiWallet = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        config.walletsDir = argv[++i];
    }
    else if (arg == "--help" || arg == "-h")
    {
      std::cout << "conceal-rpc " << CCX_RELEASE_VERSION << " - Conceal Wallet RPC Server\n\n"
                << "Usage: conceal-rpc [options]\n\n"
                << "Options:\n"
                << "  --port <port>           RPC server port (default: 8080)\n"
                << "  --data-dir <path>       Wallet data directory (default: ./wallet_data)\n"
                << "  --daemon-host <host>    Daemon host (default: 127.0.0.1)\n"
                << "  --daemon-port <port>    Daemon RPC port (default: 16000)\n"
                << "  --cors <domain>         CORS domain for web wallets (default: none)\n"
                << "  --testnet               Use testnet\n"
                << "  --generate-wallet       Create a new wallet on startup\n"
                << "  --import-keys <sp:view> Import wallet from spend:view hex keys\n"
                << "  --unlock                Auto-unlock existing wallet\n"
                << "  --password <pwd>        Password for wallet (or prompted if not set)\n"
                << "  --offline               Start without daemon connection\n"
                << "  --multi-wallet <dir>    Multi-wallet mode with optional wallets directory\n"
                << "  --help, -h              Show this help\n";
      return 0;
    }
  }

  // In multi-wallet mode, walletsDir defaults to dataDir/wallets if not specified
  if (config.multiWallet && config.walletsDir.empty())
  {
    config.walletsDir = config.dataDir + "/wallets";
  }

  // Ensure data directory exists
  if (!tools::create_directories_if_necessary(config.dataDir))
  {
    std::cerr << "Failed to create data directory: " << config.dataDir << "\n";
    return 1;
  }

  // In multi-wallet mode, also ensure wallets directory exists
  if (config.multiWallet)
  {
    if (!tools::create_directories_if_necessary(config.walletsDir))
    {
      std::cerr << "Failed to create wallets directory: " << config.walletsDir << "\n";
      return 1;
    } 
  }

  // Setup logger
  logging::LoggerManager logManager;
  std::string logFile = config.dataDir + "/conceal-rpc.log";
  logManager.configure(buildLoggerConfiguration(
      static_cast<logging::Level>(static_cast<int>(logging::ERROR) + 2),
      logFile));
  logging::LoggerRef logger(logManager, "conceal-rpc");

  logger(INFO, BRIGHT_YELLOW) << "conceal-rpc " << CCX_RELEASE_VERSION;

  if (config.multiWallet)
    logger(INFO) << "Running in multi-wallet mode. Wallets dir: " << config.walletsDir;

  // Create currency
  cn::CurrencyBuilder currencyBuilder(logManager);
  if (config.testnet)
    currencyBuilder.testnet(true);
  cn::Currency currency = currencyBuilder.currency();

  // Daemon connection (unless offline)
  std::unique_ptr<cn::INode> nodePtr;

  if (!config.offline)
  {
    logger(INFO) << "Connecting to daemon at " << config.daemonHost
                 << ":" << config.daemonPort;

    auto *client = new NodeClient::NodeClient(config.daemonHost, config.daemonPort);
    nodePtr.reset(client);

    bool initDone = false;
    std::error_code initEc;
    nodePtr->init([&initDone, &initEc](std::error_code ec)
                  {
            initDone = true;
            initEc = ec; });

    while (!initDone && !g_shutdown.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (initEc)
    {
      logger(ERROR, BRIGHT_RED) << "Failed to connect to daemon: " << initEc.message();
      return 1;
    }

    logger(INFO) << "Connected to daemon.";
  }

  // Create RPC server
  logger(INFO) << "Starting conceal-rpc server on port " << config.rpcPort;

  if (config.multiWallet)
  {
    // Create MultiWalletManager
    BoltRPC::MultiWalletManager multiWallet(
        config.walletsDir,
        nodePtr.get(),
        currency);

    // Wrap in BoltRpcServer (multi-wallet constructor)
    BoltRPC::BoltRpcServer server(
        config.rpcPort,
        multiWallet,
        currency,
        config.dataDir,
        config.daemonHost,
        config.daemonPort);

    if (!config.corsDomain.empty())
      server.setCorsDomain(config.corsDomain);

    // Start HTTP server
    if (!server.start())
    {
      logger(ERROR, BRIGHT_RED) << "Failed to start RPC server";
      goto shutdown;
    } 

    logger(INFO, BRIGHT_GREEN) << "Multi-wallet server running on http://127.0.0.1:" << config.rpcPort;
    logger(INFO) << "Manage wallets via createWallet, loadWallet, listWallets RPC methods.";

    // Wait for shutdown signal
    while (!g_shutdown.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(500));

    goto shutdown;
  } // MULTI-WALLET PATH
  else // SINGLE-WALLET PATH
  {
    BoltRPC::BoltRpcServer server(
        config.rpcPort,
        nodePtr ? *nodePtr : *static_cast<cn::INode *>(nullptr),
        currency,
        config.dataDir,
        config.daemonHost,
        config.daemonPort);

    if (!config.corsDomain.empty())
      server.setCorsDomain(config.corsDomain);

    // Startup modes
    if (config.generateWallet)
    {
      std::string password = config.password.empty()
                                 ? promptPassword("Enter new wallet password: ")
                                 : config.password;

      if (!server.wallet().generateNewWallet(password))
      {
        logger(ERROR, BRIGHT_RED) << "Failed to create wallet";
        goto shutdown;
      }
      logger(INFO, BRIGHT_GREEN) << "New wallet created: " << server.wallet().getAddress();

      if (!config.offline)
      {
        server.wallet().startSync([](const BoltRPC::WalletStatus &) {});
        logger(INFO) << "Sync started in background";
      }
    }
    else if (!config.importKeys.empty())
    {
      auto colon = config.importKeys.find(':');
      if (colon == std::string::npos)
      {
        logger(ERROR, BRIGHT_RED) << "Invalid key format. Use spendkey:viewkey";
        goto shutdown;
      }

      std::string spendKey = config.importKeys.substr(0, colon);
      std::string viewKey = config.importKeys.substr(colon + 1);
      std::string password = config.password.empty()
                                 ? promptPassword("Enter new wallet password: ")
                                 : config.password;

      if (!server.wallet().importFromKeys(viewKey, spendKey, password))
      {
        logger(ERROR, BRIGHT_RED) << "Failed to import wallet";
        goto shutdown;
      }
      logger(INFO, BRIGHT_GREEN) << "Wallet imported: " << server.wallet().getAddress();

      if (!config.offline)
      {
        server.wallet().startSync([](const BoltRPC::WalletStatus &) {});
        logger(INFO) << "Sync started in background";
      }
    }
    else if (config.autoUnlock && server.wallet().hasExistingWallet())
    {
      std::string password = config.password.empty()
                                 ? promptPassword("Enter wallet password: ")
                                 : config.password;

      if (!server.wallet().unlock(password))
      {
        logger(ERROR, BRIGHT_RED) << "Failed to unlock wallet. Wrong password?";
        goto shutdown;
      }
      logger(INFO, BRIGHT_GREEN) << "Wallet unlocked: " << server.wallet().getAddress();

      if (!config.offline)
      {
        server.wallet().startSync([](const BoltRPC::WalletStatus &) {});
        logger(INFO) << "Sync started in background";
      }
    }

    // Start HTTP server
    if (!server.start())
    {
      logger(ERROR, BRIGHT_RED) << "Failed to start RPC server";
      goto shutdown;
    }

    logger(INFO, BRIGHT_GREEN) << "conceal-rpc server running on http://127.0.0.1:" << config.rpcPort;
    logger(INFO) << "Wallet data: " << config.dataDir;

    if (server.wallet().getStatus().locked)
    {
      if (server.wallet().hasExistingWallet())
        logger(INFO) << "Existing wallet found. Use 'unlockWallet' RPC or --unlock to open.";
      else
        logger(INFO) << "No wallet found. Use 'createWallet' or 'importWallet' RPC, or --generate-wallet/--import-keys.";
    }

    if (config.offline)
      logger(INFO, BRIGHT_YELLOW) << "Running in offline mode. No sync available.";

    logger(INFO) << "Press Ctrl+C to stop.";

    // Wait for shutdown signal
    while (!g_shutdown.load())
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

shutdown:
  logger(INFO) << "Shutting down...";

  // server.stop() called in its own scope above; no need to call again

  if (nodePtr)
  {
    nodePtr->shutdown();
    nodePtr.reset();
  }

  logger(INFO) << "conceal-rpc stopped.";

  return 0;
}