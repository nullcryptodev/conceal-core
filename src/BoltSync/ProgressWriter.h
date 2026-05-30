// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltSync.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace BoltSync
{
  class ProgressWriter
  {
  public:
    ProgressWriter(const std::string &progressFile,
                   uint32_t topHeight,
                   std::atomic<uint64_t> &blocksProcessed,
                   std::vector<FoundOutput> &results,
                   uint32_t lastScannedHeight);
    ~ProgressWriter();
    void start();
    void stop();
    void waitForThreads();

  private:
    std::string m_progressFile;
    uint32_t m_topHeight;
    std::atomic<uint64_t> &m_blocksProcessed;
    std::vector<FoundOutput> &m_results;
    uint32_t m_lastScannedHeight;
    std::thread m_displayThread;
    std::thread m_fileThread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_displayStopped{false};
    std::atomic<bool> m_fileStopped{false};
  };
}