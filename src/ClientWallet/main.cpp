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

#include "crypto/crypto.h"

#include "CryptoNoteCore/Currency.h"

#include "Logging/ConsoleLogger.h"

#include "NodeClient/NodeClient.h"
#include "Rpc/HttpClient.h"
#include "System/Dispatcher.h"

#include "INode.h"

#include <iostream>
#include <memory>
#include <string>
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
  std::string walletStatePath;
  uint32_t startHeight = 0;
};

enum class StartupChoice
{
  CreateNew,
  ImportKeys,
  OpenState,
  Quit
};

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
    case 1000:
      if (selected > 0)
        selected--;
      break;
    case 1001:
      if (selected < (int)items.size() - 1)
        selected++;
      break;
    case 10:
    case 13:
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
    case 1000:
      if (selected > 0)
        selected--;
      break;
    case 1001:
      if (selected < 2)
        selected++;
      break;
    case 10:
    case 13:
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
    case 27:
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
    if (key == 10 || key == 13 || key == 'q' || key == 'Q')
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
  cfg.mode = ConnectionMode::Offline;
  return true;
}

int main(int argc, char *argv[])
{
  Config cfg;
  bool hasKeys = false;

  if (argc > 1)
  {
    if (argc >= 2 && strlen(argv[1]) == 64)
    {
      cfg.viewKeyHex = argv[1];
      hasKeys = true;
    }
    if (argc >= 3 && strlen(argv[2]) == 64)
    {
      cfg.spendKeyHex = argv[2];
    }
    for (int i = 1; i < argc; ++i)
    {
      std::string arg = argv[i];
      if (arg == "--daemon" && i + 1 < argc)
      {
        std::string hp = argv[++i];
        size_t colon = hp.find(':');
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
        cfg.mode = ConnectionMode::Local;
      }
      else if (arg == "--state" && i + 1 < argc)
      {
        cfg.stateFile = argv[++i];
        cfg.mode = ConnectionMode::Offline;
      }
      else if (arg == "--start-height" && i + 1 < argc)
      {
        cfg.startHeight = static_cast<uint32_t>(std::stoul(argv[++i]));
      }
    }
  }

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

  if (!cfg.dataPath.empty() && cfg.dataPath[0] == '~')
  {
    const char *home = std::getenv("HOME");
    if (home)
      cfg.dataPath = std::string(home) + cfg.dataPath.substr(1);
  }

  cfg.walletStatePath = cfg.dataPath + "/wallet_state.bin";

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

  if (cfg.mode == ConnectionMode::Remote)
  {
    auto *client = new NodeClient::NodeClient(cfg.daemonHost, cfg.daemonPort);
    remoteNode.reset(client);
    nodePtr = remoteNode.get();

    // Init the connection
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

  auto sync = std::make_shared<ClientWallet::SyncEngine>(
      cfg.dataPath, viewKey, spendPub,
      cfg.spendKeyHex.empty() ? nullptr : &spendKey);

  if (cfg.startHeight > 0)
  {
    sync->setScannedHeight(cfg.startHeight);
  }

  if (cfg.mode == ConnectionMode::Remote)
  {
    sync->setNode(nodePtr);

    // NodeClient handles all daemon communication internally.
    // SyncEngine uses cn::INode interface directly.
  }

  if (!cfg.stateFile.empty())
  {
    sync->loadStateFile(cfg.stateFile);
    cfg.walletStatePath = cfg.stateFile;
  }

  ClientWallet::EventLoop loop;
  loop.setWallet(wallet);
  loop.setSyncEngine(sync);
  loop.setNode(nodePtr);
  loop.setCurrency(currency);
  loop.setStatePath(cfg.walletStatePath);

  loop.pushScreen(std::make_shared<ClientWallet::OverviewScreen>(wallet));
  loop.run();

  return 0;
}