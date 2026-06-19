// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "DaemonCommandsHandler.h"

#include "Common/Util.h"
#include "CryptoNoteCore/Core.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Miner.h"
#include "pow/mining/GpuMiner.hpp"
#include "pow/mining/GpuMinerConfig.hpp"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "P2p/NetNode.h"
#include "Serialization/SerializationTools.h"
#include "Storage/MDBXBlockchainStorage.h"
#include "version.h"
#include "CryptoNoteCore/CryptoNoteTools.h"

#include <boost/filesystem.hpp>
#include <fstream>

namespace
{
  template <typename T>
  static bool print_as_json(const T &obj)
  {
    std::cout << cn::storeToJson(obj) << ENDL;
    return true;
  }
} // namespace

DaemonCommandsHandler::DaemonCommandsHandler(cn::core &core, cn::NodeServer &srv,
                                             logging::LoggerManager &log)
    : m_core(core), m_srv(srv), logger(log, "daemon"), m_logManager(log)
{
  m_consoleHandler.setHandler("exit", boost::bind(&DaemonCommandsHandler::exit, this, boost::arg<1>()),
                              "Shutdown the daemon");
  m_consoleHandler.setHandler("help", boost::bind(&DaemonCommandsHandler::help, this, boost::arg<1>()),
                              "Show this help");
  m_consoleHandler.setHandler("save", boost::bind(&DaemonCommandsHandler::save, this, boost::arg<1>()),
                              "Save the Blockchain data safely");
  m_consoleHandler.setHandler("print_pl", boost::bind(&DaemonCommandsHandler::print_pl, this, boost::arg<1>()),
                              "Print peer list");
  m_consoleHandler.setHandler("rollback_chain", boost::bind(&DaemonCommandsHandler::rollback_chain, this, boost::arg<1>()),
                              "Rollback chain to specific height, rollback_chain <height>");
  m_consoleHandler.setHandler("print_cn", boost::bind(&DaemonCommandsHandler::print_cn, this, boost::arg<1>()),
                              "Print connections");
  m_consoleHandler.setHandler("print_bci", boost::bind(&DaemonCommandsHandler::print_bci, this, boost::arg<1>()),
                              "Print blockchain current height");
  m_consoleHandler.setHandler("print_bc", boost::bind(&DaemonCommandsHandler::print_bc, this, boost::arg<1>()),
                              "Print blockchain info in a given blocks range, print_bc <begin_height> [<end_height>]");
  m_consoleHandler.setHandler("print_block", boost::bind(&DaemonCommandsHandler::print_block, this, boost::arg<1>()),
                              "Print block, print_block <block_hash> | <block_height>");
  m_consoleHandler.setHandler("print_stat", boost::bind(&DaemonCommandsHandler::print_stat, this, boost::arg<1>()),
                              "Print statistics, print_stat <nothing=last> | <block_hash> | <block_height>");
  m_consoleHandler.setHandler("print_tx", boost::bind(&DaemonCommandsHandler::print_tx, this, boost::arg<1>()),
                              "Print transaction, print_tx <transaction_hash>");
  m_consoleHandler.setHandler("start_mining", boost::bind(&DaemonCommandsHandler::start_mining, this, boost::arg<1>()),
                              "Start mining for specified address, start_mining <addr> [threads=1]");
  m_consoleHandler.setHandler("stop_mining", boost::bind(&DaemonCommandsHandler::stop_mining, this, boost::arg<1>()),
                              "Stop mining");
  m_consoleHandler.setHandler("start_gpu_mining",
                              boost::bind(&DaemonCommandsHandler::start_gpu_mining, this, boost::arg<1>()),
                              "Start GPU mining, start_gpu_mining <addr,deviceId:intensity|safe|boost[,...]>");
  m_consoleHandler.setHandler("stop_gpu_mining",
                              boost::bind(&DaemonCommandsHandler::stop_gpu_mining, this, boost::arg<1>()),
                              "Stop GPU mining");
  m_consoleHandler.setHandler("print_pool", boost::bind(&DaemonCommandsHandler::print_pool, this, boost::arg<1>()),
                              "Print transaction pool (long format)");
  m_consoleHandler.setHandler("print_pool_sh", boost::bind(&DaemonCommandsHandler::print_pool_sh, this, boost::arg<1>()),
                              "Print transaction pool (short format)");
  m_consoleHandler.setHandler("show_hr", boost::bind(&DaemonCommandsHandler::show_hr, this, boost::arg<1>()),
                              "Start showing hash rate");
  m_consoleHandler.setHandler("hide_hr", boost::bind(&DaemonCommandsHandler::hide_hr, this, boost::arg<1>()),
                              "Stop showing hash rate");
  m_consoleHandler.setHandler("set_log", boost::bind(&DaemonCommandsHandler::set_log, this, boost::arg<1>()),
                              "set_log <level> - Change current log level, <level> is a number 0-4");
  m_consoleHandler.setHandler("export_snapshot", boost::bind(&DaemonCommandsHandler::export_snapshot, this, boost::arg<1>()),
                              "Export full blockchain snapshot to file, export_snapshot <file_path>");
  m_consoleHandler.setHandler("export_headers", boost::bind(&DaemonCommandsHandler::export_headers, this, boost::arg<1>()),
                              "Export all block headers to a binary file for light client bootstrap, export_headers <file_path>");
}

