// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "gpu_result_cache.hpp"

#include <cstring>

namespace cn
{

void GpuPowResultCache::store(uint32_t height, const crypto::Hash& blockId,
                              const crypto::Hash& proofOfWork, bool ok)
{
  std::lock_guard<std::mutex> lk(m_mutex);
  Entry& e = m_byHeight[height];
  e.blockId = blockId;
  e.proofOfWork = proofOfWork;
  e.ready = true;
  e.ok = ok;
}

bool GpuPowResultCache::tryConsume(uint32_t height, const crypto::Hash& blockId, crypto::Hash& out)
{
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_byHeight.find(height);
  if (it == m_byHeight.end() || !it->second.ready || !it->second.ok)
    return false;
  if (memcmp(it->second.blockId.data, blockId.data, sizeof(blockId.data)) != 0)
    return false;
  out = it->second.proofOfWork;
  m_byHeight.erase(it);
  return true;
}

bool GpuPowResultCache::hasReady(uint32_t height, const crypto::Hash& blockId) const
{
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_byHeight.find(height);
  if (it == m_byHeight.end() || !it->second.ready || !it->second.ok)
    return false;
  return memcmp(it->second.blockId.data, blockId.data, sizeof(blockId.data)) == 0;
}

void GpuPowResultCache::clear()
{
  std::lock_guard<std::mutex> lk(m_mutex);
  m_byHeight.clear();
}

void GpuPowResultCache::pruneBelow(uint32_t minHeightKept)
{
  std::lock_guard<std::mutex> lk(m_mutex);
  for (auto it = m_byHeight.begin(); it != m_byHeight.end();)
  {
    if (it->first < minHeightKept)
      it = m_byHeight.erase(it);
    else
      ++it;
  }
}

} // namespace cn
