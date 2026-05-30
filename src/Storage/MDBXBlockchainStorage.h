// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "IBlockchainStorage.h"

#include <mdbx.h>
#include <mutex>
#include <string>
#include <vector>

namespace CryptoNote
{

  class MDBXBlockchainStorage : public IBlockchainStorage
  {
  public:
    explicit MDBXBlockchainStorage(const std::string &dataDir);
    ~MDBXBlockchainStorage() override;

    // Lifecycle
    void flush() override;
    void close() override;

    // Atomic block write (single transaction, immediate commit)
    void pushCompleteBlock(uint32_t height,
                           const crypto::Hash &hash,
                           const cn::BinaryArray &serializedEntry,
                           const cn::BlockHeaderPOD &hdr) override;

    // Atomic block removal (single transaction, immediate commit)
    void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) override;

    // Block reads
    bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const override;
    bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const override;
    void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                              std::vector<cn::BlockHeaderPOD> &out) const override;

    // Height tracking (derived from 'heights' database)
    uint32_t topBlockHeight() const override;

    // Transaction pool persistence
    void storePoolState(const std::vector<cn::BinaryArray> &serializedTxs,
                        const std::vector<crypto::KeyImage> &spentKeyImages) override;
    std::vector<cn::BinaryArray> loadPoolTransactions() const override;
    std::vector<crypto::KeyImage> loadPoolSpentKeyImages() const override;

    // Filter database
    void storeBlockFilterRecord(uint32_t height,
                                const cn::BlockFilterRecord &record) override;
    bool getBlockFilterRecord(uint32_t height,
                              cn::BlockFilterRecord &record) const override;
    bool hasBlockFilterRecord(uint32_t height) const override;

    // Diagnostics
    std::string printDatabaseStats() const;

    // Used in migration tool
    MDBX_env *getEnv() const { return m_env; }
    MDBX_dbi getDbiBlockEntries() const { return m_dbiBlockEntries; }
    MDBX_dbi getDbiBlockHeaders() const { return m_dbiBlockHeaders; }
    MDBX_dbi getDbiHeights() const { return m_dbiHeights; }
    MDBX_dbi getDbiFilterRecords() const { return m_dbiFilterRecords; }

  private:
    // Environment & database setup
    void openEnvironment(const std::string &path);
    void openDatabases(MDBX_txn *txn);

    // Key helpers
    static std::string blockEntryKey(uint32_t height);
    static std::string blockHeaderKey(uint32_t height);
    static std::string filterRecordKey(uint32_t height);

    static constexpr int kHeightKeyWidth = 8;

    // Safe MDBX_val constructors
    static MDBX_val to_val(const void *data, size_t len);
    static MDBX_val to_val(const std::string &s);
    static MDBX_val to_val(const crypto::Hash &h);
    static MDBX_val to_val(const cn::BlockHeaderPOD &hdr);

    // MDBX handles
    MDBX_env *m_env = nullptr;
    MDBX_dbi m_dbiBlockEntries;  // "be_XXXXXXXX" → serialized BlockEntry
    MDBX_dbi m_dbiBlockHeaders;  // "hdr_XXXXXXXX" → BlockHeaderPOD
    MDBX_dbi m_dbiHeights;       // uint32_t height → crypto::Hash
    MDBX_dbi m_dbiPoolState;     // "pool_tx_N" / "pool_ki_N" → binary blobs
    MDBX_dbi m_dbiFilterRecords; // "fr_XXXXXXXX" → serialized BlockFilterRecord

    // Thread safety
    mutable std::mutex m_txMutex;

    // Configuration
    std::string m_dataDir;
  };

} // namespace CryptoNote