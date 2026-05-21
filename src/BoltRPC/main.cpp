// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <boost/program_options.hpp>

#include "BoltRPC/BoltRpcServer.h"
#include "Common/PathTools.h"
#include "Common/SignalHandler.h"
#include "Common/Util.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/LoggerManager.h"
#include "NodeRpcProxy/NodeRpcProxy.h"
#include "Rpc/HttpServer.h"
#include "version.h"

namespace po = boost::program_options;
using namespace logging;

// ─── Configuration ─────────────────────────────────────────────────────────

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
};

// ─── Signal handling ───────────────────────────────────────────────────────

static std::atomic<bool> g_shutdown{false};

#ifndef _WIN32
static int g_signalPipe[2] = {-1, -1};

static bool initSignalPipe()
{
  if (pipe(g_signalPipe) != 0)
    return false;
  fcntl(g_signalPipe[0], F_SETFL, O_NONBLOCK);
  fcntl(g_signalPipe[1], F_SETFL, O_NONBLOCK);
  return true;
}

static void closeSignalPipe()
{
  if (g_signalPipe[0] >= 0)
    close(g_signalPipe[0]);
  if (g_signalPipe[1] >= 0)
    close(g_signalPipe[1]);
  g_signalPipe[0] = g_signalPipe[1] = -1;
}
#endif

void signalHandler(int signal)
{
  if (signal != SIGINT && signal != SIGTERM)
    return;

  // Second signal: force exit (e.g. sync thread blocked on daemon I/O).
  if (g_shutdown.exchange(true))
    _exit(128 + signal);

#ifndef _WIN32
  if (g_signalPipe[1] >= 0)
  {
    const char byte = 1;
    ssize_t n = write(g_signalPipe[1], &byte, 1);
    (void)n;
  }
#endif
}

static void installSignalHandlers()
{
  struct sigaction sa{};
  sa.sa_handler = signalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
}

// Main thread idle loop — returns after first SIGINT/SIGTERM.
static void waitForShutdownRequest()
{
#ifndef _WIN32
  if (g_signalPipe[0] >= 0)
  {
    while (!g_shutdown.load())
    {
      pollfd pfd{};
      pfd.fd = g_signalPipe[0];
      pfd.events = POLLIN;
      poll(&pfd, 1, 100);
    }
    return;
  }
#endif
  while (!g_shutdown.load())
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ─── Password prompt (no echo) ────────────────────────────────────────────

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

// ─── Logger configuration ────────────────────────────────────────────────

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

// ─── Main ──────────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
  installSignalHandlers();
#ifndef _WIN32
  initSignalPipe();
#endif

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
                << "  --help, -h              Show this help\n";
      return 0;
    }
  }

  // Ensure data directory exists
  if (!tools::create_directories_if_necessary(config.dataDir))
  {
    std::cerr << "Failed to create data directory: " << config.dataDir << "\n";
    return 1;
  }

  // Setup logger
  logging::LoggerManager logManager;
  std::string logFile = config.dataDir + "/conceal-rpc.log";
  logManager.configure(buildLoggerConfiguration(
      static_cast<logging::Level>(static_cast<int>(logging::ERROR) + 2),
      logFile));
  logging::LoggerRef logger(logManager, "conceal-rpc");

  logger(INFO, BRIGHT_YELLOW) << "conceal-rpc " << CCX_RELEASE_VERSION;

  // Create currency
  cn::CurrencyBuilder currencyBuilder(logManager);
  if (config.testnet)
    currencyBuilder.testnet(true);
  cn::Currency currency = currencyBuilder.currency();

  // Connect to daemon (unless offline mode)
  cn::NodeRpcProxy *nodePtr = nullptr;
  platform_system::Dispatcher *dispatcherPtr = nullptr;

  if (!config.offline)
  {
    logger(INFO) << "Connecting to daemon at " << config.daemonHost
                 << ":" << config.daemonPort;

    dispatcherPtr = new platform_system::Dispatcher();
    nodePtr = new cn::NodeRpcProxy(*dispatcherPtr, config.daemonHost,
                                   config.daemonPort, logManager);

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
      logger(ERROR, BRIGHT_RED) << "Failed to connect to daemon: " << initEc.message()
                                << " (is conceald-archive running and RPC reachable?)";
      nodePtr->shutdown();
      delete nodePtr;
      nodePtr = nullptr;
      delete dispatcherPtr;
      dispatcherPtr = nullptr;
      return 1;
    }

    logger(INFO) << "Connected to daemon.";
  }

  // Create RPC server
  logger(INFO) << "Starting conceal-rpc server on port " << config.rpcPort;

  BoltRPC::BoltRpcServer server(
      config.rpcPort,
      config.offline ? *static_cast<cn::INode *>(nullptr) : *nodePtr,
      currency,
      config.dataDir,
      config.daemonHost,
      config.daemonPort);

  if (!config.corsDomain.empty())
    server.setCorsDomain(config.corsDomain);

  // ── Startup modes ──────────────────────────────────────────────────────

  if (config.generateWallet)
  {
    std::string password = config.password.empty()
                               ? promptPassword("Enter new wallet password: ")
                               : config.password;

    if (!server.wallet().generateNewWallet(password))
    {
      logger(ERROR, BRIGHT_RED) << "Failed to create wallet";
      if (nodePtr)
      {
        nodePtr->shutdown();
        delete nodePtr;
        delete dispatcherPtr;
      }
      return 1;
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
      if (nodePtr)
      {
        nodePtr->shutdown();
        delete nodePtr;
        delete dispatcherPtr;
      }
      return 1;
    }

    std::string spendKey = config.importKeys.substr(0, colon);
    std::string viewKey = config.importKeys.substr(colon + 1);
    std::string password = config.password.empty()
                               ? promptPassword("Enter new wallet password: ")
                               : config.password;

    if (!server.wallet().importFromKeys(viewKey, spendKey, password))
    {
      logger(ERROR, BRIGHT_RED) << "Failed to import wallet";
      if (nodePtr)
      {
        nodePtr->shutdown();
        delete nodePtr;
        delete dispatcherPtr;
      }
      return 1;
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
      if (nodePtr)
      {
        nodePtr->shutdown();
        delete nodePtr;
        delete dispatcherPtr;
      }
      return 1;
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
    if (nodePtr)
    {
      nodePtr->shutdown();
      delete nodePtr;
      delete dispatcherPtr;
    }
    return 1;
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

  waitForShutdownRequest();

  logger(INFO) << "Shutting down...";
  server.wallet().stopSync();
  server.stop();
  if (nodePtr)
  {
    nodePtr->shutdown();
    delete nodePtr;
    nodePtr = nullptr;
  }
  delete dispatcherPtr;
  dispatcherPtr = nullptr;
  logger(INFO) << "conceal-rpc stopped.";

#ifndef _WIN32
  closeSignalPipe();
#endif
  return 0;
}