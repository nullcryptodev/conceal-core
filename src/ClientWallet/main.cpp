// ClientWallet - interactive TUI wallet for Conceal Network
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "EventLoop.h"
#include "SyncEngine.h"
#include "NullNode.h"

#include "Screens/Screen.h"
#include "Screens/OverviewScreen.h"
#include "Screens/SendScreen.h"
#include "Screens/HistoryScreen.h"

#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"

#include "Common/Tui.h"
#include "Common/Util.h"
#include "Common/StringTools.h"
#include "Common/PathHelpers.h"

#include "crypto/crypto.h"

#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteConfig.h"

#include "Logging/ConsoleLogger.h"

#include "NodeClient/NodeClient.h"
#include "Rpc/HttpClient.h"
#include "System/Dispatcher.h"

#include "INode.h"

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstring>

enum class ConnectionMode
{
  Local,
  Remote,
  Offline
};

struct ConnectionConfig
{
  ConnectionMode mode = ConnectionMode::Local;
  std::string dataPath;
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string stateFile;
};

struct Config
{
  std::string viewKeyHex;
  std::string spendKeyHex;
  std::string spendPubHex;
  ConnectionMode mode = ConnectionMode::Local;
  std::string dataPath;
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string stateFile;
  uint32_t startHeight = 0;
};

enum class StartupChoice
{
  CreateNew,
  ImportKeys,
  OpenState,
  Quit
};

namespace
{

constexpr const char *kWalletStateFileName = "wallet_state.bin";

std::string expandHomePath(std::string path)
{
  if (!path.empty() && path[0] == '~')
  {
    if (const char *home = std::getenv("HOME"))
      path = std::string(home) + path.substr(1);
  }
  return path;
}

std::string defaultWalletStatePath(const std::string &dataDir)
{
  return PathHelpers::appendPath(dataDir, kWalletStateFileName);
}

bool pathIsReadableFile(const std::string &path)
{
  std::ifstream file(path, std::ios::binary);
  return file.good();
}

void printWalletUsage(const char *program)
{
  const char *name = program ? program : "conceal-wallet";
  std::cout
      << name << " — Conceal mainchain wallet (terminal UI)\n\n"
      << "Usage:\n"
      << "  " << name << " [options]\n"
      << "  " << name << " <view_key> [<spend_key>] [options]\n\n"
      << "Wallet keys (optional; otherwise use the startup menu):\n"
      << "  --view-key HEX       Private view key (64 hex characters)\n"
      << "  --spend-key HEX      Private spend key (64 hex characters)\n\n"
      << "Blockchain / sync:\n"
      << "  --data-dir PATH      Path to conceald data directory (contains mdbx_blocks)\n"
      << "  --daemon HOST:PORT   Daemon JSON-RPC (default 127.0.0.1:"
      << cn::RPC_DEFAULT_PORT << ")\n"
      << "  --start-height N     Fresh rescan from block N (skips existing state file)\n"
      << "  --state FILE         Resume from saved wallet state (sync continues unless --offline)\n\n"
      << "Other:\n"
      << "  --offline            MDBX only; no daemon RPC of any kind\n"
      << "  -h, --help           Show this help\n\n"
      << "  Sync log:            /tmp/conceal-wallet-sync.log\n\n"
      << "Example first-time import:\n"
      << "  " << name << " <view_hex> <spend_hex> --data-dir ~/.conceal-mdbx --start-height 2000000\n"
      << "  (creates ~/.conceal-mdbx/wallet_state.bin on exit)\n"
      << "Example resume with remote sync:\n"
      << "  " << name << " <view_hex> <spend_hex> --state ~/.conceal/wallet_state.bin --daemon 66.203.178.176:16000\n";
}

bool isHexKey(const std::string &s)
{
  return s.size() == 64 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}

bool parseWalletArgs(int argc, char *argv[], Config &cfg, bool &hasKeys)
{
  std::vector<std::string> positional;
  bool hasDataDir = false;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h")
      continue;
    if (arg == "--view-key" && i + 1 < argc)
    {
      cfg.viewKeyHex = argv[++i];
      hasKeys = true;
    }
    else if (arg == "--spend-key" && i + 1 < argc)
      cfg.spendKeyHex = argv[++i];
    else if (arg == "--daemon" && i + 1 < argc)
    {
      const std::string hp = argv[++i];
      const size_t colon = hp.find(':');
      if (colon != std::string::npos)
      {
        cfg.daemonHost = hp.substr(0, colon);
        cfg.daemonPort = static_cast<uint16_t>(std::stoi(hp.substr(colon + 1)));
      }
      cfg.mode = ConnectionMode::Remote;
    }
    else if (arg == "--data-dir" && i + 1 < argc)
    {
      cfg.dataPath = argv[++i];
      hasDataDir = true;
    }
    else if (arg == "--offline")
      cfg.mode = ConnectionMode::Offline;
    else if (arg == "--state" && i + 1 < argc)
      cfg.stateFile = argv[++i];
    else if (arg == "--start-height" && i + 1 < argc)
      cfg.startHeight = static_cast<uint32_t>(std::stoul(argv[++i]));
    else if (arg.rfind("--", 0) == 0)
    {
      std::cerr << "Unknown option: " << arg << " (try --help)\n";
      return false;
    }
    else
      positional.push_back(arg);
  }

