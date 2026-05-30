// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <mdbx.h>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

namespace Sidechain
{
  // Lightweight MDBX wrapper for sidechain storage.
  // Provides simple KV operations (put/get) and block management
  // independent of the main blockchain storage.
  class SidechainMdbxStorage
  {
  public:
    explicit SidechainMdbxStorage(const std::string &dataDir);
    ~SidechainMdbxStorage();

    // ── Block storage ────────────────────────────────────────────────

    void pushBlockEntry(uint32_t height, const std::vector<uint8_t> &serializedBlock);
    bool getBlockEntry(uint32_t height, std::vector<uint8_t> &serializedBlock) const;

    // ── Height tracking ──────────────────────────────────────────────

    uint32_t topBlockHeight() const;
    void setTopBlockHeight(uint32_t height);

    // ── Key-value operations ─────────────────────────────────────────

    void put(const std::string &key, const std::vector<uint8_t> &value);
    bool get(const std::string &key, std::vector<uint8_t> &value) const;
    void remove(const std::string &key);

    // ── Lifecycle ────────────────────────────────────────────────────

    void flush();
    void close();

  private:
    void openEnvironment(const std::string &path);
    void openDatabases(MDBX_txn *txn);

    static MDBX_val to_val(const void *data, size_t len);
    static MDBX_val to_val(const std::string &s);

    static std::string blockEntryKey(uint32_t height);
    static constexpr int kHeightKeyWidth = 8;

    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiBlockEntries; // "be_XXXXXXXX" → serialized sidechain Block
    MDBX_dbi m_dbiMeta;         // arbitrary string → binary blob
    MDBX_dbi m_dbiHeights;      // uint32_t height → (empty, presence = exists)

    mutable std::mutex m_mutex;
    std::string m_dataDir;
  };

} // namespace Sidechain