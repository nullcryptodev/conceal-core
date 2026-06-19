// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2016-2022, The Karbo developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "version.h"

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <fstream>
#include <iostream>

#include "DaemonCommandsHandler.h"
#include "Blockchain/BlockchainFilter.h"
#include "Blockchain/Checkpoints.h"
#include "Common/CommandLine.h"
#include "Common/PathTools.h"
#include "Common/SignalHandler.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/CoreConfig.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/MinerConfig.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "Logging/ConsoleLogger.h"
#include "Logging/LoggerManager.h"
#include "P2p/NetNode.h"
#include "P2p/NetNodeConfig.h"
#include "Rpc/RpcServer.h"
#include "Rpc/RpcServerConfig.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "crypto/hash.h"
#include "version.h"

#include "Blockchain/Blockchain.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "pow/GpuPowConfig.h"
#include "pow/mining/GpuMinerConfig.hpp"
#include "pow/backend.hpp"
#include "pow/pow_service.hpp"
#include "pow/pow_sync_log.hpp"

#ifdef CONCEAL_WITH_OPENCL
#include "pow/opencl/raii.hpp"
#endif

#if defined(WIN32)
#include <crtdbg.h>
#endif

using common::JsonValue;
using namespace cn;
using namespace logging;

namespace po = boost::program_options;

namespace
{
  const command_line::arg_descriptor<std::string> arg_config_file = {
      "config-file", "Specify configuration file", "conceal.conf"};
  const command_line::arg_descriptor<bool> arg_os_version = {"os-version", ""};
  const command_line::arg_descriptor<std::string> arg_log_file = {"log-file", "", ""};
  const command_line::arg_descriptor<std::string> arg_set_fee_address = {
      "fee-address", "Set a fee address for remote nodes", ""};
  const command_line::arg_descriptor<std::string> arg_set_view_key = {
      "view-key", "Set secret view-key for remote node fee confirmation", ""};
  const command_line::arg_descriptor<int> arg_log_level = {"log-level", "", 2};
  const command_line::arg_descriptor<bool> arg_console = {
      "no-console", "Disable daemon console commands"};
  const command_line::arg_descriptor<bool> arg_print_genesis_tx = {
      "print-genesis-tx", "Prints genesis' block tx hex to insert it to config and exits"};
  const command_line::arg_descriptor<bool> arg_export_snapshot = {
      "export-snapshot", "Export full blockchain snapshot for fast bootstrap", false};
  const command_line::arg_descriptor<std::string> arg_import_snapshot = {
      "import-snapshot", "Import full blockchain snapshot and bootstrap database", ""};
}

// ── Genesis tx printing ─────────────────────────────────────────────────

void print_genesis_tx_hex()
{
  logging::ConsoleLogger logger;
  cn::Transaction tx = cn::CurrencyBuilder(logger).generateGenesisTransaction();
  cn::BinaryArray txb = cn::toBinaryArray(tx);
  std::string tx_hex = common::toHex(txb);
  std::cout << "Random genesis hex: " << tx_hex << std::endl;
}

// ── Logger configuration ────────────────────────────────────────────────

JsonValue buildLoggerConfiguration(Level level, const std::string &logfile)
{
  JsonValue loggerConfiguration(JsonValue::OBJECT);
  loggerConfiguration.insert("globalLevel", static_cast<int64_t>(level));

  JsonValue &cfgLoggers = loggerConfiguration.insert("loggers", JsonValue::ARRAY);

  JsonValue &fileLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  fileLogger.insert("type", "file");
  fileLogger.insert("filename", logfile);
  fileLogger.insert("level", static_cast<int64_t>(TRACE));

  JsonValue &consoleLogger = cfgLoggers.pushBack(JsonValue::OBJECT);
  consoleLogger.insert("type", "console");
  consoleLogger.insert("level", static_cast<int64_t>(TRACE));
  consoleLogger.insert("pattern", "%T %L ");

  return loggerConfiguration;
}

// ── Snapshot export ─────────────────────────────────────────────────────

