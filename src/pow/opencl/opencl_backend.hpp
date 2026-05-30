// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../backend.hpp"
#include "../cpu_backend.hpp"
#include "../gpu_result_cache.hpp"
namespace cn
{

class OpenclPowBackend : public PowVerifyBackend
{
public:
  OpenclPowBackend(int deviceIndex, uint32_t batchSize, uint32_t minBatchSize, uint32_t maxWaitUs,
                   bool debugCrossCheck, bool debugInnerTrace);
  ~OpenclPowBackend() override;

  bool available() const override { return m_ready; }

  bool gpuAsyncOffload() const override { return m_ready; }

  bool computeLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                       crypto::Hash& out) override;

  bool submitLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                      const crypto::Hash& blockId, uint32_t blockHeight) override;

  bool tryConsumeLonghash(uint32_t blockHeight, const crypto::Hash& blockId,
                          crypto::Hash& out) override;

  bool tryGetLonghash(const crypto::Hash& blockId, crypto::Hash& out) override;

  bool awaitLonghash(const crypto::Hash& blockId, crypto::Hash& out) override;

  bool verifyPow(crypto::cn_context& ctx, const void* data, size_t length,
                 difficulty_type difficulty, crypto::Hash& proofOfWork) override;

  void flush() override;

  void pruneResultsBelow(uint32_t minHeightKept) override;

  void dropWorkAtOrBelow(uint32_t validatedHeight) override;

  void shutdown() override;

  void setWorkerBatchPolicy(uint32_t minBatchSize, uint32_t maxWaitUs) override;

  const PowVerifyMetrics& metrics() const override { return m_metrics; }

  size_t pendingJobCount() override;

private:
  struct GpuJob
  {
    std::unique_ptr<crypto::cn_context> ctx;
    crypto::Hash blockId;
    uint32_t height = UINT32_MAX;
    std::vector<uint8_t> blob;
  };

  struct GpuResult
  {
    crypto::Hash hash;
    bool ok = false;
  };

  bool initOpencl(int deviceIndex);
  bool runSelfTest();
  bool runGpuInnerEnqueue(std::vector<crypto::cn_context*>& contexts, uint32_t maxIter);
  bool runGpuInnerOnly(crypto::cn_context& ctx, const void* data, size_t length, uint32_t maxIter);
  bool runBatchSync(std::vector<crypto::cn_context*>& contexts,
                    const std::vector<const void*>& datas,
                    const std::vector<size_t>& lengths, std::vector<crypto::Hash>& outs,
                    uint32_t maxIter = 0, bool finishHashes = true);

  void startWorker();
  void stopWorker();
  void workerLoop();
  bool executeBatch(std::vector<GpuJob>& batch);
  bool jobKnownLocked(const crypto::Hash& blockId, uint32_t blockHeight) const;

  CpuPowBackend m_cpuFallback;
  GpuPowResultCache m_resultCache;
  PowVerifyMetrics m_metrics;
  uint32_t m_batchSize;
  uint32_t m_minBatchSize;
  uint32_t m_maxWaitUs;
  bool m_debugCrossCheck;
  bool m_ready = false;

  struct OpenclState;
  std::unique_ptr<OpenclState> m_ocl;

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<GpuJob> m_queue;
  std::unordered_map<crypto::Hash, GpuResult, HashHasher> m_completed;
  std::unordered_map<crypto::Hash, char, HashHasher> m_pending;
  std::chrono::steady_clock::time_point m_oldestQueued{};
  bool m_hasOldestQueued = false;
  std::thread m_worker;
  std::atomic<bool> m_shutdown{false};
  std::atomic<uint32_t> m_validatedTip{0};
};

} // namespace cn
