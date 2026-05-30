// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license.

#include "cpu_backend.hpp"

#include <cstring>

#include "../CryptoNoteCore/Difficulty.h"
#include "../crypto/cryptonight.hpp"

namespace cn
{

bool CpuPowBackend::computeLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                                    crypto::Hash& out)
{
  cn_gpu_hash_v0(ctx, data, length, out);
  ++m_metrics.jobsSubmitted;
  return true;
}

bool CpuPowBackend::submitLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                                   const crypto::Hash& blockId, uint32_t blockHeight)
{
  crypto::Hash out;
  if (!computeLonghash(ctx, data, length, out))
    return false;
  std::lock_guard<std::mutex> lk(m_mutex);
  m_completed[blockId] = out;
  if (blockHeight != UINT32_MAX)
    m_byHeight[blockHeight] = {blockId, out};
  return true;
}

bool CpuPowBackend::tryConsumeLonghash(uint32_t blockHeight, const crypto::Hash& blockId,
                                       crypto::Hash& out)
{
  if (blockHeight == UINT32_MAX)
    return false;
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_byHeight.find(blockHeight);
  if (it == m_byHeight.end())
    return false;
  if (memcmp(it->second.blockId.data, blockId.data, sizeof(blockId.data)) != 0)
    return false;
  out = it->second.pow;
  m_byHeight.erase(it);
  m_completed.erase(blockId);
  return true;
}

bool CpuPowBackend::tryGetLonghash(const crypto::Hash& blockId, crypto::Hash& out)
{
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_completed.find(blockId);
  if (it == m_completed.end())
    return false;
  out = it->second;
  m_completed.erase(it);
  return true;
}

bool CpuPowBackend::awaitLonghash(const crypto::Hash& blockId, crypto::Hash& out)
{
  return tryGetLonghash(blockId, out);
}

size_t CpuPowBackend::pendingJobCount()
{
  std::lock_guard<std::mutex> lk(m_mutex);
  return m_completed.size() + m_byHeight.size();
}

bool CpuPowBackend::verifyPow(crypto::cn_context& ctx, const void* data, size_t length,
                              difficulty_type difficulty, crypto::Hash& proofOfWork)
{
  if (!computeLonghash(ctx, data, length, proofOfWork))
    return false;
  return check_hash(proofOfWork, difficulty);
}

} // namespace cn
