// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <cstdint>
#include <memory>

#include "../CryptoNoteCore/CryptoNoteBasic.h"
#include "../Logging/LoggerRef.h"
#include "backend.hpp"
#include "GpuPowConfig.h"
#include "prefetch_cache.hpp"

namespace cn
{

class PowService
{
public:
  static PowService& instance();

  void init(std::unique_ptr<PowVerifyBackend> backend, const GpuPowConfig& gpuConfig,
            logging::LoggerRef* syncLogger = nullptr);

  /** Stop prefetch, drain GPU queue, and join the OpenCL worker. */
  void shutdown();

  /** After chain sync: stop GPU verify offload, release OpenCL, fall back to CPU verify. */
  int deactivateGpuOffloadOnSynchronized();

  PowVerifyBackend& backend();
  PowPrefetchCache& prefetch();

  /** OpenCL async prefetch is active (--gpu-device N and backend ready). */
  bool gpuPrefetchEnabled() const { return m_gpuPrefetchEnabled; }

  /** GPU result cache may be used (sync backlog above threshold). */
  bool gpuSpeculativeActive() const;

  /** @a batchRemainder: blocks left in current P2P batch; @a catchupGap: peer height minus local tip. */
  void updateSyncContext(size_t batchRemainder, uint32_t catchupGap);

  /**
   * Scale prefetch window/depth and worker min-batch/max-wait with outgoing P2P lanes unless overridden on CLI.
   * Call after P2P init and when auto-scale changes the target connection count.
   */
  void updatePrefetchForConnections(size_t outgoingConnectionLanes);

  void onBlockValidated(uint32_t height);

  bool verifyCnGpu(crypto::cn_context& ctx, const Block& block, difficulty_type difficulty,
                   crypto::Hash& proofOfWork, uint32_t blockHeight);

  void recordPushBlockTiming(uint64_t powNs, uint64_t txNs, uint64_t dbNs);

  const PowVerifyMetrics& metrics() const;

private:
  PowService() = default;
  std::unique_ptr<PowVerifyBackend> m_backend;
  PowPrefetchCache m_prefetch;
  PowVerifyMetrics m_profileMetrics;
  bool m_gpuPrefetchEnabled = false;
  int m_offloadDeviceIndex = -1;
  bool m_trustGpuCache = true;
  uint32_t m_backlogThreshold = 128;
  bool m_prefetchWindowUserSet = false;
  bool m_prefetchDepthUserSet = false;
  bool m_minBatchUserSet = false;
  bool m_maxWaitUserSet = false;
  uint32_t m_userPrefetchWindow = 0;
  uint32_t m_userPrefetchDepth = 0;
  uint32_t m_baseMinBatchSize = 32;
  uint32_t m_baseMaxWaitUs = 9000;
  uint32_t m_batchCap = 128;
  size_t m_prefetchConnectionLanes = 0;
  uint32_t m_effectivePrefetchWindow = 0;
  uint32_t m_effectivePrefetchDepth = 0;
  uint32_t m_effectiveMinBatchSize = 0;
  uint32_t m_effectiveMaxWaitUs = 0;
  size_t m_syncBatchRemainder = 0;
  uint32_t m_syncCatchupGap = 0;
};

} // namespace cn
