// Config.h — wallet configuration
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <string>

struct Config
{
  std::string dataDir;
  std::string daemonHost = "127.0.0.1";
  uint16_t daemonPort = 16000;
  std::string viewKeyHex;
  std::string spendKeyHex;
  unsigned int scanThreads = 1;
  bool skipScan = false;
  std::string sidechainHost = "127.0.0.1";
  uint16_t sidechainPort = 8080;
  bool sidechainTestnet = false;
  std::string dexHost = "127.0.0.1";
  uint16_t dexPort = 8090;
};

bool parseArgs(int argc, char *argv[], Config &cfg);