// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SidechainMdbxStorage.h"
#include <stdexcept>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <memory>
#include <functional>

namespace Sidechain
{
  // ── MDBX_val helpers ──────────────────────────────────────────────────

  MDBX_val SidechainMdbxStorage::to_val(const void *data, size_t len)
  {
    MDBX_val v;
    v.iov_base = const_cast<void *>(data);
    v.iov_len = len;
    return v;
  }

  MDBX_val SidechainMdbxStorage::to_val(const std::string &s)
  {
    return to_val(s.data(), s.size());
  }

  // ── Key helper ────────────────────────────────────────────────────────

  std::string SidechainMdbxStorage::blockEntryKey(uint32_t height)
  {
    std::ostringstream oss;
    oss << "be_" << std::setw(kHeightKeyWidth) << std::setfill('0') << height;
    return oss.str();
  }

  // ── Constructor / Destructor ──────────────────────────────────────────

  SidechainMdbxStorage::SidechainMdbxStorage(const std::string &dataDir)
      : m_dataDir(dataDir)
  {
    openEnvironment(dataDir);
  }

  SidechainMdbxStorage::~SidechainMdbxStorage()
  {
    close();
  }

  void SidechainMdbxStorage::openEnvironment(const std::string &path)
  {
    int rc = mdbx_env_create(&m_env);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("SidechainMdbxStorage: mdbx_env_create failed: " + std::string(mdbx_strerror(rc)));

    // 3 named databases: side_blocks, side_meta, side_heights
    mdbx_env_set_maxdbs(m_env, 3);
    mdbx_env_set_geometry(m_env, -1, -1, (intptr_t)1 << 37, 256 << 20, -1, -1);

    const MDBX_env_flags_t openFlags = MDBX_NOSUBDIR | MDBX_NORDAHEAD | MDBX_LIFORECLAIM;

    rc = mdbx_env_open(m_env, path.c_str(), openFlags, 0664);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("SidechainMdbxStorage: mdbx_env_open failed: " + std::string(mdbx_strerror(rc)));

    MDBX_txn *txn = nullptr;
    rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("SidechainMdbxStorage: txn_begin failed: " + std::string(mdbx_strerror(rc)));

    openDatabases(txn);

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("SidechainMdbxStorage: initial commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  void SidechainMdbxStorage::openDatabases(MDBX_txn *txn)
  {
    mdbx_dbi_open(txn, "side_blocks", MDBX_CREATE, &m_dbiBlockEntries);
    mdbx_dbi_open(txn, "side_meta", MDBX_CREATE, &m_dbiMeta);
    mdbx_dbi_open(txn, "side_heights", MDBX_CREATE, &m_dbiHeights);
  }

  // ── Block storage ─────────────────────────────────────────────────────

  void SidechainMdbxStorage::pushBlockEntry(uint32_t height, const std::vector<uint8_t> &serializedBlock)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn = nullptr;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("SidechainMdbxStorage: pushBlockEntry txn_begin failed: " + std::string(mdbx_strerror(rc)));

    // Write block entry
    std::string beKey = blockEntryKey(height);
    MDBX_val bkey = to_val(beKey);
    MDBX_val bval = to_val(serializedBlock.data(), serializedBlock.size());
    rc = mdbx_put(txn, m_dbiBlockEntries, &bkey, &bval, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("SidechainMdbxStorage: pushBlockEntry put failed: " + std::string(mdbx_strerror(rc)));
    }

    // Mark height as existing
    MDBX_val hkey = to_val(&height, sizeof(height));
    MDBX_val emptyVal = to_val(nullptr, 0);
    mdbx_put(txn, m_dbiHeights, &hkey, &emptyVal, MDBX_UPSERT);

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("SidechainMdbxStorage: pushBlockEntry commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  bool SidechainMdbxStorage::getBlockEntry(uint32_t height, std::vector<uint8_t> &serializedBlock) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn = nullptr;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS)
      return false;

    std::string beKey = blockEntryKey(height);
    MDBX_val bkey = to_val(beKey);
    MDBX_val bval;
    rc = mdbx_get(txn, m_dbiBlockEntries, &bkey, &bval);

    if (rc == MDBX_SUCCESS)
    {
      serializedBlock.assign(
          static_cast<const uint8_t *>(bval.iov_base),
          static_cast<const uint8_t *>(bval.iov_base) + bval.iov_len);
      mdbx_txn_abort(txn);
      return true;
    }

    mdbx_txn_abort(txn);
    return false;
  }

  // ── Height tracking ───────────────────────────────────────────────────

  uint32_t SidechainMdbxStorage::topBlockHeight() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn = nullptr;
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

  void SidechainMdbxStorage::setTopBlockHeight(uint32_t height)
  {
    (void)height;
  }

  // ── Key-value operations ──────────────────────────────────────────────

  void SidechainMdbxStorage::put(const std::string &key, const std::vector<uint8_t> &value)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn = nullptr;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("SidechainMdbxStorage: put txn_begin failed: " + std::string(mdbx_strerror(rc)));

    MDBX_val mk = to_val(key);
    MDBX_val mv = to_val(value.data(), value.size());
    rc = mdbx_put(txn, m_dbiMeta, &mk, &mv, MDBX_UPSERT);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("SidechainMdbxStorage: put failed: " + std::string(mdbx_strerror(rc)));
    }

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("SidechainMdbxStorage: put commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  bool SidechainMdbxStorage::get(const std::string &key, std::vector<uint8_t> &value) const
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn = nullptr;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS)
      return false;

    MDBX_val mk = to_val(key);
    MDBX_val mv;
    rc = mdbx_get(txn, m_dbiMeta, &mk, &mv);

    if (rc == MDBX_SUCCESS)
    {
      value.assign(
          static_cast<const uint8_t *>(mv.iov_base),
          static_cast<const uint8_t *>(mv.iov_base) + mv.iov_len);
      mdbx_txn_abort(txn);
      return true;
    }

    mdbx_txn_abort(txn);
    return false;
  }

  void SidechainMdbxStorage::remove(const std::string &key)
  {
    std::lock_guard<std::mutex> lock(m_mutex);

    MDBX_txn *txn = nullptr;
    int rc = mdbx_txn_begin(m_env, nullptr, MDBX_TXN_READWRITE, &txn);
    if (rc != MDBX_SUCCESS)
      throw std::runtime_error("SidechainMdbxStorage: remove txn_begin failed: " + std::string(mdbx_strerror(rc)));

    MDBX_val mk = to_val(key);
    mdbx_del(txn, m_dbiMeta, &mk, nullptr);

    rc = mdbx_txn_commit(txn);
    if (rc != MDBX_SUCCESS)
    {
      mdbx_txn_abort(txn);
      throw std::runtime_error("SidechainMdbxStorage: remove commit failed: " + std::string(mdbx_strerror(rc)));
    }
  }

  // ── Lifecycle ─────────────────────────────────────────────────────────

  void SidechainMdbxStorage::flush()
  {
    if (m_env)
      mdbx_env_sync(m_env);
  }

  void SidechainMdbxStorage::close()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_env)
    {
      mdbx_env_close(m_env);
      m_env = nullptr;
    }
  }

} // namespace Sidechain