// ── Help ──────────────────────────────────────────────────────────────

const std::string DaemonCommandsHandler::get_commands_str()
{
  std::stringstream ss;
  ss << CCX_RELEASE_VERSION << std::endl;
  ss << "Commands: " << std::endl;
  std::string usage = m_consoleHandler.getUsage();
  boost::replace_all(usage, "\n", "\n  ");
  usage.insert(0, "  ");
  ss << usage << std::endl;
  return ss.str();
}

bool DaemonCommandsHandler::help(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"help\"";
    return true;
  }
  logger(logging::INFO) << get_commands_str();
  return true;
}

// ── Exit ──────────────────────────────────────────────────────────────

bool DaemonCommandsHandler::exit(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"exit\"";
    return true;
  }
  m_consoleHandler.requestStop();
  m_srv.sendStopSignal();
  return true;
}

// ── Save ──────────────────────────────────────────────────────────────

bool DaemonCommandsHandler::save(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"save\"";
    return true;
  }
  if (!m_core.saveBlockchain())
  {
    logger(logging::ERROR) << "Could not save the blockchain data!";
    return false;
  }
  return true;
}

// ── Peer list ─────────────────────────────────────────────────────────

bool DaemonCommandsHandler::print_pl(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"print_pl\"";
    return true;
  }
  m_srv.logPeerlist();
  return true;
}

// ── Hash rate ─────────────────────────────────────────────────────────

bool DaemonCommandsHandler::show_hr(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"show_hr\"";
    return true;
  }
  if (!m_core.get_miner().is_mining() && !m_core.get_gpu_miner().is_mining())
    logger(logging::WARNING) << "Mining is not started. Start mining first.";
  else
  {
    m_core.get_miner().do_print_hashrate(true);
    m_core.get_gpu_miner().do_print_hashrate(true);
  }
  return true;
}

bool DaemonCommandsHandler::hide_hr(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"hide_hr\"";
    return true;
  }
  m_core.get_miner().do_print_hashrate(false);
  m_core.get_gpu_miner().do_print_hashrate(false);
  return true;
}

// ── Connections ───────────────────────────────────────────────────────

bool DaemonCommandsHandler::print_cn(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"print_cn\"";
    return true;
  }
  m_srv.getPayloadObject().log_connections();
  return true;
}

// ── Blockchain printing ───────────────────────────────────────────────

bool DaemonCommandsHandler::print_bc(const std::vector<std::string> &args)
{
  if (args.empty())
  {
    logger(logging::ERROR) << "Usage: \"print_bc <block_from> [<block_to>]\"";
    return false;
  }

  uint32_t start_index = 0;
  if (!common::fromString(args[0], start_index))
  {
    logger(logging::ERROR) << "Incorrect start block!";
    return false;
  }

  uint32_t end_index = m_core.get_current_blockchain_height();
  if (args.size() > 1 && !common::fromString(args[1], end_index))
  {
    logger(logging::ERROR) << "Incorrect end block!";
    return false;
  }

  if (end_index > m_core.get_current_blockchain_height())
  {
    logger(logging::ERROR) << "End block shouldn't be greater than "
                           << m_core.get_current_blockchain_height();
    return false;
  }

  if (end_index <= start_index)
  {
    logger(logging::ERROR) << "End block should be greater than start block";
    return false;
  }

  m_core.print_blockchain(start_index, end_index);
  return true;
}

