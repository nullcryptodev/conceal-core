// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "MDBXBlockchainStorage.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "Common/StringTools.h"
#include "Serialization/SerializationTools.h"

#include <functional>
#include <stdexcept>
#include <cstring>
#include <iomanip>
#include <sstream>

using namespace CryptoNote;
using namespace common;
using namespace cn;

// MDBX_val helpers
MDBX_val MDBXBlockchainStorage::to_val(const void *data, size_t len)
{
  MDBX_val v;
  v.iov_base = const_cast<void *>(data);
  v.iov_len = len;
  return v;
}

MDBX_val MDBXBlockchainStorage::to_val(const std::string &s)
{
  return to_val(s.data(), s.size());
}

MDBX_val MDBXBlockchainStorage::to_val(const crypto::Hash &h)
{
  return to_val(h.data, sizeof(h));
}

MDBX_val MDBXBlockchainStorage::to_val(const cn::BlockHeaderPOD &hdr)
{
  return to_val(&hdr, sizeof(hdr));
}

// Key helpers
std::string MDBXBlockchainStorage::blockEntryKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "be_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

std::string MDBXBlockchainStorage::blockHeaderKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "hdr_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

std::string MDBXBlockchainStorage::filterRecordKey(uint32_t height)
{
  std::ostringstream oss;
  oss << "fr_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
  return oss.str();
}

// Constructor / Destructor
MDBXBlockchainStorage::MDBXBlockchainStorage(const std::string &dataDir)
    : m_dataDir(dataDir)
{
  openEnvironment(dataDir);
}

MDBXBlockchainStorage::~MDBXBlockchainStorage()
{
  close();
}

void MDBXBlockchainStorage::openEnvironment(const std::string &path)
{
  int rc = mdbx_env_create(&m_env);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

  auto envGuard = std::unique_ptr<MDBX_env, decltype(&mdbx_env_close)>(m_env, mdbx_env_close);

  // 5 named databases: block_entries, block_headers, heights, pool_state, filter_records
  mdbx_env_set_maxdbs(m_env, 5);

  // Sensible geometry: 256 MB initial, up to 128 GB growth
  mdbx_env_set_geometry(m_env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);

  const MDBX_env_flags_t openFlags = MDBX_NOSUBDIR | MDBX_NORDAHEAD | MDBX_LIFORECLAIM;

  rc = mdbx_env_open(m_env, path.c_str(), openFlags, 0664);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));

  // Open a write transaction to create/verify all named databases
  MDBX_txn *txn;
  rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("txn_begin failed: " + std::string(mdbx_strerror(rc)));

  auto txnGuard = std::unique_ptr<MDBX_txn, std::function<void(MDBX_txn *)>>(
      txn, [](MDBX_txn *t)
      { if (t) mdbx_txn_abort(t); });

  openDatabases(txn);

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("initial txn_commit failed: " + std::string(mdbx_strerror(rc)));

  txnGuard.release();
  envGuard.release();
}

void MDBXBlockchainStorage::openDatabases(MDBX_txn *txn)
{
  mdbx_dbi_open(txn, "block_entries", MDBX_CREATE, &m_dbiBlockEntries);
  mdbx_dbi_open(txn, "block_headers", MDBX_CREATE, &m_dbiBlockHeaders);
  mdbx_dbi_open(txn, "heights", MDBX_CREATE, &m_dbiHeights);
  mdbx_dbi_open(txn, "pool_state", MDBX_CREATE, &m_dbiPoolState);
  mdbx_dbi_open(txn, "filter_records", MDBX_CREATE, &m_dbiFilterRecords);
}

// Lifecycle
void MDBXBlockchainStorage::flush()
{
  if (m_env)
    mdbx_env_sync(m_env);
}

void MDBXBlockchainStorage::close()
{
  std::lock_guard<std::mutex> lock(m_txMutex);
  if (m_env)
  {
    mdbx_env_close(m_env);
    m_env = nullptr;
  }
}

// Height tracking
uint32_t MDBXBlockchainStorage::topBlockHeight() const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return 0;

  MDBX_stat stat;
  rc = mdbx_dbi_stat(txn, m_dbiHeights, &stat, sizeof(stat));
  mdbx_txn_abort(txn);

  if (rc != MDBX_SUCCESS || stat.ms_entries == 0)
    return 0;

  return static_cast<uint32_t>(stat.ms_entries - 1);
}