bool exportSnapshot(const std::string &dataDir, const std::string &outputFile,
                    logging::LoggerRef &logger)
{
  logger(INFO) << "Exporting full blockchain snapshot to " << outputFile;

  std::string dbPath = dataDir;
  if (!dbPath.empty() && dbPath.back() != '/')
    dbPath += '/';
  dbPath += "mdbx_blocks";

  CryptoNote::MDBXBlockchainStorage storage(dbPath);
  uint32_t topHeight = storage.topBlockHeight();

  if (topHeight == 0)
  {
    logger(ERROR) << "Blockchain is empty, nothing to export";
    return false;
  }

  logger(INFO) << "Blockchain height: " << topHeight;

  std::ofstream file(outputFile, std::ios::binary);
  if (!file)
  {
    logger(ERROR) << "Failed to open output file: " << outputFile;
    return false;
  }

  // Magic + version header
  const uint32_t magic = 0x43434E58; // "CCNX"
  const uint32_t version = 3;        // v3: includes filter records
  file.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char *>(&version), sizeof(version));
  file.write(reinterpret_cast<const char *>(&topHeight), sizeof(topHeight));

  // Write each block: [32-byte hash][4-byte entry_size][serialized BlockEntry][4-byte filter_size][serialized BlockFilterRecord]
  for (uint32_t h = 0; h <= topHeight; ++h)
  {
    if (h % 10000 == 0)
      logger(INFO) << "Exporting block " << h << "/" << topHeight;

    cn::BinaryArray ba;
    if (!storage.getBlockEntry(h, ba))
    {
      logger(ERROR) << "Failed to read block entry at height " << h;
      file.close();
      return false;
    }

    // Derive block hash from the entry
    cn::Blockchain::BlockEntry entry;
    if (!cn::fromBinaryArray(entry, ba))
    {
      logger(ERROR) << "Failed to deserialize block entry at height " << h;
      file.close();
      return false;
    }
    crypto::Hash blockHash = cn::get_block_hash(entry.bl);

    // Write hash + size + data
    file.write(reinterpret_cast<const char *>(blockHash.data), sizeof(crypto::Hash));
    uint32_t entrySize = static_cast<uint32_t>(ba.size());
    file.write(reinterpret_cast<const char *>(&entrySize), sizeof(entrySize));
    file.write(reinterpret_cast<const char *>(ba.data()), ba.size());

    // Export filter record if available
    cn::BlockFilterRecord filterRecord;
    bool hasFilter = storage.getBlockFilterRecord(h, filterRecord);
    cn::BinaryArray filterBa;
    if (hasFilter)
      filterBa = cn::toBinaryArray(filterRecord);
    uint32_t filterSize = static_cast<uint32_t>(filterBa.size());
    file.write(reinterpret_cast<const char *>(&filterSize), sizeof(filterSize));
    if (filterSize > 0)
      file.write(reinterpret_cast<const char *>(filterBa.data()), filterBa.size());
  }

  file.close();

  logger(INFO, BRIGHT_GREEN) << "Snapshot exported successfully: " << outputFile;
  logger(INFO) << "  Blocks: " << (topHeight + 1);
  return true;
}

// ── Snapshot import ─────────────────────────────────────────────────────