bool DaemonCommandsHandler::print_bci(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"print_bci\"";
    return true;
  }
  m_core.print_blockchain_index(false);
  return true;
}

bool DaemonCommandsHandler::print_block_by_height(uint32_t height)
{
  std::list<cn::Block> blocks;
  m_core.get_blocks(height, 1, blocks);

  if (blocks.size() == 1)
  {
    logger(logging::INFO) << "block_id: " << get_block_hash(blocks.front());
    print_as_json(blocks.front());
  }
  else
  {
    uint32_t current_height;
    crypto::Hash top_id;
    m_core.get_blockchain_top(current_height, top_id);
    logger(logging::ERROR) << "Block wasn't found. Current height: "
                           << current_height << ", requested: " << height;
    return false;
  }
  return true;
}

bool DaemonCommandsHandler::print_block_by_hash(const std::string &arg)
{
  crypto::Hash block_hash;
  if (!parse_hash256(arg, block_hash))
    return false;

  std::list<crypto::Hash> block_ids{block_hash};
  std::list<cn::Block> blocks;
  std::list<crypto::Hash> missed_ids;
  m_core.get_blocks(block_ids, blocks, missed_ids);

  if (blocks.size() == 1)
    print_as_json(blocks.front());
  else
  {
    logger(logging::ERROR) << "Block wasn't found: " << arg;
    return false;
  }
  return true;
}

bool DaemonCommandsHandler::print_block(const std::vector<std::string> &args)
{
  if (args.empty())
  {
    logger(logging::ERROR) << "Usage: print_block <block_hash> | <block_height>";
    return true;
  }

  const std::string &arg = args.front();
  try
  {
    uint32_t height = boost::lexical_cast<uint32_t>(arg);
    print_block_by_height(height);
  }
  catch (boost::bad_lexical_cast &)
  {
    print_block_by_hash(arg);
  }
  return true;
}

// ── Statistics ────────────────────────────────────────────────────────

std::string DaemonCommandsHandler::calculatePercent(const cn::Currency &currency,
                                                    uint64_t value, uint64_t total)
{
  double percent = 100.0 * static_cast<double>(value) / static_cast<double>(total);
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << percent << "%";
  return oss.str();
}

bool DaemonCommandsHandler::print_stat(const std::vector<std::string> &args)
{
  uint32_t height = 0;
  uint32_t max_height = m_core.get_current_blockchain_height() - 1;

  if (args.empty())
  {
    height = max_height;
  }
  else
  {
    try
    {
      height = boost::lexical_cast<uint32_t>(args.front());
    }
    catch (boost::bad_lexical_cast &)
    {
      crypto::Hash block_hash;
      if (!parse_hash256(args.front(), block_hash) ||
          !m_core.getBlockHeight(block_hash, height))
        return false;
    }
    if (height > max_height)
    {
      logger(logging::INFO) << "Showing stats for last available block: " << max_height;
      height = max_height;
    }
  }

  const auto &currency = m_core.currency();

  uint64_t block_difficulty = m_core.blockDifficulty(height);
  uint64_t total_coins = m_core.coinsEmittedAtHeight(height);
  uint64_t staked_coins_total = m_core.depositAmountAtHeight(height);
  uint64_t active_coins = total_coins - staked_coins_total;

  std::string coins_minted = currency.formatAmount(total_coins) + " (" +
                             calculatePercent(currency, total_coins, cn::parameters::MONEY_SUPPLY) + ")";
  std::string staked_coins = currency.formatAmount(staked_coins_total) + " (" +
                             calculatePercent(currency, staked_coins_total, total_coins) + ")";
  std::string circulation = currency.formatAmount(active_coins) + " (" +
                            calculatePercent(currency, active_coins, total_coins) + ")";
  std::string rewards_paid = currency.formatAmount(m_core.depositInterestAtHeight(height));
  std::string database_stats = m_core.printDatabaseStats();

  std::ostringstream oss;
  oss << "\n=== Network Status ===\n"
      << "Block Height: " << height << "\n"
      << "Block Difficulty: " << block_difficulty << "\n"
      << "Coins Minted:  " << coins_minted << "\n"
      << "Staked Coins: " << staked_coins << "\n"
      << "Circulation Supply:  " << circulation << "\n"
      << "Staked Rewards Paid: " << rewards_paid << "\n\n"
      << "=== Database Status ===\n"
      << database_stats;

  logger(logging::INFO) << oss.str();
  return true;
}

