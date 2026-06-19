// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "BoltSync.h"
#include "Blockchain/BlockchainFilter.h"
#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Storage/MDBXBlockchainStorage.h"

namespace BoltSync
{
  struct ScanContext
  {
    CryptoNote::MDBXBlockchainStorage &storage;
    const crypto::SecretKey &viewKey;
    const crypto::PublicKey &spendPublicKey;
    const crypto::SecretKey *spendKey;
    std::atomic<uint64_t> &blocksProcessed;
    std::atomic<uint32_t> &lastCheckpointHeight;
    std::mutex &resultsMutex;
    std::vector<FoundOutput> &results;
    std::function<void(uint32_t)> saveCheckpoint;
  };

  bool deserializeBlockEntry(const cn::BinaryArray &rawEntry,
                             cn::Block &block,
                             std::vector<cn::Transaction> &transactions,
                             std::vector<std::vector<uint32_t>> *globalIndexesPerTx = nullptr);

  void scanSingleBlock(uint32_t height, ScanContext &ctx);

  void markSpentOutputs(CryptoNote::MDBXBlockchainStorage &storage,
                        uint32_t topHeight,
                        std::vector<FoundOutput> &results);
}