bool importSnapshot(const std::string &dataDir, const std::string &inputFile,
                    logging::LoggerRef &logger)
{
  logger(INFO) << "Importing blockchain snapshot from " << inputFile;

  std::ifstream file(inputFile, std::ios::binary);
  if (!file)
  {
    logger(ERROR) << "Failed to open input file: " << inputFile;
    return false;
  }

  uint32_t magic, version, topHeight;
  file.read(reinterpret_cast<char *>(&magic), sizeof(magic));
  file.read(reinterpret_cast<char *>(&version), sizeof(version));
  file.read(reinterpret_cast<char *>(&topHeight), sizeof(topHeight));

  if (magic != 0x43434E58)
  {
    logger(ERROR) << "Invalid snapshot file (bad magic)";
    return false;
  }
  if (version < 2 || version > 3)
  {
    logger(ERROR) << "Unsupported snapshot version: " << version
                  << " (expected version 2 or 3)";
    return false;
  }

  bool hasFilterRecords = (version >= 3);
  logger(INFO) << "Snapshot contains " << (topHeight + 1) << " blocks (0.." << topHeight << ")"
               << (hasFilterRecords ? " [with filter records]" : " [no filter records]");

  if (!tools::directoryExists(dataDir))
  {
    if (!tools::create_directories_if_necessary(dataDir))
    {
      logger(ERROR) << "Failed to create data directory: " << dataDir;
      return false;
    }
  }

  std::string dbPath = dataDir;
  if (!dbPath.empty() && dbPath.back() != '/')
    dbPath += '/';
  dbPath += "mdbx_blocks";

  // Remove any existing database
  boost::system::error_code ec;
  boost::filesystem::remove_all(dbPath, ec);

  CryptoNote::MDBXBlockchainStorage storage(dbPath);

  for (uint32_t h = 0; h <= topHeight; ++h)
  {
    if (h % 10000 == 0)
      logger(INFO) << "Importing block " << h << "/" << topHeight;

    crypto::Hash blockHash;
    uint32_t entrySize;

    file.read(reinterpret_cast<char *>(blockHash.data), sizeof(crypto::Hash));
    file.read(reinterpret_cast<char *>(&entrySize), sizeof(entrySize));

    if (!file)
    {
      logger(ERROR) << "Failed to read block header at height " << h << " from snapshot";
      return false;
    }

    cn::BinaryArray ba(entrySize);
    file.read(reinterpret_cast<char *>(ba.data()), entrySize);

    if (!file)
    {
      logger(ERROR) << "Failed to read block entry at height " << h << " from snapshot";
      return false;
    }

    // Deserialize to get the header
    cn::Blockchain::BlockEntry entry;
    if (!cn::fromBinaryArray(entry, ba))
    {
      logger(ERROR) << "Failed to deserialize block entry at height " << h;
      return false;
    }

    // Build header POD
    cn::BlockHeaderPOD hdr;
    hdr.majorVersion = entry.bl.majorVersion;
    hdr.minorVersion = entry.bl.minorVersion;
    hdr.timestamp = entry.bl.timestamp;
    hdr.previousBlockHash = entry.bl.previousBlockHash;
    hdr.nonce = entry.bl.nonce;
    hdr.blockCumulativeSize = entry.block_cumulative_size;
    hdr.cumulativeDifficulty = entry.cumulative_difficulty;
    hdr.alreadyGeneratedCoins = entry.already_generated_coins;
    hdr.height = entry.height;

    // Write to MDBX — single atomic transaction
    storage.pushCompleteBlock(h, blockHash, ba, hdr);

    // Import or build filter record
    try
    {
      if (hasFilterRecords)
      {
        // v3+: filter record is embedded in the snapshot
        uint32_t filterSize;
        file.read(reinterpret_cast<char *>(&filterSize), sizeof(filterSize));
        if (!file)
        {
          logger(ERROR) << "Failed to read filter size at height " << h;
          return false;
        }

        if (filterSize > 0)
        {
          cn::BinaryArray filterBa(filterSize);
          file.read(reinterpret_cast<char *>(filterBa.data()), filterSize);
          if (!file)
          {
            logger(ERROR) << "Failed to read filter record at height " << h;
            return false;
          }

          cn::BlockFilterRecord filterRecord;
          if (cn::fromBinaryArray(filterRecord, filterBa))
          {
            storage.storeBlockFilterRecord(h, filterRecord);
          }
        }
      }
      else
      {
        // v2: no filter records in snapshot — build them on import
        std::vector<cn::Transaction> allTxs;
        for (uint32_t i = 1; i < entry.transactions.size(); ++i)
          allTxs.push_back(entry.transactions[i].tx);

        cn::BlockFilterRecord filterRecord = buildBlockFilterRecord(
            entry.bl, entry.height, allTxs);
        storage.storeBlockFilterRecord(entry.height, filterRecord);
      }
    }
    catch (const std::exception &e)
    {
      logger(WARNING, BRIGHT_YELLOW)
          << "Filter record build failed for block " << h << ": " << e.what();
    }
  }

  file.close();

  logger(INFO, BRIGHT_GREEN) << "Snapshot imported successfully!";
  logger(INFO) << "  Blocks: " << (topHeight + 1);
  logger(INFO) << "  Filter records: " << (hasFilterRecords ? "imported" : "built on import");
  logger(INFO) << "  Database created at: " << dbPath;
  logger(INFO) << "  Start daemon normally to begin syncing remaining blocks.";

  return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
#ifdef _WIN32
  _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

  LoggerManager logManager;
  LoggerRef logger(logManager, "daemon");
  LoggerRef powLogger(logManager, "pow");

  try
  {
    po::options_description desc_cmd_only("Command line options");
    po::options_description desc_cmd_sett("Command line options and settings options");

    desc_cmd_sett.add_options()("enable-blockchain-indexes,i", po::bool_switch()->default_value(false),
                                "Enable blockchain indexes");

    command_line::add_arg(desc_cmd_only, command_line::arg_help);
    command_line::add_arg(desc_cmd_only, command_line::arg_version);
    command_line::add_arg(desc_cmd_only, arg_os_version);
    command_line::add_arg(desc_cmd_only, command_line::arg_data_dir,
                          tools::getDefaultDataDirectory());
    command_line::add_arg(desc_cmd_only, arg_config_file);
    command_line::add_arg(desc_cmd_sett, arg_set_fee_address);
    command_line::add_arg(desc_cmd_sett, arg_log_file);
    command_line::add_arg(desc_cmd_sett, arg_log_level);
    command_line::add_arg(desc_cmd_sett, arg_console);
    command_line::add_arg(desc_cmd_sett, arg_set_view_key);
    command_line::add_arg(desc_cmd_sett, command_line::arg_testnet_on);
    command_line::add_arg(desc_cmd_sett, arg_print_genesis_tx);
    command_line::add_arg(desc_cmd_sett, arg_export_snapshot);
    command_line::add_arg(desc_cmd_sett, arg_import_snapshot);

    RpcServerConfig::initOptions(desc_cmd_sett);
    NetNodeConfig::initOptions(desc_cmd_sett);
    MinerConfig::initOptions(desc_cmd_sett);
    GpuMinerConfig::initOptions(desc_cmd_sett);
    GpuPowConfig::initOptions(desc_cmd_sett);
    po::options_description desc_gpu_offload("GPU PoW offload tuning");
    GpuPowConfig::initOffloadOptions(desc_gpu_offload);

    po::options_description desc_options("Allowed options");
    desc_options.add(desc_cmd_only).add(desc_cmd_sett).add(desc_gpu_offload);

    po::variables_map vm;
    CoreConfig coreConfig;
    GpuPowConfig gpuPowConfig;

    bool r = command_line::handle_error_helper(desc_options, [&]()
                                               {
      po::store(po::parse_command_line(argc, argv, desc_options), vm);
      coreConfig.init(vm);
      gpuPowConfig.init(vm);

      if (command_line::get_arg(vm, command_line::arg_help))
      {
        std::cout << CCX_RELEASE_VERSION << std::endl << std::endl;
        std::cout << desc_cmd_only << std::endl << desc_cmd_sett;
        return false;
      }
      else if (command_line::get_arg(vm, command_line::arg_version))
      {
        std::cout << CCX_RELEASE_VERSION << std::endl;
        return false;
      }
      else if (command_line::get_arg(vm, arg_os_version))
      {
        std::cout << "OS " << tools::get_os_version_string() << std::endl;
        return false;
      }
      else if (command_line::get_arg(vm, arg_print_genesis_tx))
      {
        print_genesis_tx_hex();
        return false;
      }
      if (gpuPowConfig.listDevices)
      {
#ifdef CONCEAL_WITH_OPENCL
        std::cout << cn::ocl::getDeviceListText();
#else
        std::cout << "OpenCL not enabled at build time (rebuild with -DWITH_OPENCL=ON and OpenCL installed).\n";
#endif
        return false;
      }
      if (gpuPowConfig.showOffloadHelp)
      {
        std::cout << desc_gpu_offload;
        return false;
      }

      std::string data_dir = command_line::get_arg(vm, command_line::arg_data_dir);
      std::string config = command_line::get_arg(vm, arg_config_file);

      boost::filesystem::path data_dir_path(data_dir);
      boost::filesystem::path config_path(config);
      if (!config_path.has_parent_path())
        config_path = data_dir_path / config_path;

      boost::system::error_code ec;
      if (boost::filesystem::exists(config_path, ec))
      {
        po::options_description desc_config(desc_cmd_sett);
        desc_config.add(desc_gpu_offload);
        po::store(po::parse_config_file<char>(config_path.string<std::string>().c_str(),
                                               desc_config), vm);
      }

      po::notify(vm);
      gpuPowConfig.init(vm);
      return true; });

    if (!r)
      return 1;

    auto modulePath = common::NativePathToGeneric(argv[0]);
    auto cfgLogFile = common::NativePathToGeneric(command_line::get_arg(vm, arg_log_file));

    if (cfgLogFile.empty())
      cfgLogFile = common::ReplaceExtenstion(modulePath, ".log");
    else if (!common::HasParentPath(cfgLogFile))
      cfgLogFile = common::CombinePath(common::GetPathDirectory(modulePath), cfgLogFile);

    auto cfgLogLevel = static_cast<Level>(static_cast<int>(logging::ERROR) +
                                          command_line::get_arg(vm, arg_log_level));
    logManager.configure(buildLoggerConfiguration(cfgLogLevel, cfgLogFile));

    logger(INFO, BRIGHT_YELLOW) << CCX_RELEASE_VERSION;
    logger(INFO) << "Module folder: " << argv[0];

    // Handle snapshot export (doesn't require full init)
    if (command_line::get_arg(vm, arg_export_snapshot))
    {
      std::string dataDir = command_line::get_arg(vm, command_line::arg_data_dir);
      boost::filesystem::path snapPath = boost::filesystem::current_path();
      snapPath /= "conceal-snapshot.dat";
      bool ok = exportSnapshot(dataDir, snapPath.string(), logger);
      return ok ? 0 : 1;
    }

    // Handle snapshot import (doesn't require full init)
    std::string importFile = command_line::get_arg(vm, arg_import_snapshot);
    if (!importFile.empty())
    {
      std::string dataDir = command_line::get_arg(vm, command_line::arg_data_dir);
      bool ok = importSnapshot(dataDir, importFile, logger);
      return ok ? 0 : 1;
    }

    logger(INFO) << "Blockchain and configuration folder: " << coreConfig.configFolder;
    if (coreConfig.testnet)
      logger(INFO, MAGENTA) << "/!\\ Starting in testnet mode /!\\";

    // Currency validation
    cn::CurrencyBuilder currencyBuilder(logManager);
    currencyBuilder.testnet(coreConfig.testnet);

    try
    {
      currencyBuilder.currency();
    }
    catch (const std::exception &)
    {
      logger(ERROR) << "Incorrect genesis hash! Do not change the genesis hash: "
                    << cn::GENESIS_COINBASE_TX_HEX;
      return 1;
    }

    cn::Currency currency = currencyBuilder.currency();

    {
      auto powBackend = cn::createPowVerifyBackend(gpuPowConfig.deviceIndex, gpuPowConfig.batchSize,
                                                   gpuPowConfig.minBatchSize, gpuPowConfig.maxWaitUs,
                                                   gpuPowConfig.debugCrossCheck, gpuPowConfig.debugInnerTrace);
      cn::PowService::instance().init(std::move(powBackend), gpuPowConfig, &powLogger);
      if (gpuPowConfig.deviceIndex >= 0) {
        logger(INFO) << "CN-GPU speculative policy: batch=" << gpuPowConfig.batchSize
                     << " min_batch="
                     << (gpuPowConfig.minBatchSizeUserSet ? std::to_string(gpuPowConfig.minBatchSize)
                                                          : std::string("auto"))
                     << " max_wait_us="
                     << (gpuPowConfig.maxWaitUsUserSet ? std::to_string(gpuPowConfig.maxWaitUs)
                                                       : std::string("auto"))
                     << " prefetch depth="
                     << (gpuPowConfig.prefetchDepthUserSet ? std::to_string(gpuPowConfig.prefetchQueueDepth)
                                                           : std::string("auto"))
                     << " window="
                     << (gpuPowConfig.prefetchWindowUserSet ? std::to_string(gpuPowConfig.prefetchWindow)
                                                            : std::string("auto"))
                     << " backlog threshold=" << gpuPowConfig.backlogThreshold
                     << " trust_gpu_cache=" << (gpuPowConfig.trustGpuCache ? "yes" : "no");
      }
    }

    cn::core ccore(currency, nullptr, logManager,
                   vm["enable-blockchain-indexes"].as<bool>());

    cn::Checkpoints checkpoints(logManager);
    checkpoints.set_testnet(coreConfig.testnet);
    checkpoints.load_checkpoints();
    checkpoints.load_checkpoints_from_dns();
    ccore.set_checkpoints(std::move(checkpoints));

    NetNodeConfig netNodeConfig;
    netNodeConfig.init(vm);
    netNodeConfig.setTestnet(coreConfig.testnet);
    netNodeConfig.setConfigFolder(coreConfig.configFolder);

    MinerConfig minerConfig;
    minerConfig.init(vm);

    GpuMinerConfig gpuMinerConfig;
    gpuMinerConfig.init(vm);
    applyMiningModeExclusivity(minerConfig.startMining, gpuMinerConfig,
                               [&](const std::string& msg) { logger(WARNING, BRIGHT_YELLOW) << msg; });

    RpcServerConfig rpcConfig;
    rpcConfig.init(vm);

    if (!coreConfig.configFolderDefaulted)
    {
      if (!tools::directoryExists(coreConfig.configFolder))
        throw std::runtime_error("Directory does not exist: " + coreConfig.configFolder);
    }
    else
    {
      if (!tools::create_directories_if_necessary(coreConfig.configFolder))
        throw std::runtime_error("Can't create directory: " + coreConfig.configFolder);
    }

    platform_system::Dispatcher dispatcher;

    cn::CryptoNoteProtocolHandler cprotocol(currency, dispatcher, ccore, nullptr, logManager);
    cn::NodeServer p2psrv(dispatcher, cprotocol, logManager);
    cn::RpcServer rpcServer(dispatcher, logManager, ccore, p2psrv, cprotocol);

    cprotocol.set_p2p_endpoint(&p2psrv);
    ccore.set_cryptonote_protocol(&cprotocol);

    DaemonCommandsHandler dch(ccore, p2psrv, logManager);

    // Initialize P2P
    logger(INFO) << "Initializing p2p server...";
    if (!p2psrv.init(netNodeConfig))
    {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize p2p server.";
      return 1;
    }
    logger(INFO) << "P2p server initialized OK";

    if (gpuPowConfig.deviceIndex >= 0)
      cn::PowService::instance().updatePrefetchForConnections(p2psrv.getTargetOutgoingConnectionsCount());

    // Initialize core
    logger(INFO) << "Initializing core...";
    if (!ccore.init(coreConfig, minerConfig, gpuMinerConfig, gpuPowConfig.deviceIndex, true))
    {
      logger(ERROR, BRIGHT_RED) << "Failed to initialize core";
      return 1;
    }
    logger(INFO) << "Core initialized OK";

    if (gpuMinerConfig.enabled())
    {
      logger(INFO) << "GPU mining configured for address " << gpuMinerConfig.rewardAddress;
      for (const auto& dev : gpuMinerConfig.devices)
      {
        logger(INFO) << "  GPU " << dev.deviceIndex << ": intensity " << dev.userIntensity << " -> 3x"
                     << dev.perThreadIntensity << " (worksize " << GpuMinerConfig::kWorkSize << ")";
      }
    }

    // Start console handler
    if (!command_line::has_arg(vm, arg_console))
      dch.start_handling();

    // Configure RPC
    logger(INFO) << "Starting core rpc server on address " << rpcConfig.getBindAddress();

    if (command_line::has_arg(vm, arg_set_fee_address))
    {
      std::string addr_str = command_line::get_arg(vm, arg_set_fee_address);
      if (!addr_str.empty())
      {
        AccountPublicAddress acc = boost::value_initialized<AccountPublicAddress>();
        if (!currency.parseAccountAddressString(addr_str, acc))
        {
          logger(ERROR, BRIGHT_RED) << "Bad fee address: " << addr_str;
          return 1;
        }
        rpcServer.setFeeAddress(addr_str, acc);
        logger(INFO, BRIGHT_YELLOW) << "Remote node fee address set: " << addr_str;
      }
    }

    if (command_line::has_arg(vm, arg_set_view_key))
    {
      std::string vk_str = command_line::get_arg(vm, arg_set_view_key);
      if (!vk_str.empty())
      {
        rpcServer.setViewKey(vk_str);
        logger(INFO, BRIGHT_YELLOW) << "Secret view key set: " << vk_str;
      }
    }

    rpcServer.start(rpcConfig.bindIp, rpcConfig.bindPort);
    rpcServer.enableCors(rpcConfig.enableCors);
    logger(INFO) << "Core rpc server started ok";

    // Signal handling
    tools::SignalHandler::install([&dch, &p2psrv]
                                  {
      dch.stop_handling();
      p2psrv.sendStopSignal(); });

    // Run
    logger(INFO) << "Starting p2p net loop...";
    p2psrv.run();
    logger(INFO) << "p2p net loop stopped";

    // Shutdown
    dch.stop_handling();
    logger(INFO) << "Stopping core rpc server...";
    rpcServer.stop();
    logger(INFO) << "Stopping PoW GPU worker...";
    cn::PowService::instance().shutdown();
    logger(INFO) << "Deinitializing core...";
    ccore.deinit();
    logger(INFO) << "Deinitializing p2p...";
    p2psrv.deinit();

    ccore.set_cryptonote_protocol(nullptr);
    cprotocol.set_p2p_endpoint(nullptr);
  }
  catch (const std::exception &e)
  {
    logger(ERROR, BRIGHT_RED) << "Exception: " << e.what();
    return 1;
  }

  logger(INFO) << "Node stopped.";
  return 0;
}