  size_t pi = 0;
  if (cfg.viewKeyHex.empty() && pi < positional.size() && isHexKey(positional[pi]))
  {
    cfg.viewKeyHex = positional[pi++];
    hasKeys = true;
  }
  if (cfg.spendKeyHex.empty() && pi < positional.size() && isHexKey(positional[pi]))
    cfg.spendKeyHex = positional[pi++];

  if (hasKeys && !isHexKey(cfg.viewKeyHex))
  {
    std::cerr << "Error: view key must be 64 hex characters\n";
    return false;
  }
  if (!cfg.spendKeyHex.empty() && !isHexKey(cfg.spendKeyHex))
  {
    std::cerr << "Error: spend key must be 64 hex characters\n";
    return false;
  }

  // --data-dir implies Local (MDBX + daemon RPC), but must not override an
  // explicit --offline: in offline mode the user wants MDBX-only, no RPC.
  if (hasDataDir && cfg.mode != ConnectionMode::Offline)
    cfg.mode = ConnectionMode::Local;
  // --state alone does not disable sync; use --offline for state-only mode

  return true;
}

} // namespace

StartupChoice showStartupMenu()
{
  std::cout << Tui::clearScreen();
  std::cout << Tui::cursorTo(1, 0);
  std::cout << Tui::bold() << Tui::brightCyan() << "  Conceal Wallet" << Tui::reset() << "\n\n";

  std::vector<std::string> items = {
      "Create New Wallet",
      "Import from Keys",
      "Open Saved State",
      "Quit"};

  int selected = 0;
  while (true)
  {
    std::cout << Tui::cursorTo(3, 0) << Tui::clearToEndOfScreen();
    for (size_t i = 0; i < items.size(); ++i)
    {
      if (static_cast<int>(i) == selected)
        std::cout << Tui::bold() << Tui::brightWhite() << "  > " << items[i] << Tui::reset() << "\n";
      else
        std::cout << Tui::dim() << "    " << items[i] << Tui::reset() << "\n";
    }
    std::cout << "\n"
              << Tui::dim() << "  Arrow keys: navigate  |  Enter: select  |  Q: quit" << Tui::reset();

    int key = Tui::readKey();
    while (key == -1)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      key = Tui::readKey();
    }

    switch (key)
    {
    case Tui::KEY_UP:
      if (selected > 0)
        selected--;
      break;
    case Tui::KEY_DOWN:
      if (selected < (int)items.size() - 1)
        selected++;
      break;
    case Tui::KEY_LF:
    case Tui::KEY_ENTER:
      return static_cast<StartupChoice>(selected);
    case 'q':
    case 'Q':
      return StartupChoice::Quit;
    }
  }
}