// ── Transaction printing ──────────────────────────────────────────────

bool DaemonCommandsHandler::print_tx(const std::vector<std::string> &args)
{
  if (args.empty())
  {
    logger(logging::ERROR) << "Usage: print_tx <transaction hash>";
    return true;
  }

  crypto::Hash tx_hash;
  if (!parse_hash256(args.front(), tx_hash))
    return true;

  std::vector<crypto::Hash> tx_ids{tx_hash};
  std::list<cn::Transaction> txs;
  std::list<crypto::Hash> missed_ids;
  m_core.getTransactions(tx_ids, txs, missed_ids, true);

  if (txs.size() == 1)
    print_as_json(txs.front());
  else
    logger(logging::ERROR) << "Transaction was not found: <" << args.front() << ">";

  return true;
}

// ── Transaction pool ─────────────────────────────────────────────────

bool DaemonCommandsHandler::print_pool(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"print_pool\"";
    return true;
  }
  logger(logging::INFO) << "Pool state: \n"
                        << m_core.print_pool(false);
  return true;
}

bool DaemonCommandsHandler::print_pool_sh(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"print_pool_sh\"";
    return true;
  }
  logger(logging::INFO) << "Pool state: \n"
                        << m_core.print_pool(true);
  return true;
}

// ── Mining ────────────────────────────────────────────────────────────

bool DaemonCommandsHandler::start_mining(const std::vector<std::string> &args)
{
  if (args.empty())
  {
    logger(logging::ERROR) << "Usage: start_mining <addr> [threads=1]";
    return true;
  }

  if (!m_core.currency().isTestnet() && !m_srv.getPayloadObject().isSynchronized())
  {
    logger(logging::ERROR) << "Cannot mine while synchronizing; wait until the chain is caught up";
    return true;
  }

  if (m_core.get_gpu_miner().is_mining())
  {
    logger(logging::ERROR) << "GPU mining is active; stop it before starting CPU mining";
    return true;
  }

  cn::AccountPublicAddress adr;
  if (!m_core.currency().parseAccountAddressString(args.front(), adr))
  {
    logger(logging::ERROR) << "Invalid wallet address!";
    return true;
  }

  size_t threads_count = 1;
  if (args.size() > 1)
  {
    bool ok = common::fromString(args[1], threads_count);
    threads_count = (ok && threads_count > 0) ? threads_count : 1;
  }

  m_core.get_miner().start(adr, threads_count);
  return true;
}

bool DaemonCommandsHandler::stop_mining(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"stop_mining\"";
    return true;
  }
  m_core.get_miner().stop();
  return true;
}

bool DaemonCommandsHandler::start_gpu_mining(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    logger(logging::ERROR) << "Usage: start_gpu_mining <addr,deviceId:intensity|safe|boost[,...]> "
                              "(same format as --start-gpu-mining)";
    return true;
  }

  if (!m_core.currency().isTestnet() && !m_srv.getPayloadObject().isSynchronized())
  {
    logger(logging::ERROR) << "Cannot mine while synchronizing; wait until the chain is caught up";
    return true;
  }

  if (m_core.get_miner().is_mining())
  {
    logger(logging::ERROR) << "CPU mining is active; stop it before starting GPU mining";
    return true;
  }

  std::string reward;
  std::vector<cn::GpuDeviceSpec> devices;
  std::string err;
  if (!cn::GpuMinerConfig::parseValue(args[0], reward, devices, err))
  {
    logger(logging::ERROR) << "Invalid GPU mining spec: " << err;
    return true;
  }

  cn::AccountPublicAddress adr;
  if (!m_core.currency().parseAccountAddressString(reward, adr))
  {
    logger(logging::ERROR) << "Invalid wallet address!";
    return true;
  }

  if (!m_core.get_gpu_miner().start(adr, devices))
    logger(logging::ERROR) << "Failed to start GPU mining";
  return true;
}

