// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <vector>
#include "crypto/hash.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Blockchain/BlockchainFilter.h"

namespace CryptoNote
{

  // ──────────────────────────────────────────────────────────
  // Interface — storage backend for blockchain data
  // ──────────────────────────────────────────────────────────

  class IBlockchainStorage
  {
  public:
    virtual ~IBlockchainStorage() = default;

    // ── Lifecycle
    virtual void flush() = 0;
    virtual void close() = 0;

    // ── Atomic block write (single transaction, immediate commit)
    virtual void pushCompleteBlock(uint32_t height,
                                   const crypto::Hash &hash,
                                   const cn::BinaryArray &serializedEntry,
                                   const cn::BlockHeaderPOD &hdr) = 0;

    // ── Atomic block removal
    virtual void removeCompleteBlock(uint32_t height, const crypto::Hash &hash) = 0;

    // ── Block reads
    virtual bool getBlockEntry(uint32_t height, cn::BinaryArray &serializedEntry) const = 0;
    virtual bool getBlockHeader(uint32_t height, cn::BlockHeaderPOD &hdr) const = 0;
    virtual void getBlockHeadersRange(uint32_t startHeight, uint32_t count,
                                      std::vector<cn::BlockHeaderPOD> &out) const = 0;

    // ── Height tracking
    virtual uint32_t topBlockHeight() const = 0;

    // ── Transaction pool persistence
    virtual void storePoolState(const std::vector<cn::BinaryArray> &serializedTxs,
                                const std::vector<crypto::KeyImage> &spentKeyImages) = 0;
    virtual std::vector<cn::BinaryArray> loadPoolTransactions() const = 0;
    virtual std::vector<crypto::KeyImage> loadPoolSpentKeyImages() const = 0;

    // ── Filter database (two-pass output recognition)
    virtual void storeBlockFilterRecord(uint32_t height,
                                        const cn::BlockFilterRecord &record) = 0;
    virtual bool getBlockFilterRecord(uint32_t height,
                                      cn::BlockFilterRecord &record) const = 0;
    virtual bool hasBlockFilterRecord(uint32_t height) const = 0;
  };

} // namespace CryptoNote