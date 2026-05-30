// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>

#include <boost/program_options.hpp>

#include "Blockchain/Blockchain.h"
#include "Blockchain/BlockchainFilter.h"
#include "Blockchain/SwappedVector.h"
#include "Common/CommandLine.h"
#include "Common/FileMappedVector.h"
#include "Common/PathHelpers.h"
#include "Common/StringTools.h"
#include "Common/Util.h"
#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/Currency.h"
#include "Logging/ConsoleLogger.h"
#include "Storage/MDBXBlockchainStorage.h"

#include <mdbx.h>

namespace po = boost::program_options;
using namespace cn;
using namespace common;

namespace
{
  const command_line::arg_descriptor<std::string> arg_old_data_dir = {
      "old-dir", "Path to the old blockchain data (SwappedVector format)", ""};
  const command_line::arg_descriptor<std::string> arg_new_data_dir = {
      "new-dir", "Path where the new MDBX database will be created", ""};
  const command_line::arg_descriptor<bool> arg_testnet = {
      "testnet", "Use testnet parameters", false};
  const command_line::arg_descriptor<bool> arg_skip_validation = {
      "skip-validation", "Skip chain continuity validation (faster, daemon will reject invalid blocks)", false};
  const command_line::arg_descriptor<uint32_t> arg_batch_size = {
      "batch-size", "Blocks per MDBX transaction (default: 50000, higher = faster but more RAM)", 50000};
}

// ── Block source abstraction ──────────────────────────────────────────────

struct BlockSource
{
  virtual ~BlockSource() = default;
  virtual uint32_t size() const = 0;
  virtual bool getBlock(uint32_t height, Blockchain::BlockEntry &entry) const = 0;
  virtual crypto::Hash getHash(uint32_t height) const = 0;
  virtual void close() = 0;
};

struct SwappedVectorSource : BlockSource
{
  SwappedVector<Blockchain::BlockEntry> blocks;

  bool open(const std::string &oldDir)
  {
    std::string blocksPath = PathHelpers::appendPath(oldDir, parameters::CRYPTONOTE_BLOCKS_FILENAME);
    std::string indexesPath = PathHelpers::appendPath(oldDir, parameters::CRYPTONOTE_BLOCKINDEXES_FILENAME);

    if (!blocks.open(blocksPath, indexesPath, 1024))
    {
      std::cerr << "Error: failed to open old blockchain files" << std::endl;
      std::cerr << "  blocks file: " << blocksPath << std::endl;
      std::cerr << "  indexes file: " << indexesPath << std::endl;
      return false;
    }
    if (blocks.empty())
    {
      std::cerr << "Error: old blockchain is empty" << std::endl;
      return false;
    }
    return true;
  }

  uint32_t size() const override { return static_cast<uint32_t>(blocks.size()); }

  bool getBlock(uint32_t height, Blockchain::BlockEntry &entry) const override
  {
    if (height >= blocks.size())
      return false;
    entry = const_cast<SwappedVector<Blockchain::BlockEntry> &>(blocks)[height];
    return true;
  }

  crypto::Hash getHash(uint32_t height) const override
  {
    return get_block_hash(
        const_cast<SwappedVector<Blockchain::BlockEntry> &>(blocks)[height].bl);
  }

  void close() override { blocks.close(); }
};

// ── Key helpers (match MDBXBlockchainStorage) ─────────────────────────────

std::string blockEntryKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "be_" << std::setw(8) << std::setfill('0') << height;
  return oss.str();
}

std::string blockHeaderKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "hdr_" << std::setw(8) << std::setfill('0') << height;
  return oss.str();
}

std::string filterRecordKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "fr_" << std::setw(8) << std::setfill('0') << height;
  return oss.str();
}

// ── MDBX_val helper ──────────────────────────────────────────────────────

MDBX_val toMdbxVal(const void *data, size_t len)
{
  MDBX_val v;
  v.iov_base = const_cast<void *>(data);
  v.iov_len = len;
  return v;
}

// ── In-memory index structures (for spot-check verification only) ─────────