bool DaemonCommandsHandler::stop_gpu_mining(const std::vector<std::string> &args)
{
  if (!args.empty())
  {
    logger(logging::ERROR) << "Usage: \"stop_gpu_mining\"";
    return true;
  }
  m_core.get_gpu_miner().stop();
  return true;
}

// ── Log level ─────────────────────────────────────────────────────────

bool DaemonCommandsHandler::set_log(const std::vector<std::string> &args)
{
  if (args.size() != 1)
  {
    logger(logging::ERROR) << "Usage: \"set_log <0-4>\"";
    return true;
  }

  uint16_t l = 0;
  if (!common::fromString(args[0], l))
  {
    logger(logging::ERROR) << "Incorrect number format, use: set_log <0-4>";
    return true;
  }

  ++l;
  if (l > logging::TRACE)
  {
    logger(logging::ERROR) << "Incorrect number range, use: set_log <0-4>";
    return true;
  }

  m_logManager.setMaxLevel(static_cast<logging::Level>(l));
  return true;
}

// ── Rollback ──────────────────────────────────────────────────────────

bool DaemonCommandsHandler::rollback_chain(const std::vector<std::string> &args)
{
  if (args.empty())
  {
    logger(logging::ERROR) << "Usage: \"rollback_chain <block_height>\"";
    return true;
  }

  uint32_t height = boost::lexical_cast<uint32_t>(args.front());
  m_core.rollback_chain_to(height);
  return true;
}

// ── Snapshot export (console command) ─────────────────────────────────

bool DaemonCommandsHandler::export_snapshot(const std::vector<std::string> &args)
{
  logger(logging::INFO) << "Exporting full blockchain snapshot...";

  std::string outputFile;
  if (args.empty())
  {
    boost::filesystem::path snapPath = boost::filesystem::current_path();
    snapPath /= "conceal-snapshot.dat";
    outputFile = snapPath.string();
  }
  else
  {
    outputFile = args.front();
  }

  std::string dataDir = m_core.get_config_folder();
  std::string dbPath = dataDir;
  if (!dbPath.empty() && dbPath.back() != '/')
    dbPath += '/';
  dbPath += "mdbx_blocks";

  CryptoNote::MDBXBlockchainStorage storage(dbPath);
  uint32_t topHeight = storage.topBlockHeight();

  if (topHeight == 0)
  {
    logger(logging::ERROR) << "Blockchain is empty, nothing to export";
    return true;
  }

  logger(logging::INFO) << "Blockchain height: " << topHeight;

  std::ofstream file(outputFile, std::ios::binary);
  if (!file)
  {
    logger(logging::ERROR) << "Failed to open output file: " << outputFile;
    return true;
  }

  // Magic + version header
  const uint32_t magic = 0x43434E58; // "CCNX"
  const uint32_t version = 2;        // v2: full block entries
  file.write(reinterpret_cast<const char *>(&magic), sizeof(magic));
  file.write(reinterpret_cast<const char *>(&version), sizeof(version));
  file.write(reinterpret_cast<const char *>(&topHeight), sizeof(topHeight));

  // Write each block: [32-byte hash][4-byte entry_size][serialized BlockEntry]
  for (uint32_t h = 0; h <= topHeight; ++h)
  {
    if (h % 10000 == 0)
      logger(logging::INFO) << "Exporting block " << h << "/" << topHeight;

    cn::BinaryArray ba;
    if (!storage.getBlockEntry(h, ba))
    {
      logger(logging::ERROR) << "Failed to read block entry at height " << h;
      file.close();
      return true;
    }

    // Derive block hash from the entry
    cn::Blockchain::BlockEntry entry;
    if (!cn::fromBinaryArray(entry, ba))
    {
      logger(logging::ERROR) << "Failed to deserialize block entry at height " << h;
      file.close();
      return true;
    }
    crypto::Hash blockHash = cn::get_block_hash(entry.bl);

    // Write hash + size + data
    file.write(reinterpret_cast<const char *>(blockHash.data), sizeof(crypto::Hash));
    uint32_t entrySize = static_cast<uint32_t>(ba.size());
    file.write(reinterpret_cast<const char *>(&entrySize), sizeof(entrySize));
    file.write(reinterpret_cast<const char *>(ba.data()), ba.size());
  }

  file.close();

  logger(logging::INFO, logging::BRIGHT_GREEN)
      << "Snapshot exported: " << outputFile
      << " (" << (topHeight + 1) << " blocks)";

  return true;
}

