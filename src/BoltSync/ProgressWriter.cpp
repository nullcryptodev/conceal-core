// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license

#include "ProgressWriter.h"
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace BoltSync
{
  ProgressWriter::ProgressWriter(const std::string &progressFile,
                                 uint32_t topHeight,
                                 std::atomic<uint64_t> &blocksProcessed,
                                 std::vector<FoundOutput> &results,
                                 uint32_t lastScannedHeight)
      : m_progressFile(progressFile), m_topHeight(topHeight),
        m_blocksProcessed(blocksProcessed), m_results(results),
        m_lastScannedHeight(lastScannedHeight) {}

  ProgressWriter::~ProgressWriter() { stop(); }

  void ProgressWriter::start()
  {
    m_stop = false;
    m_displayStopped = false;
    m_fileStopped = false;

    m_displayThread = std::thread([this]()
                                  {
      auto startTime = std::chrono::steady_clock::now();
      while (!m_stop)
      {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (m_stop)
          break;

        uint64_t processed = m_blocksProcessed.load(std::memory_order_relaxed);
        if (processed == 0 || m_topHeight == 0)
          continue;

        float percent = (float)processed / (float)m_topHeight * 100.0f;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startTime).count();
        float speed = elapsed > 0 ? (float)(processed - m_lastScannedHeight) / (float)elapsed : 0;
        uint64_t eta = speed > 0 ? (uint64_t)((m_topHeight - processed) / speed) : 0;

        std::cout << "\r[Progress] " << processed << "/" << m_topHeight
                  << " (" << std::fixed << std::setprecision(1) << percent << "%)"
                  << " | Speed: " << (int)speed << " blk/s"
                  << " | ETA: " << (eta / 60) << "m " << (eta % 60) << "s"
                  << " | Outputs: " << m_results.size() << "    " << std::flush;
      }
      m_displayStopped = true; });

    if (!m_progressFile.empty())
    {
      m_fileThread = std::thread([this]()
                                 {
        while (!m_stop)
        {
          std::ofstream pf(m_progressFile, std::ios::trunc);
          if (pf.is_open())
          {
            pf << m_blocksProcessed.load() << "/" << m_topHeight << std::endl;
            pf.close();
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        m_fileStopped = true; });
    }
    else
    {
      m_fileStopped = true;
    }
  }

  void ProgressWriter::waitForThreads()
  {
    // Wait for display thread to fully exit
    for (int i = 0; i < 50 && !m_displayStopped; ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Wait for file thread to fully exit
    for (int i = 0; i < 50 && !m_fileStopped; ++i)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (m_displayThread.joinable())
      m_displayThread.join();

    if (m_fileThread.joinable())
      m_fileThread.join();
  }

  void ProgressWriter::stop()
  {
    m_stop = true;

    // Give threads time to notice the stop flag
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    waitForThreads();
  }
}