// Atomic block write
void MDBXBlockchainStorage::pushCompleteBlock(uint32_t height,
                                              const crypto::Hash &hash,
                                              const cn::BinaryArray &serializedEntry,
                                              const cn::BlockHeaderPOD &hdr)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("pushCompleteBlock: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // 1. Height -> Hash
  MDBX_val hkey = to_val(&height, sizeof(height));
  MDBX_val hashVal = to_val(hash);
  rc = mdbx_put(txn, m_dbiHeights, &hkey, &hashVal, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: heights put failed: " + std::string(mdbx_strerror(rc)));
  }

  // 2. Block entry
  std::string beKey = blockEntryKey(height);
  MDBX_val bkey = to_val(beKey);
  MDBX_val bval = to_val(serializedEntry.data(), serializedEntry.size());
  rc = mdbx_put(txn, m_dbiBlockEntries, &bkey, &bval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: block_entries put failed: " + std::string(mdbx_strerror(rc)));
  }

  // 3. Block header
  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hdrk = to_val(hdrKey);
  MDBX_val hdrv = to_val(hdr);
  rc = mdbx_put(txn, m_dbiBlockHeaders, &hdrk, &hdrv, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: block_headers put failed: " + std::string(mdbx_strerror(rc)));
  }

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("pushCompleteBlock: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

// Atomic block removal
void MDBXBlockchainStorage::removeCompleteBlock(uint32_t height, const crypto::Hash &hash)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("removeCompleteBlock: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // 1. Remove height -> hash
  MDBX_val hkey = to_val(&height, sizeof(height));
  mdbx_del(txn, m_dbiHeights, &hkey, nullptr);

  // 2. Remove block entry
  std::string beKey = blockEntryKey(height);
  MDBX_val bkey = to_val(beKey);
  mdbx_del(txn, m_dbiBlockEntries, &bkey, nullptr);

  // 3. Remove block header
  std::string hdrKey = blockHeaderKey(height);
  MDBX_val hdrk = to_val(hdrKey);
  mdbx_del(txn, m_dbiBlockHeaders, &hdrk, nullptr);

  // 4. Remove filter record
  std::string frKey = filterRecordKey(height);
  MDBX_val frk = to_val(frKey);
  mdbx_del(txn, m_dbiFilterRecords, &frk, nullptr);

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("removeCompleteBlock: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

// Block reads
bool MDBXBlockchainStorage::getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string key = blockEntryKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;
  rc = mdbx_get(txn, m_dbiBlockEntries, &mkey, &mval);

  if (rc == MDBX_SUCCESS)
  {
    serializedEntry.assign(
        static_cast<const uint8_t *>(mval.iov_base),
        static_cast<const uint8_t *>(mval.iov_base) + mval.iov_len);
    mdbx_txn_abort(txn);
    return true;
  }

  mdbx_txn_abort(txn);
  return false;
}

bool MDBXBlockchainStorage::getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string key = blockHeaderKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;
  rc = mdbx_get(txn, m_dbiBlockHeaders, &mkey, &mval);

  if (rc == MDBX_SUCCESS && mval.iov_len == sizeof(cn::BlockHeaderPOD))
  {
    memcpy(&hdr, mval.iov_base, sizeof(hdr));
    mdbx_txn_abort(txn);
    return true;
  }

  mdbx_txn_abort(txn);
  return false;
}

void MDBXBlockchainStorage::getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                                 std::vector<cn::BlockHeaderPOD> &out) const
{
  out.clear();
  out.reserve(count);

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return;

  MDBX_cursor *cursor;
  rc = mdbx_cursor_open(txn, m_dbiBlockHeaders, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    return;
  }

  std::string startKey = blockHeaderKey(startHeight);
  MDBX_val mkey = to_val(startKey);
  MDBX_val mval;
  rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_SET_RANGE);

  while (rc == MDBX_SUCCESS && out.size() < count)
  {
    if (mkey.iov_len > 4 && memcmp(mkey.iov_base, "hdr_", 4) == 0 &&
        mval.iov_len == sizeof(cn::BlockHeaderPOD))
    {
      cn::BlockHeaderPOD hdr;
      memcpy(&hdr, mval.iov_base, sizeof(hdr));
      out.push_back(hdr);
    }
    rc = mdbx_cursor_get(cursor, &mkey, &mval, MDBX_NEXT);
  }

  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
}

