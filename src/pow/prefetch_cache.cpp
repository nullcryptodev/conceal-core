// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "prefetch_cache.hpp"

#include <sstream>

#include "../CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "pow_sync_log.hpp"

namespace cn
{

void PowPrefetchCache::configure(uint32_t queueDepth, uint32_t prefetchWindow, uint32_t backlogThreshold,
                               PowVerifyBackend* backend)
{
  std::lock_guard<std::mutex> lk(m_mutex);
  m_backend = backend;
  m_prefetchWindow = prefetchWindow > 0 ? prefetchWindow : 1024;
  m_backlogThreshold = backlogThreshold;
  m_maxDepth = (!m_backend || queueDepth == 0) ? 0 : queueDepth;
}

bool PowPrefetchCache::shouldPrefetchHeight(uint32_t height) const
{
  if (height == UINT32_MAX)
    return false;
  return height > m_validatedTip && height <= m_validatedTip + m_prefetchWindow;
}

void PowPrefetchCache::enqueueAtHeight(const Block& block, uint32_t height)
{
  if (!m_backend || m_maxDepth == 0 || !shouldPrefetchHeight(height))
    return;

  if (m_backend->pendingJobCount() >= m_maxDepth)
    return;

  const crypto::Hash blockId = get_block_hash(block);
  BinaryArray blob;
  if (!get_block_hashing_blob(block, blob))
    return;

  crypto::cn_context unused;
  if (!m_backend->submitLonghash(unused, blob.data(), blob.size(), blockId, height))
    return;

  if (powSyncLogEnabled(height))
  {
    std::ostringstream oss;
    oss << "CN-GPU prefetch block height=" << height << " id=" << powHashShort(blockId)
        << " gpuPending=" << m_backend->pendingJobCount() << "/" << m_maxDepth;
    powSyncLogDebug(oss.str());
  }
}

void PowPrefetchCache::enqueueUpcoming(const Block* blocks, const uint32_t* heights, size_t blockCount,
                                       size_t offset, size_t syncBacklog)
{
  if (!m_backend || m_maxDepth == 0 || !blocks || !heights || offset >= blockCount)
    return;

  if (syncBacklog < m_backlogThreshold)
    return;

  const size_t tailCount = blockCount - offset;
  if (tailCount > 0 && powSyncLogEnabled(heights[offset]))
  {
    std::ostringstream oss;
    oss << "CN-GPU prefetch " << tailCount << " block(s) heights " << heights[offset];
    if (tailCount > 1)
      oss << ".." << heights[blockCount - 1];
    powSyncLogDebug(oss.str());
  }

  for (size_t i = offset; i < blockCount; ++i)
  {
    if (blocks[i].majorVersion < 8)
      continue;
    if (!shouldPrefetchHeight(heights[i]))
      break;
    if (m_backend->pendingJobCount() >= m_maxDepth)
      break;
    enqueueAtHeight(blocks[i], heights[i]);
  }
}

void PowPrefetchCache::clear() {}

} // namespace cn