struct MigrationIndexes
{
  std::vector<crypto::Hash> blockHashes;
  std::unordered_map<crypto::Hash, uint32_t> hashToHeight;

  void reserve(uint32_t totalBlocks)
  {
    blockHashes.reserve(totalBlocks);
    hashToHeight.reserve(totalBlocks);
  }
};

// ── Progress formatting ──────────────────────────────────────────────────

std::string formatDuration(int64_t seconds)
{
  if (seconds < 0)
    return "0s";
  if (seconds < 60)
    return std::to_string(seconds) + "s";
  if (seconds < 3600)
    return std::to_string(seconds / 60) + "m " + std::to_string(seconds % 60) + "s";
  return std::to_string(seconds / 3600) + "h " + std::to_string((seconds % 3600) / 60) + "m " + std::to_string(seconds % 60) + "s";
}

std::string formatSpeed(double blocksPerSecond)
{
  if (blocksPerSecond >= 1000)
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (blocksPerSecond / 1000.0) << "k blk/s";
    return oss.str();
  }
  return std::to_string(static_cast<int>(blocksPerSecond)) + " blk/s";
}

// ═══════════════════════════════════════════════════════════════════════════════
//  main
// ═══════════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
  po::options_description required("Required");
  command_line::add_arg(required, arg_old_data_dir);
  command_line::add_arg(required, arg_new_data_dir);

  po::options_description options("Options");
  command_line::add_arg(options, arg_testnet);
  command_line::add_arg(options, arg_skip_validation);
  command_line::add_arg(options, arg_batch_size);
  options.add_options()("help,h", "Show this help message and exit");

  po::options_description all;
  all.add(required).add(options);

  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, all), vm);
  po::notify(vm);

  if (vm.count("help"))
  {
    std::cout << "Conceal Migration Tool\n\n"
              << "Usage example:\n"
              << "  ./conceald-migrate --old-dir ~/.conceal --new-dir ~/.conceal-mdbx\n\n"
              << required << "\n"
              << options << std::endl;
    return 0;
  }

  std::string oldDir = command_line::get_arg(vm, arg_old_data_dir);
  std::string newDir = command_line::get_arg(vm, arg_new_data_dir);
  bool testnet = command_line::get_arg(vm, arg_testnet);
  bool skipValidation = command_line::get_arg(vm, arg_skip_validation);
  uint32_t batchSize = command_line::get_arg(vm, arg_batch_size);

  if (batchSize == 0)
    batchSize = 50000;
  if (batchSize > 100000)
    batchSize = 100000;

  if (oldDir.empty() || newDir.empty())
  {
    std::cerr << "Error: --old-dir and --new-dir are required" << std::endl;
    return 1;
  }

  if (oldDir == newDir)
  {
    std::cerr << "Error: old-dir and new-dir must be different" << std::endl;
    return 1;
  }

  if (!tools::create_directories_if_necessary(newDir))
  {
    std::cerr << "Error: failed to create new directory: " << newDir << std::endl;
    return 1;
  }

  std::cout << "Conceal Migration Tool v3" << std::endl;
  std::cout << "Source dir:  " << oldDir << std::endl;
  std::cout << "Target dir:  " << newDir << std::endl;
  std::cout << "Testnet:     " << (testnet ? "yes" : "no") << std::endl;
  std::cout << "Validation:  " << (skipValidation ? "SKIPPED" : "ENABLED") << std::endl;
  std::cout << "Batch size:  " << batchSize << " blocks" << std::endl;

  logging::ConsoleLogger consoleLogger;
  Currency currency = CurrencyBuilder(consoleLogger).currency();

  // Open SwappedVector source
  std::cout << std::endl
            << "Opening SwappedVector blockchain..." << std::endl;
  auto svSource = std::unique_ptr<SwappedVectorSource>(new SwappedVectorSource());
  if (!svSource->open(oldDir))
    return 1;

  uint32_t totalBlocks = svSource->size();
  std::cout << "Source blockchain height: " << (totalBlocks - 1) << std::endl;

  // Verify genesis
  std::cout << "Verifying genesis block..." << std::endl;
  Blockchain::BlockEntry genesisEntry;
  if (!svSource->getBlock(0, genesisEntry))
  {
    std::cerr << "Error: failed to read genesis block" << std::endl;
    return 1;
  }
  crypto::Hash genesisHash = get_block_hash(genesisEntry.bl);

  if (!skipValidation && !testnet && genesisHash != currency.genesisBlockHash())
  {
    std::cerr << "Error: genesis block mismatch" << std::endl;
    std::cerr << "  expected: " << common::podToHex(currency.genesisBlockHash()) << std::endl;
    std::cerr << "  actual:   " << common::podToHex(genesisHash) << std::endl;
    return 1;
  }
  std::cout << "Genesis block verified: " << common::podToHex(genesisHash) << std::endl;

  // Initialize MDBX storage
  std::cout << "Initializing MDBX storage backend..." << std::endl;
  std::string mdbxPath = PathHelpers::appendPath(newDir, "mdbx_blocks");
  CryptoNote::MDBXBlockchainStorage mdbxStorage(mdbxPath);

  // Get MDBX handles for batched writes
  MDBX_env *env = mdbxStorage.getEnv();
  MDBX_dbi dbiEntries = mdbxStorage.getDbiBlockEntries();
  MDBX_dbi dbiHeaders = mdbxStorage.getDbiBlockHeaders();
  MDBX_dbi dbiHeights = mdbxStorage.getDbiHeights();
  MDBX_dbi dbiFilterRecords = mdbxStorage.getDbiFilterRecords();

  // Build indexes during migration (for verification only)
  MigrationIndexes idx;
  idx.reserve(totalBlocks);

  uint32_t skippedBlocks = 0;
  uint32_t migratedBlocks = 0;
  uint32_t filterRecordsBuilt = 0;

  // Batched transaction state
  MDBX_txn *batchTxn = nullptr;
  uint32_t batchCount = 0;

  std::cout << std::endl;
  std::cout << "═══════════════════════════════════════════════════════" << std::endl;
  std::cout << "  Starting migration (blocks + filter records)" << std::endl;
  std::cout << "  Total blocks: " << totalBlocks << std::endl;
  std::cout << "  Batch size:   " << batchSize << std::endl;
  std::cout << "  Commits:      ~" << (totalBlocks / batchSize + 1) << std::endl;
  std::cout << "  Progress updates every 10,000 blocks" << std::endl;
  std::cout << "═══════════════════════════════════════════════════════" << std::endl;
  std::cout << std::endl;

  auto startTime = std::chrono::steady_clock::now();
  auto lastReportTime = startTime;
  uint32_t lastReportBlocks = 0;

  for (uint32_t h = 0; h < totalBlocks; ++h)
  {
    Blockchain::BlockEntry entry;
    if (!svSource->getBlock(h, entry))
    {
      std::cerr << "Warning: failed to read block at height " << h << " - skipping" << std::endl;
      skippedBlocks++;
      continue;
    }

    crypto::Hash blockHash = get_block_hash(entry.bl);

    // Chain continuity check
    if (!skipValidation)
    {
      if (h > 0 && entry.bl.previousBlockHash != idx.blockHashes.back())
      {
        std::cerr << "Warning: block " << h << " has invalid previous hash - skipping" << std::endl;
        skippedBlocks++;
        continue;
      }
    }

    // Build header POD
    BlockHeaderPOD hdr;
    hdr.majorVersion = entry.bl.majorVersion;
    hdr.minorVersion = entry.bl.minorVersion;
    hdr.timestamp = entry.bl.timestamp;
    hdr.previousBlockHash = entry.bl.previousBlockHash;
    hdr.nonce = entry.bl.nonce;
    hdr.blockCumulativeSize = entry.block_cumulative_size;
    hdr.cumulativeDifficulty = entry.cumulative_difficulty;
    hdr.alreadyGeneratedCoins = entry.already_generated_coins;
    hdr.height = entry.height;

    // Serialize block entry
    BinaryArray ba = toBinaryArray(entry);

    // ── Begin batch transaction if needed ──────────────────────────────
    if (batchTxn == nullptr)
    {
      int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &batchTxn);
      if (rc != MDBX_SUCCESS)
      {
        std::cerr << "Error: failed to begin MDBX transaction: " << mdbx_strerror(rc) << std::endl;
        return 1;
      }
    }

    // ── Write height → hash ───────────────────────────────────────────
    {
      MDBX_val hkey = toMdbxVal(&h, sizeof(h));
      MDBX_val hval = toMdbxVal(blockHash.data, sizeof(blockHash));
      mdbx_put(batchTxn, dbiHeights, &hkey, &hval, MDBX_UPSERT);
    }

    // ── Write block entry ─────────────────────────────────────────────
    {
      std::string beKey = blockEntryKey(h);
      MDBX_val bkey = toMdbxVal(beKey.data(), beKey.size());
      MDBX_val bval = toMdbxVal(ba.data(), ba.size());
      mdbx_put(batchTxn, dbiEntries, &bkey, &bval, MDBX_UPSERT);
    }

    // ── Write block header ────────────────────────────────────────────
    {
      std::string hdrKey = blockHeaderKey(h);
      MDBX_val hk = toMdbxVal(hdrKey.data(), hdrKey.size());
      MDBX_val hv = toMdbxVal(&hdr, sizeof(hdr));
      mdbx_put(batchTxn, dbiHeaders, &hk, &hv, MDBX_UPSERT);
    }

    // ── Build and write filter record ─────────────────────────────────
    {
      // Collect all transactions for the filter
      std::vector<Transaction> allTxs;
      for (uint32_t i = 1; i < entry.transactions.size(); ++i)
        allTxs.push_back(entry.transactions[i].tx);

      BlockFilterRecord filterRecord = buildBlockFilterRecord(
          entry.bl, entry.height, allTxs);

      BinaryArray filterBa = toBinaryArray(filterRecord);
      std::string frKey = filterRecordKey(h);
      MDBX_val fk = toMdbxVal(frKey.data(), frKey.size());
      MDBX_val fv = toMdbxVal(filterBa.data(), filterBa.size());
      mdbx_put(batchTxn, dbiFilterRecords, &fk, &fv, MDBX_UPSERT);
      filterRecordsBuilt++;
    }

    idx.blockHashes.push_back(blockHash);
    idx.hashToHeight[blockHash] = h;
    migratedBlocks++;
    batchCount++;

    // ── Commit batch if full or final block ────────────────────────────
    if (batchCount >= batchSize || h == totalBlocks - 1)
    {
      int rc = mdbx_txn_commit(batchTxn);
      if (rc != MDBX_SUCCESS)
      {
        std::cerr << "Error: failed to commit MDBX transaction at height " << h
                  << ": " << mdbx_strerror(rc) << std::endl;
        mdbx_txn_abort(batchTxn);
        return 1;
      }
      batchTxn = nullptr;
      batchCount = 0;
    }

    // ── Progress report every 10,000 blocks or final block ─────────────
    if (migratedBlocks % 10000 == 0 || h == totalBlocks - 1)
    {
      auto now = std::chrono::steady_clock::now();
      auto totalElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
      auto intervalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastReportTime).count();

      float percent = migratedBlocks * 100.0f / totalBlocks;

      float overallSpeed = totalElapsed > 0 ? migratedBlocks / (float)totalElapsed : 0;

      uint32_t blocksInInterval = migratedBlocks - lastReportBlocks;
      float recentSpeed = intervalElapsed > 0 ? blocksInInterval / (intervalElapsed / 1000.0f) : 0;

      uint64_t remaining = totalBlocks - migratedBlocks;
      int64_t etaSeconds = overallSpeed > 0 ? (int64_t)(remaining / overallSpeed) : 0;

      std::cout << "\r  ["
                << std::setw(7) << migratedBlocks << " / " << std::setw(7) << totalBlocks
                << "] " << std::fixed << std::setprecision(1) << std::setw(6) << percent << "%"
                << " | Overall: " << std::setw(10) << formatSpeed(overallSpeed)
                << " | Recent: " << std::setw(10) << formatSpeed(recentSpeed)
                << " | ETA: " << formatDuration(etaSeconds)
                << " | Elapsed: " << formatDuration(totalElapsed);

      if (skippedBlocks > 0)
        std::cout << " | Skipped: " << skippedBlocks;

      std::cout << std::flush;

      if (migratedBlocks % 100000 == 0 || h == totalBlocks - 1)
        std::cout << std::endl;

      lastReportTime = now;
      lastReportBlocks = migratedBlocks;
    }
  }

  // Final flush
  mdbxStorage.flush();

  auto endTime = std::chrono::steady_clock::now();
  auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(endTime - startTime).count();

  std::cout << std::endl;
  std::cout << std::endl;
  std::cout << "═══════════════════════════════════════════════════════" << std::endl;
  std::cout << "  Migration complete!" << std::endl;
  std::cout << "═══════════════════════════════════════════════════════" << std::endl;
  std::cout << "  Blocks migrated:       " << migratedBlocks << std::endl;
  std::cout << "  Filter records built:  " << filterRecordsBuilt << std::endl;
  std::cout << "  Blocks skipped:        " << skippedBlocks << std::endl;
  std::cout << "  Total time:            " << formatDuration(totalSeconds) << std::endl;
  std::cout << "  Avg speed:             " << formatSpeed(totalSeconds > 0 ? migratedBlocks / (float)totalSeconds : 0) << std::endl;
  std::cout << "  Database:              " << mdbxPath << std::endl;
  std::cout << "═══════════════════════════════════════════════════════" << std::endl;

  if (skippedBlocks > 0)
  {
    std::cout << std::endl;
    std::cout << "  NOTE: " << skippedBlocks << " blocks were skipped." << std::endl;
    std::cout << "  The daemon will validate the chain on startup." << std::endl;
  }

  // ── Spot-check verification ─────────────────────────────────────────
  std::cout << std::endl;
  std::cout << "Verifying migrated data (spot-check)..." << std::endl;
  bool verificationOk = true;
  uint32_t blocksToCheck = std::min(static_cast<uint32_t>(20), migratedBlocks);
  for (uint32_t i = 0; i < blocksToCheck && verificationOk; ++i)
  {
    uint32_t h = (i == 0) ? 0 : (static_cast<uint32_t>(rand()) % (migratedBlocks - 1) + 1);

    BinaryArray migratedBa;
    if (!mdbxStorage.getBlockEntry(h, migratedBa))
    {
      std::cerr << "  FAIL: block " << h << " not found in MDBX" << std::endl;
      verificationOk = false;
      break;
    }

    Blockchain::BlockEntry migratedEntry;
    if (!cn::fromBinaryArray(migratedEntry, migratedBa))
    {
      std::cerr << "  FAIL: block " << h << " failed to deserialize" << std::endl;
      verificationOk = false;
      break;
    }

    crypto::Hash migratedHash = get_block_hash(migratedEntry.bl);
    if (idx.blockHashes[h] != migratedHash)
    {
      std::cerr << "  FAIL: block " << h << " hash mismatch" << std::endl;
      std::cerr << "    expected: " << common::podToHex(idx.blockHashes[h]) << std::endl;
      std::cerr << "    actual:   " << common::podToHex(migratedHash) << std::endl;
      verificationOk = false;
      break;
    }

    // Spot-check filter record
    BlockFilterRecord filterRecord;
    if (!mdbxStorage.getBlockFilterRecord(h, filterRecord))
    {
      std::cerr << "  FAIL: filter record " << h << " not found in MDBX" << std::endl;
      verificationOk = false;
      break;
    }
  }

  if (verificationOk)
    std::cout << "  All " << blocksToCheck << " spot-checks passed (blocks + filter records)!" << std::endl;
  else
    std::cerr << "  Verification FAILED." << std::endl;

  svSource->close();
  mdbxStorage.close();

  std::cout << std::endl;
  std::cout << "Done. Start your daemon with:" << std::endl;
  std::cout << "  ./conceald --data-dir " << newDir << std::endl;
  std::cout << std::endl;
  std::cout << "In-memory indexes and filter records will be available immediately." << std::endl;

  return verificationOk ? 0 : 1;
}