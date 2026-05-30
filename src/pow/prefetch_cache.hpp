// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "../CryptoNoteCore/CryptoNoteBasic.h"
#include "backend.hpp"

namespace cn
{

class PowPrefetchCache
{
public:
  void configure(uint32_t queueDepth, uint32_t prefetchWindow, uint32_t backlogThreshold,
                 PowVerifyBackend* backend);

  uint32_t maxDepth() const { return m_maxDepth; }
  uint32_t prefetchWindow() const { return m_prefetchWindow; }
  uint32_t backlogThreshold() const { return m_backlogThreshold; }

  void setValidatedTip(uint32_t height) { m_validatedTip = height; }

  /** True when @a height is strictly ahead of tip and within the prefetch window. */
  bool shouldPrefetchHeight(uint32_t height) const;

  /** Queue one block's longhash on the GPU worker (non-blocking). */
  void enqueueAtHeight(const Block& block, uint32_t height);

  /** Queue upcoming blocks when sync backlog and prefetch window allow. */
  void enqueueUpcoming(const Block* blocks, const uint32_t* heights, size_t blockCount, size_t offset,
                       size_t syncBacklog);

  void clear();

private:
  std::mutex m_mutex;
  PowVerifyBackend* m_backend = nullptr;
  uint32_t m_maxDepth = 8;
  uint32_t m_prefetchWindow = 1024;
  uint32_t m_backlogThreshold = 128;
  uint32_t m_validatedTip = 0;
};

} // namespace cn
