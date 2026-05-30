// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license

#include "BoltSync.h"
#include "BlockDeserializer.h"
#include "CryptoHelpers.h"
#include "ProgressWriter.h"
#include "Common/PathHelpers.h"

#include "Storage/MDBXBlockchainStorage.h"
#include "Common/Util.h"

#include <thread>
#include <fstream>

namespace BoltSync
{
  Scanner::Scanner(const crypto::SecretKey &viewKey,
                   const crypto::PublicKey &spendPublicKey,
                   const crypto::SecretKey *spendKey)
      : m_viewKey(viewKey), m_spendPublicKey(spendPublicKey), m_spendKey(spendKey) {}

  Scanner::~Scanner() {}

  // ── Progress file helpers ──────────────────────────────────────────────

  namespace
  {
    std::string progressFilePath(const std::string &dataDir)
    {
      return PathHelpers::appendPath(dataDir, "bolt_sync_progress.dat");
    }

    uint32_t loadProgress(const std::string &dataDir)
    {
      std::ifstream file(progressFilePath(dataDir), std::ios::binary);
      if (!file)
        return 0;
      uint32_t height = 0;
      file.read(reinterpret_cast<char *>(&height), sizeof(height));
      return file ? height : 0;
    }

    void saveProgress(const std::string &dataDir, uint32_t height)
    {
      std::ofstream file(progressFilePath(dataDir), std::ios::binary | std::ios::trunc);
      if (file)
        file.write(reinterpret_cast<const char *>(&height), sizeof(height));
    }

    void clearProgress(const std::string &dataDir)
    {
      std::remove(progressFilePath(dataDir).c_str());
    }
  }

  bool Scanner::scan(const ScanConfig &config, ScanState &state)
  {
    std::string dbPath = PathHelpers::appendPath(config.dataDir, "mdbx_blocks");
    CryptoNote::MDBXBlockchainStorage storage(dbPath);

    uint32_t topHeight = storage.topBlockHeight();
    if (topHeight == 0)
      return false;

    // Check for resume from progress file
    uint32_t lastScannedHeight = loadProgress(config.dataDir);
    if (lastScannedHeight >= topHeight)
      lastScannedHeight = 0;

    state.blocksProcessed = lastScannedHeight;
    state.lastCheckpointHeight = lastScannedHeight;

    auto saveCheckpoint = [&](uint32_t height)
    {
      saveProgress(config.dataDir, height);
      state.lastCheckpointHeight.store(height, std::memory_order_relaxed);
    };

    unsigned int numThreads = config.numThreads;
    if (numThreads == 0)
    {
      numThreads = std::thread::hardware_concurrency();
      if (numThreads == 0)
        numThreads = 4;
    }
    if (numThreads > 16)
      numThreads = 16;

    // Progress writer
    ProgressWriter progress(config.progressFile, topHeight, state.blocksProcessed, state.results, lastScannedHeight);
    progress.start();

    uint32_t scanTopHeight = (config.endBlock > 0 && config.endBlock < topHeight) ? config.endBlock : topHeight;
    uint32_t scanStartHeight = std::max(config.startBlock, lastScannedHeight + 1);

    state.scannedTopHeight = scanTopHeight;

    if (scanStartHeight > scanTopHeight)
      return false;

    uint32_t blocksPerThread = (scanTopHeight - scanStartHeight + 1 + numThreads - 1) / numThreads;

    std::vector<std::thread> threads;
    for (unsigned int t = 0; t < numThreads; ++t)
    {
      uint32_t start = scanStartHeight + static_cast<uint32_t>(t * blocksPerThread);
      uint32_t end = std::min(start + blocksPerThread - 1, scanTopHeight);
      if (start > end)
        break;

      threads.emplace_back([&, start, end]()
                           {
            ScanContext ctx{ storage, m_viewKey, m_spendPublicKey, m_spendKey,
                             state.blocksProcessed, state.lastCheckpointHeight,
                             state.resultsMutex, state.results, saveCheckpoint };
            for (uint32_t h = start; h <= end; ++h)
            {
                scanSingleBlock(h, ctx);
                uint32_t reportInterval = (end - start) / 100;
                if (reportInterval < 1000)
                    reportInterval = 1000;
                if (h % reportInterval == 0 || h == end)
                {
                    ctx.saveCheckpoint(h);
                    if (config.onProgress) config.onProgress(h);
                }
            } });
    }

    for (auto &t : threads)
    {
      if (t.joinable())
        t.join();
    }

    state.progressDone = true;
    progress.stop();
    progress.waitForThreads();

    // Second pass: mark outputs as spent
    markSpentOutputs(storage, scanTopHeight, state.results);

    // Clear checkpoint on success
    clearProgress(config.dataDir);

    return true;
  }
}