ConnectionConfig simpleSetupScreen(const std::string &defaultDataDir)
{
  ConnectionConfig config;
  config.dataPath = defaultDataDir;

  std::vector<std::string> items = {
      "Local Blockchain (fast sync from local data)",
      "Remote Daemon (connect to a node)",
      "Offline Mode (state file only)"};

  int selected = 0;
  while (true)
  {
    std::cout << Tui::clearScreen();
    std::cout << Tui::cursorTo(1, 0);
    std::cout << Tui::bold() << Tui::brightCyan() << "  Connection Setup" << Tui::reset() << "\n\n";

    for (size_t i = 0; i < items.size(); ++i)
    {
      if (static_cast<int>(i) == selected)
        std::cout << Tui::bold() << Tui::brightWhite() << "  > " << items[i] << Tui::reset() << "\n";
      else
        std::cout << Tui::dim() << "    " << items[i] << Tui::reset() << "\n";
    }
    std::cout << "\n"
              << Tui::dim() << "  Arrow keys: select  |  Enter: confirm  |  Esc: back" << Tui::reset();

    int key = Tui::readKey();
    while (key == -1)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      key = Tui::readKey();
    }

    switch (key)
    {
    case Tui::KEY_UP:
      if (selected > 0)
        selected--;
      break;
    case Tui::KEY_DOWN:
      if (selected < 2)
        selected++;
      break;
    case Tui::KEY_LF:
    case Tui::KEY_ENTER:
      config.mode = static_cast<ConnectionMode>(selected);
      Tui::disableRawMode();
      if (config.mode == ConnectionMode::Local)
      {
        std::cout << "\nBlockchain path [" << config.dataPath << "]: ";
        std::string path;
        std::getline(std::cin, path);
        if (!path.empty())
          config.dataPath = path;
      }
      else if (config.mode == ConnectionMode::Remote)
      {
        std::cout << "\nDaemon host:port [127.0.0.1:16000]: ";
        std::string host;
        std::getline(std::cin, host);
        if (!host.empty())
        {
          size_t colon = host.find(':');
          if (colon != std::string::npos)
          {
            config.daemonHost = host.substr(0, colon);
            config.daemonPort = static_cast<uint16_t>(std::stoi(host.substr(colon + 1)));
          }
        }
      }
      else
      {
        std::cout << "\nState file path: ";
        std::getline(std::cin, config.stateFile);
      }
      Tui::enableRawMode();
      return config;
    case Tui::KEY_ESC:
    case 'q':
    case 'Q':
      return config;
    }
  }
}