// Transaction pool persistence
void MDBXBlockchainStorage::storePoolState(
    const std::vector<cn::BinaryArray> &serializedTxs,
    const std::vector<crypto::KeyImage> &spentKeyImages)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("storePoolState: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  // Clear old pool data
  MDBX_cursor *cursor;
  rc = mdbx_cursor_open(txn, m_dbiPoolState, &cursor);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("storePoolState: cursor open failed: " + std::string(mdbx_strerror(rc)));
  }

  std::vector<std::string> keysToDelete;
  MDBX_val ckey, cval;
  while (mdbx_cursor_get(cursor, &ckey, &cval, MDBX_NEXT) == MDBX_SUCCESS)
  {
    std::string keyStr(static_cast<const char *>(ckey.iov_base), ckey.iov_len);
    keysToDelete.push_back(keyStr);
  }
  mdbx_cursor_close(cursor);

  for (const auto &keyStr : keysToDelete)
  {
    MDBX_val mk = to_val(keyStr);
    mdbx_del(txn, m_dbiPoolState, &mk, nullptr);
  }

  // Write transaction count
  uint32_t txCount = static_cast<uint32_t>(serializedTxs.size());
  MDBX_val countKey = to_val(std::string("pool_tx_count"));
  MDBX_val countVal = to_val(&txCount, sizeof(txCount));
  mdbx_put(txn, m_dbiPoolState, &countKey, &countVal, MDBX_UPSERT);

  // Write each transaction
  for (size_t i = 0; i < serializedTxs.size(); ++i)
  {
    std::string keyStr = "pool_tx_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv = to_val(serializedTxs[i].data(), serializedTxs[i].size());
    mdbx_put(txn, m_dbiPoolState, &mk, &mv, MDBX_UPSERT);
  }

  // Write spent key image count
  uint32_t kiCount = static_cast<uint32_t>(spentKeyImages.size());
  MDBX_val kiCountKey = to_val(std::string("pool_ki_count"));
  MDBX_val kiCountVal = to_val(&kiCount, sizeof(kiCount));
  mdbx_put(txn, m_dbiPoolState, &kiCountKey, &kiCountVal, MDBX_UPSERT);

  // Write each spent key image
  for (size_t i = 0; i < spentKeyImages.size(); ++i)
  {
    std::string keyStr = "pool_ki_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv = to_val(spentKeyImages[i].data, sizeof(spentKeyImages[i].data));
    mdbx_put(txn, m_dbiPoolState, &mk, &mv, MDBX_UPSERT);
  }

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("storePoolState: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

std::vector<cn::BinaryArray> MDBXBlockchainStorage::loadPoolTransactions() const
{
  std::vector<cn::BinaryArray> result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return result;

  MDBX_val countKey = to_val(std::string("pool_tx_count"));
  MDBX_val countVal;
  if (mdbx_get(txn, m_dbiPoolState, &countKey, &countVal) != MDBX_SUCCESS ||
      countVal.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return result;
  }

  uint32_t txCount = *static_cast<const uint32_t *>(countVal.iov_base);
  result.reserve(txCount);

  for (uint32_t i = 0; i < txCount; ++i)
  {
    std::string keyStr = "pool_tx_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv;
    if (mdbx_get(txn, m_dbiPoolState, &mk, &mv) == MDBX_SUCCESS)
    {
      cn::BinaryArray ba(
          static_cast<const uint8_t *>(mv.iov_base),
          static_cast<const uint8_t *>(mv.iov_base) + mv.iov_len);
      result.push_back(std::move(ba));
    }
  }

  mdbx_txn_abort(txn);
  return result;
}

std::vector<crypto::KeyImage> MDBXBlockchainStorage::loadPoolSpentKeyImages() const
{
  std::vector<crypto::KeyImage> result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return result;

  MDBX_val countKey = to_val(std::string("pool_ki_count"));
  MDBX_val countVal;
  if (mdbx_get(txn, m_dbiPoolState, &countKey, &countVal) != MDBX_SUCCESS ||
      countVal.iov_len < sizeof(uint32_t))
  {
    mdbx_txn_abort(txn);
    return result;
  }

  uint32_t kiCount = *static_cast<const uint32_t *>(countVal.iov_base);
  result.reserve(kiCount);

  for (uint32_t i = 0; i < kiCount; ++i)
  {
    std::string keyStr = "pool_ki_" + std::to_string(i);
    MDBX_val mk = to_val(keyStr);
    MDBX_val mv;
    if (mdbx_get(txn, m_dbiPoolState, &mk, &mv) == MDBX_SUCCESS &&
        mv.iov_len == sizeof(crypto::KeyImage))
    {
      crypto::KeyImage ki;
      std::memcpy(ki.data, mv.iov_base, sizeof(ki.data));
      result.push_back(ki);
    }
  }

  mdbx_txn_abort(txn);
  return result;
}

// Filter database
void MDBXBlockchainStorage::storeBlockFilterRecord(uint32_t height,
                                                   const cn::BlockFilterRecord &record)
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS)
    throw std::runtime_error("storeBlockFilterRecord: txn_begin failed: " + std::string(mdbx_strerror(rc)));

  cn::BinaryArray ba = cn::toBinaryArray(record);
  std::string key = filterRecordKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval = to_val(ba.data(), ba.size());

  rc = mdbx_put(txn, m_dbiFilterRecords, &mkey, &mval, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("storeBlockFilterRecord: put failed: " + std::string(mdbx_strerror(rc)));
  }

  rc = mdbx_txn_commit(txn);
  if (rc != MDBX_SUCCESS)
  {
    mdbx_txn_abort(txn);
    throw std::runtime_error("storeBlockFilterRecord: commit failed: " + std::string(mdbx_strerror(rc)));
  }
}