// ── Headers export (light client bootstrap) ───────────────────────────

#pragma pack(push, 1)
struct ExportedHeader
{
  uint32_t height;
  uint8_t majorVersion;
  uint8_t minorVersion;
  uint64_t timestamp;
  uint32_t nonce;
  uint64_t cumulativeDifficulty;
  uint64_t alreadyGeneratedCoins;
  crypto::Hash previousBlockHash;
  crypto::Hash blockHash;
};
#pragma pack(pop)

bool DaemonCommandsHandler::export_headers(const std::vector<std::string> &args)
{
  std::string outputFile;

  if (args.empty())
  {
    boost::filesystem::path headersPath = boost::filesystem::current_path();
    headersPath /= "conceal-headers.bin";
    outputFile = headersPath.string();
  }
  else
  {
    outputFile = args.front();
  }

  logger(logging::INFO) << "Exporting headers to " << outputFile;

  uint32_t blockchainHeight = m_core.get_current_blockchain_height();
  if (blockchainHeight == 0)
  {
    logger(logging::ERROR) << "Blockchain not initialized";
    return true;
  }

  // Find last valid block (not NULL_HASH)
  uint32_t topHeight = blockchainHeight - 1;
  while (topHeight > 0 && m_core.getBlockIdByHeight(topHeight) == cn::NULL_HASH)
  {
    topHeight--;
  }

  if (topHeight == 0 && m_core.getBlockIdByHeight(0) == cn::NULL_HASH)
  {
    logger(logging::ERROR) << "No valid blocks found in blockchain";
    return true;
  }

  logger(logging::INFO) << "Blockchain reported height: " << blockchainHeight
                        << ", last valid block height: " << topHeight;

  std::ofstream file(outputFile, std::ios::binary);
  if (!file.is_open())
  {
    logger(logging::ERROR) << "Failed to open " << outputFile;
    return true;
  }

  // Write magic bytes "CCHD" (Conceal Headers)
  uint32_t magic = 0x43434844;
  file.write(reinterpret_cast<char *>(&magic), sizeof(magic));

  // Write version
  uint32_t version = 1;
  file.write(reinterpret_cast<char *>(&version), sizeof(version));

  // Write total header count (topHeight + 1)
  uint32_t count = topHeight + 1;
  file.write(reinterpret_cast<char *>(&count), sizeof(count));

  logger(logging::INFO) << "Exporting " << count << " headers...";

  // Write genesis hash for verification
  crypto::Hash genesisHash = m_core.getBlockIdByHeight(0);
  file.write(reinterpret_cast<char *>(&genesisHash), sizeof(genesisHash));

  // Write each header using packed struct
  for (uint32_t h = 0; h <= topHeight; ++h)
  {
    // Verify block exists before trying to export
    crypto::Hash blockHash = m_core.getBlockIdByHeight(h);
    if (blockHash == cn::NULL_HASH)
    {
      logger(logging::WARNING) << "Block at height " << h << " is NULL_HASH, stopping export";
      break;
    }

    cn::BlockHeaderPOD hdr = m_core.getBlockHeader(h);

    ExportedHeader exh;
    exh.height = hdr.height;
    exh.majorVersion = hdr.majorVersion;
    exh.minorVersion = hdr.minorVersion;
    exh.timestamp = hdr.timestamp;
    exh.nonce = hdr.nonce;
    exh.cumulativeDifficulty = hdr.cumulativeDifficulty;
    exh.alreadyGeneratedCoins = hdr.alreadyGeneratedCoins;
    exh.previousBlockHash = hdr.previousBlockHash;
    exh.blockHash = blockHash;

    file.write(reinterpret_cast<const char *>(&exh), sizeof(ExportedHeader));

    if (h % 100000 == 0 && h > 0)
    {
      logger(logging::INFO) << "Exported header " << h << " of " << topHeight;
    }
  }

  file.close();

  // Get actual file size
  std::ifstream checkFile(outputFile, std::ios::binary | std::ios::ate);
  uint64_t fileSize = checkFile.tellg();
  checkFile.close();

  logger(logging::INFO, logging::BRIGHT_GREEN)
      << "Successfully exported headers to " << outputFile
      << " (" << (fileSize / 1024 / 1024) << " MB)";

  return true;
}