bool createNewWallet(Config &cfg)
{
  std::cout << Tui::clearScreen();
  std::cout << Tui::cursorTo(1, 0);
  std::cout << Tui::bold() << "Create New Wallet" << Tui::reset() << "\n\n";

  crypto::SecretKey viewKey, spendKey;
  crypto::PublicKey viewPub, spendPub;
  crypto::generate_keys(viewPub, viewKey);
  crypto::generate_keys(spendPub, spendKey);

  cfg.viewKeyHex = common::podToHex(viewKey);
  cfg.spendKeyHex = common::podToHex(spendKey);

  std::cout << Tui::brightGreen() << "Wallet created!" << Tui::reset() << "\n\n";
  std::cout << Tui::brightYellow() << "SAVE THESE KEYS - YOU CANNOT RECOVER THEM\n\n"
            << Tui::reset();
  std::cout << "View Key:  " << cfg.viewKeyHex << "\n";
  std::cout << "Spend Key: " << cfg.spendKeyHex << "\n\n";

  logging::ConsoleLogger logger;
  cn::Currency currency = cn::CurrencyBuilder(logger).currency();
  cn::AccountPublicAddress addr = {spendPub, viewPub};
  std::string addrStr = currency.accountAddressAsString(addr);
  std::cout << "Address:   " << addrStr << "\n\n";
  std::cout << "Press Enter to continue...";
  std::cout.flush();

  while (true)
  {
    int key = Tui::readKey();
    if (key == Tui::KEY_LF || key == Tui::KEY_ENTER || key == 'q' || key == 'Q')
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return true;
}

bool importKeys(Config &cfg)
{
  std::cout << Tui::clearScreen();
  std::cout << Tui::cursorTo(1, 0);
  std::cout << Tui::bold() << "Import from Keys" << Tui::reset() << "\n\n";
  Tui::disableRawMode();

  std::cout << "Enter 64-char view key: ";
  std::getline(std::cin, cfg.viewKeyHex);

  std::cout << "Enter 64-char spend key (or empty for view-only): ";
  std::getline(std::cin, cfg.spendKeyHex);

  if (cfg.spendKeyHex.empty())
  {
    std::cout << "Enter 64-char spend PUBLIC key (or wallet address): ";
    std::getline(std::cin, cfg.spendPubHex);
  }

  Tui::enableRawMode();
  if (cfg.viewKeyHex.size() != 64)
  {
    std::cout << Tui::red() << "Invalid view key" << Tui::reset() << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return false;
  }
  return true;
}

bool openSavedState(Config &cfg)
{
  std::cout << Tui::clearScreen();
  std::cout << Tui::cursorTo(1, 0);
  std::cout << Tui::bold() << "Open Saved State" << Tui::reset() << "\n\n";
  Tui::disableRawMode();

  std::cout << "Path to state file: ";
  std::getline(std::cin, cfg.stateFile);

  Tui::enableRawMode();
  if (cfg.stateFile.empty())
  {
    std::cout << Tui::red() << "No path provided" << Tui::reset() << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return false;
  }
  return true;
}

int main(int argc, char *argv[])
{
  for (int i = 1; i < argc; ++i)
  {
    if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h")
    {
      printWalletUsage(argv[0]);
      return 0;
    }
  }

  Config cfg;
  bool hasKeys = false;

  if (argc > 1 && !parseWalletArgs(argc, argv, cfg, hasKeys))
    return 1;

  if (cfg.dataPath.empty())
    cfg.dataPath = tools::getDefaultDataDirectory();

  if (!hasKeys && cfg.stateFile.empty())
  {
    Tui::enableRawMode();
    bool keyDone = false;
    while (!keyDone)
    {
      StartupChoice choice = showStartupMenu();
      switch (choice)
      {
      case StartupChoice::CreateNew:
        keyDone = createNewWallet(cfg);
        hasKeys = keyDone;
        break;
      case StartupChoice::ImportKeys:
        keyDone = importKeys(cfg);
        hasKeys = keyDone && !cfg.viewKeyHex.empty();
        break;
      case StartupChoice::OpenState:
        keyDone = openSavedState(cfg);
        break;
      case StartupChoice::Quit:
        Tui::disableRawMode();
        return 0;
      }
    }
    Tui::disableRawMode();
  }

  if (!hasKeys && cfg.stateFile.empty())
  {
    std::cerr << "No keys provided." << std::endl;
    return 1;
  }

  if (argc <= 1 && cfg.stateFile.empty())
  {
    Tui::enableRawMode();
    ConnectionConfig conn = simpleSetupScreen(cfg.dataPath);
    Tui::disableRawMode();
    cfg.mode = conn.mode;
    cfg.dataPath = conn.dataPath;
    cfg.daemonHost = conn.daemonHost;
    cfg.daemonPort = conn.daemonPort;
  }

  cfg.dataPath = expandHomePath(cfg.dataPath);
  cfg.stateFile = expandHomePath(cfg.stateFile);

  if (cfg.stateFile.empty() && !cfg.dataPath.empty())
    cfg.stateFile = defaultWalletStatePath(cfg.dataPath);

  crypto::SecretKey viewKey, spendKey;
  crypto::PublicKey viewPub, spendPub;

  if (hasKeys)
  {
    common::podFromHex(cfg.viewKeyHex, viewKey);
    crypto::secret_key_to_public_key(viewKey, viewPub);

    if (!cfg.spendKeyHex.empty())
    {
      common::podFromHex(cfg.spendKeyHex, spendKey);
      crypto::secret_key_to_public_key(spendKey, spendPub);
    }
    else if (!cfg.spendPubHex.empty())
    {
      if (cfg.spendPubHex.size() == 64)
      {
        size_t size;
        crypto::Hash tmp;
        common::fromHex(cfg.spendPubHex, &tmp, sizeof(tmp), size);
        if (size == sizeof(crypto::PublicKey))
          memcpy(&spendPub, &tmp, sizeof(spendPub));
      }
      else
      {
        logging::ConsoleLogger logger;
        cn::Currency currency = cn::CurrencyBuilder(logger).currency();
        cn::AccountPublicAddress addr;
        if (currency.parseAccountAddressString(cfg.spendPubHex, addr))
          spendPub = addr.spendPublicKey;
        else
        {
          std::cerr << "Invalid spend public key or address" << std::endl;
          return 1;
        }
      }
    }
  }

  logging::ConsoleLogger consoleLogger;
  cn::Currency currency = cn::CurrencyBuilder(consoleLogger).currency();

  platform_system::Dispatcher dispatcher;

  ClientWallet::NullNode nullNode;
  std::unique_ptr<cn::INode> remoteNode;
  cn::INode *nodePtr = &nullNode;

  if (cfg.mode != ConnectionMode::Offline)
  {
    auto *client = new NodeClient::NodeClient(cfg.daemonHost, cfg.daemonPort);
    remoteNode.reset(client);
    nodePtr = remoteNode.get();

    // Init the connection (needed for send/deposit relay and mixin RPC even in local MDBX mode).
    bool initDone = false;
    client->init([&initDone](std::error_code ec)
                 { initDone = true; });
    while (!initDone)
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  auto wallet = std::make_shared<BoltCore::Wallet>(
      viewKey,
      cfg.spendKeyHex.empty() ? crypto::SecretKey() : spendKey,
      viewPub,
      spendPub,
      *nodePtr,
      currency);

  const bool chainSync = (cfg.mode != ConnectionMode::Offline);
  auto sync = std::make_shared<ClientWallet::SyncEngine>(
      cfg.dataPath, viewKey, spendPub,
      cfg.spendKeyHex.empty() ? nullptr : &spendKey,
      chainSync);

  // When --start-height is provided the user explicitly requests a fresh
  // rescan from that height.  Skip loading any existing state file so the
  // old wallet_state.bin is not silently restored.  On exit, saveStateFile
  // will overwrite it with the results of the new scan.
  bool stateLoaded = false;
  if (!cfg.stateFile.empty() && cfg.startHeight == 0)
  {
    const bool stateExists = pathIsReadableFile(cfg.stateFile);
    if (stateExists && sync->loadStateFile(cfg.stateFile))
    {
      stateLoaded = true;
      wallet->loadOutputs(sync->getCachedOutputs(), sync->lastScannedHeight());
    }
    else if (stateExists)
      std::cerr << "Warning: could not load state file: " << cfg.stateFile << std::endl;
  }

  // Apply --start-height unconditionally (no !stateLoaded guard) — it was
  // already guaranteed above that no state was loaded when startHeight > 0.
  if (cfg.startHeight > 0)
    sync->setScannedHeight(cfg.startHeight);

  if (cfg.mode != ConnectionMode::Offline)
  {
    sync->setNode(nodePtr);

    // Wire the JSON-RPC callback so Polling strategy (no local MDBX) works exactly
    // like legacy concealwallet: BoltRPC::SyncManager pulls blocks from the daemon.
    // This applies whether --daemon points to localhost or a remote node.
    auto *nc = dynamic_cast<NodeClient::NodeClient *>(nodePtr);
    if (nc)
    {
      sync->setDaemonRpc([nc](const std::string &method, const std::string &params) -> std::string
      {
        return nc->callDaemonMethod(method, params);
      });
    }
  }

  ClientWallet::EventLoop loop;
  loop.setWallet(wallet);
  loop.setSyncEngine(sync);
  loop.setNode(nodePtr);
  loop.setCurrency(currency);
  loop.setStatePath(cfg.stateFile);

  loop.pushScreen(std::make_shared<ClientWallet::OverviewScreen>(wallet));
  loop.run();

  return 0;
}