bool MDBXBlockchainStorage::getBlockFilterRecord(uint32_t height,
                                                 cn::BlockFilterRecord &record) const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string key = filterRecordKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;

  rc = mdbx_get(txn, m_dbiFilterRecords, &mkey, &mval);
  if (rc == MDBX_SUCCESS)
  {
    cn::BinaryArray ba(
        static_cast<const uint8_t *>(mval.iov_base),
        static_cast<const uint8_t *>(mval.iov_base) + mval.iov_len);
    mdbx_txn_abort(txn);
    return cn::fromBinaryArray(record, ba);
  }

  mdbx_txn_abort(txn);
  return false;
}

bool MDBXBlockchainStorage::hasBlockFilterRecord(uint32_t height) const
{
  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
  if (rc != MDBX_SUCCESS)
    return false;

  std::string key = filterRecordKey(height);
  MDBX_val mkey = to_val(key);
  MDBX_val mval;

  rc = mdbx_get(txn, m_dbiFilterRecords, &mkey, &mval);
  mdbx_txn_abort(txn);
  return rc == MDBX_SUCCESS;
}

// Diagnostics
std::string MDBXBlockchainStorage::printDatabaseStats() const
{
  std::string result;

  std::lock_guard<std::mutex> lock(m_txMutex);

  MDBX_txn *txn;
  if (mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return "error: failed to begin transaction";

  MDBX_dbi dbis[] = {m_dbiBlockEntries, m_dbiBlockHeaders, m_dbiHeights, m_dbiPoolState, m_dbiFilterRecords};
  const char *names[] = {"block_entries", "block_headers", "heights", "pool_state", "filter_records"};

  for (int i = 0; i < 5; i++)
  {
    MDBX_stat stat;
    if (mdbx_dbi_stat(txn, dbis[i], &stat, sizeof(stat)) == MDBX_SUCCESS)
    {
      size_t dataMB = (stat.ms_leaf_pages * 4096) >> 20;
      char line[128];
      snprintf(line, sizeof(line), "  %-18s %8lu entries  %6zu MB\n",
               names[i], (unsigned long)stat.ms_entries, dataMB);
      result += line;
    }
  }

  mdbx_txn_abort(txn);
  return result;
}