// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "pow_service.hpp"

#include <cstring>
#include <sstream>

#include "../CryptoNoteConfig.h"

#include "../CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "../CryptoNoteCore/Difficulty.h"
#include "cpu_backend.hpp"
#include "pow_sync_log.hpp"

namespace cn
{

PowService& PowService::instance()
{
  static PowService inst;
  return inst;
}

void PowService::init(std::unique_ptr<PowVerifyBackend> backend, const GpuPowConfig& gpuConfig,
                      logging::LoggerRef* syncLogger)
{
  m_backend = std::move(backend);
  m_gpuPrefetchEnabled = m_backend && m_backend->gpuAsyncOffload();
  m_offloadDeviceIndex = m_gpuPrefetchEnabled ? gpuConfig.deviceIndex : -1;
  m_backlogThreshold = gpuConfig.backlogThreshold;
  m_trustGpuCache = gpuConfig.trustGpuCache;
  m_prefetchWindowUserSet = gpuConfig.prefetchWindowUserSet;
  m_prefetchDepthUserSet = gpuConfig.prefetchDepthUserSet;
  m_minBatchUserSet = gpuConfig.minBatchSizeUserSet;
  m_maxWaitUserSet = gpuConfig.maxWaitUsUserSet;
  m_userPrefetchWindow = gpuConfig.prefetchWindow;
  m_userPrefetchDepth = gpuConfig.prefetchQueueDepth;
  m_baseMinBatchSize = gpuConfig.minBatchSize;
  m_baseMaxWaitUs = gpuConfig.maxWaitUs;
  m_batchCap = gpuConfig.batchSize;
  setPowSyncLogger(m_gpuPrefetchEnabled ? syncLogger : nullptr);
  updatePrefetchForConnections(P2P_DEFAULT_CONNECTIONS_COUNT);
}

void PowService::updatePrefetchForConnections(size_t outgoingConnectionLanes)
{
  if (!m_gpuPrefetchEnabled || !m_backend)
    return;

  const uint32_t window = m_prefetchWindowUserSet ? m_userPrefetchWindow
                                                  : GpuPowConfig::prefetchWindowForConnections(outgoingConnectionLanes);
  const uint32_t depth = m_prefetchDepthUserSet ? m_userPrefetchDepth
                                                : GpuPowConfig::prefetchDepthForConnections(outgoingConnectionLanes);
  uint32_t minBatch = m_minBatchUserSet ? m_baseMinBatchSize
                                        : GpuPowConfig::scaledMinBatchSize(outgoingConnectionLanes);
  if (minBatch > m_batchCap)
    minBatch = m_batchCap;
  const uint32_t maxWait = m_maxWaitUserSet ? m_baseMaxWaitUs
                                            : GpuPowConfig::scaledMaxWaitUs(outgoingConnectionLanes);

  if (outgoingConnectionLanes == m_prefetchConnectionLanes && window == m_effectivePrefetchWindow &&
      depth == m_effectivePrefetchDepth && minBatch == m_effectiveMinBatchSize &&
      maxWait == m_effectiveMaxWaitUs)
    return;

  m_prefetchConnectionLanes = outgoingConnectionLanes;
  m_effectivePrefetchWindow = window;
  m_effectivePrefetchDepth = depth;
  m_effectiveMinBatchSize = minBatch;
  m_effectiveMaxWaitUs = maxWait;
  m_prefetch.configure(depth, window, m_backlogThreshold, m_backend.get());
  m_backend->setWorkerBatchPolicy(minBatch, maxWait);

  std::ostringstream oss;
  oss << "GPU OFFLOAD auto-scale: peer count=" << outgoingConnectionLanes << " window=" << window
      << (m_prefetchWindowUserSet ? " (user)," : ",") << " depth=" << depth
      << (m_prefetchDepthUserSet ? " (user)," : ",") << " min_batch=" << minBatch
      << (m_minBatchUserSet ? " (user)," : ",") << " max_wait_us=" << maxWait
      << (m_maxWaitUserSet ? " (user)" : "");
  powSyncLogInfo(oss.str());
}

void PowService::shutdown()
{
  m_gpuPrefetchEnabled = false;
  m_offloadDeviceIndex = -1;
  setPowSyncLogger(nullptr);
  m_prefetch.configure(0, 0, 0, nullptr);
  if (m_backend)
    m_backend->shutdown();
}

int PowService::deactivateGpuOffloadOnSynchronized()
{
  if (!m_gpuPrefetchEnabled || !m_backend || !m_backend->gpuAsyncOffload())
    return -1;

  const int deviceIndex = m_offloadDeviceIndex;

  m_gpuPrefetchEnabled = false;
  m_offloadDeviceIndex = -1;
  setPowSyncLogger(nullptr);
  m_prefetch.configure(0, 0, 0, nullptr);
  m_syncBatchRemainder = 0;
  m_syncCatchupGap = 0;

  m_backend->shutdown();
  m_backend.reset(new CpuPowBackend());

  return deviceIndex;
}

PowVerifyBackend& PowService::backend()
{
  return *m_backend;
}

PowPrefetchCache& PowService::prefetch() { return m_prefetch; }

bool PowService::gpuSpeculativeActive() const
{
  const size_t backlog = std::max(m_syncBatchRemainder, static_cast<size_t>(m_syncCatchupGap));
  return m_gpuPrefetchEnabled && backlog >= m_backlogThreshold;
}

void PowService::updateSyncContext(size_t batchRemainder, uint32_t catchupGap)
{
  m_syncBatchRemainder = batchRemainder;
  m_syncCatchupGap = catchupGap;
}

void PowService::onBlockValidated(uint32_t height)
{
  m_prefetch.setValidatedTip(height);
  if (m_backend)
  {
    m_backend->dropWorkAtOrBelow(height);
    const uint32_t keepBack = 32;
    const uint32_t floor = height > keepBack ? height - keepBack : 0;
    m_backend->pruneResultsBelow(floor);
  }
}

bool PowService::verifyCnGpu(crypto::cn_context& ctx, const Block& block, difficulty_type difficulty,
                             crypto::Hash& proofOfWork, uint32_t blockHeight)
{
  crypto::Hash blockId = get_block_hash(block);

  crypto::Hash gpuCached;
  const bool hadGpuCache =
      !m_trustGpuCache && gpuSpeculativeActive() && blockHeight != UINT32_MAX &&
      m_backend->tryConsumeLonghash(blockHeight, blockId, gpuCached);

  if (m_trustGpuCache && gpuSpeculativeActive() && blockHeight != UINT32_MAX &&
      m_backend->tryConsumeLonghash(blockHeight, blockId, proofOfWork))
  {
    ++m_profileMetrics.gpuHits;
    if (m_gpuPrefetchEnabled && powSyncLogEnabled(blockHeight))
    {
      std::ostringstream oss;
      oss << "CN-GPU verify height=" << blockHeight << " id=" << powHashShort(blockId)
          << " PoW=GPU (cached OpenCL inner, CPU finish_hash)";
      powSyncLogDebug(oss.str());
    }
    return check_hash(proofOfWork, difficulty);
  }

  BinaryArray blob;
  if (!get_block_hashing_blob(block, blob))
    return false;

  if (!m_backend->computeLonghash(ctx, blob.data(), blob.size(), proofOfWork))
    return false;

  ++m_profileMetrics.cpuPowUsed;

  if (hadGpuCache)
  {
    if (memcmp(gpuCached.data, proofOfWork.data, sizeof(proofOfWork.data)) == 0)
      ++m_profileMetrics.gpuHits;
    else
    {
      ++m_profileMetrics.gpuCacheMismatch;
      if (m_gpuPrefetchEnabled && powSyncLogEnabled(blockHeight))
      {
        std::ostringstream oss;
        oss << "CN-GPU verify height=" << blockHeight << " id=" << powHashShort(blockId)
            << " GPU/CPU PoW MISMATCH — using CPU hash";
        powSyncLogDebug(oss.str());
      }
    }
  }

  if (m_gpuPrefetchEnabled && powSyncLogEnabled(blockHeight))
  {
    std::ostringstream oss;
    oss << "CN-GPU verify height=" << blockHeight << " id=" << powHashShort(blockId)
        << " PoW=CPU (" << powCpuInnerName();
    if (!gpuSpeculativeActive())
      oss << ", speculative off (catchup=" << m_syncCatchupGap << " batch=" << m_syncBatchRemainder
          << " < " << m_backlogThreshold << ")";
    else
      oss << ", no GPU result in cache";
    oss << ")";
    powSyncLogDebug(oss.str());
  }

  return check_hash(proofOfWork, difficulty);
}

void PowService::recordPushBlockTiming(uint64_t powNs, uint64_t txNs, uint64_t dbNs)
{
  m_profileMetrics.powVerifyNs += powNs;
  m_profileMetrics.txValidateNs += txNs;
  m_profileMetrics.dbCommitNs += dbNs;
}

const PowVerifyMetrics& PowService::metrics() const
{
  if (m_backend)
  {
    static PowVerifyMetrics combined;
    combined = m_backend->metrics();
    combined.powVerifyNs = m_profileMetrics.powVerifyNs;
    combined.txValidateNs = m_profileMetrics.txValidateNs;
    combined.dbCommitNs = m_profileMetrics.dbCommitNs;
    combined.cpuPowUsed += m_profileMetrics.cpuPowUsed;
    combined.gpuHits += m_profileMetrics.gpuHits;
    combined.gpuCacheMismatch += m_profileMetrics.gpuCacheMismatch;
    return combined;
  }
  return m_profileMetrics;
}

} // namespace cn
