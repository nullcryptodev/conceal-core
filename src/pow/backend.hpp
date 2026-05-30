// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "../CryptoNoteCore/Difficulty.h"
#include "../crypto/hash.h"

namespace cn
{

struct HashHasher
{
  size_t operator()(const crypto::Hash& h) const
  {
    size_t r = 0;
    for (size_t i = 0; i < 4; ++i)
      r ^= reinterpret_cast<const size_t*>(h.data)[i];
    return r;
  }
};

struct PowVerifyMetrics
{
  uint64_t jobsSubmitted = 0;
  uint64_t batchesExecuted = 0;
  uint64_t avgBatchSize = 0;
  uint64_t gpuVerifyNs = 0;
  uint64_t cpuFallbackCount = 0;
  uint64_t mismatchCount = 0;
  uint64_t gpuHits = 0;
  uint64_t cpuPowUsed = 0;
  uint64_t gpuCacheMismatch = 0;
  uint64_t powVerifyNs = 0;
  uint64_t txValidateNs = 0;
  uint64_t dbCommitNs = 0;
};

class PowVerifyBackend
{
public:
  virtual ~PowVerifyBackend() = default;

  virtual bool available() const = 0;

  virtual bool computeLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                               crypto::Hash& out) = 0;

  /** Queue GPU/CPU inner work; @a blockHeight tags sync results (UINT32_MAX if unused). */
  virtual bool submitLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                              const crypto::Hash& blockId, uint32_t blockHeight);

  /** Non-blocking consume by chain height (sync hot path). */
  virtual bool tryConsumeLonghash(uint32_t blockHeight, const crypto::Hash& blockId, crypto::Hash& out);

  /** Non-blocking: true if @a blockId result is ready. */
  virtual bool tryGetLonghash(const crypto::Hash& blockId, crypto::Hash& out);

  /** Block until @a blockId result is ready (tests / flush / computeLonghash only). */
  virtual bool awaitLonghash(const crypto::Hash& blockId, crypto::Hash& out);

  virtual bool verifyPow(crypto::cn_context& ctx, const void* data, size_t length,
                         difficulty_type difficulty, crypto::Hash& proofOfWork) = 0;

  virtual void flush() {}

  /** Drop cached GPU results below @a minHeightKept (validated tip housekeeping). */
  virtual void pruneResultsBelow(uint32_t minHeightKept) { (void)minHeightKept; }

  /** Cancel queued GPU work at or below @a validatedHeight (CPU passed this height). */
  virtual void dropWorkAtOrBelow(uint32_t validatedHeight) { (void)validatedHeight; }

  /** Drain pending work and stop async workers (daemon shutdown). */
  virtual void shutdown() {}

  /** Update OpenCL worker batch aggregation (no-op on CPU backend). */
  virtual void setWorkerBatchPolicy(uint32_t minBatchSize, uint32_t maxWaitUs)
  {
    (void)minBatchSize;
    (void)maxWaitUs;
  }

  /** True when OpenCL async offload is active (prefetch / GPU worker). */
  virtual bool gpuAsyncOffload() const { return false; }

  /** Jobs queued or running on the GPU worker (for prefetch back-pressure). */
  virtual size_t pendingJobCount() { return 0; }

  virtual const PowVerifyMetrics& metrics() const = 0;
};

std::unique_ptr<PowVerifyBackend> createPowVerifyBackend(int gpuDeviceIndex, uint32_t batchSize,
                                                         uint32_t minBatchSize, uint32_t maxWaitUs,
                                                         bool debugCrossCheck, bool debugInnerTrace);

} // namespace cn
