// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "opencl_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <sstream>
#include "../mining/kernel_loader.hpp"

#include "../../crypto/cryptonight.hpp"
#include "../../crypto/pow_hash/hw_detect.hpp"
#include "../cpu_backend.hpp"
#include "../pow_sync_log.hpp"
#include "../../CryptoNoteCore/Difficulty.h"
#include "opencl_program_cache.hpp"
#include "raii.hpp"

#include <iostream>

namespace cn
{

namespace
{
void powLog(const std::string& msg)
{
  std::cerr << "[pow/opencl] " << msg << std::endl;
}

std::string readKernelFile()
{
  const char* paths[] = {
#ifdef CONCEAL_KERNEL_SOURCE
      CONCEAL_KERNEL_SOURCE,
#endif
#ifdef CONCEAL_KERNEL_DIR
      CONCEAL_KERNEL_DIR "/cn_gpu_inner.cl",
#endif
      "cn_gpu_inner.cl",
      "src/pow/kernels/cn_gpu_inner.cl",
      "../src/pow/kernels/cn_gpu_inner.cl",
      "../../src/pow/kernels/cn_gpu_inner.cl",
      "../../../src/pow/kernels/cn_gpu_inner.cl",
  };
  return loadOpenclKernelSource(std::vector<const char*>(paths, paths + 7));
}
} // namespace

OpenclPowBackend::OpenclPowBackend(int deviceIndex, uint32_t batchSize, uint32_t minBatchSize,
                                   uint32_t maxWaitUs, bool debugCrossCheck, bool debugInnerTrace)
    : m_batchSize(batchSize), m_minBatchSize(minBatchSize), m_maxWaitUs(maxWaitUs),
      m_debugCrossCheck(debugCrossCheck)
{
#ifndef CONCEAL_WITH_OPENCL
  (void)deviceIndex;
  (void)debugInnerTrace;
  powLog("OpenCL support not compiled in; GPU PoW disabled");
  return;
#else
  if (debugInnerTrace)
    crypto::cn_gpu_set_inner_trace(true);
  if (!initOpencl(deviceIndex))
  {
    powLog("OpenCL init failed; falling back to CPU for CN-GPU PoW");
    return;
  }
  if (!runSelfTest())
  {
    powLog("OpenCL CN-GPU self-test failed; GPU PoW disabled");
    m_ready = false;
  }
  else
  {
    m_ready = true;
    startWorker();
  }
#endif
}

#ifdef CONCEAL_WITH_OPENCL
struct OpenclPowBackend::OpenclState
{
  cl_platform_id platform = nullptr;
  cl_device_id device = nullptr;
  cl_context context = nullptr;
  cl_command_queue queue = nullptr;
  cl_program program = nullptr;
  cl_kernel innerKernel = nullptr;
  cl_mem spads = nullptr;
  cl_mem lpads = nullptr;
  size_t maxJobs = 0;
  std::vector<uint64_t> hostSpads;
  std::vector<uint8_t> hostLpads;
};
#endif

OpenclPowBackend::~OpenclPowBackend()
{
  stopWorker();
#ifdef CONCEAL_WITH_OPENCL
  if (!m_ocl)
    return;
  if (m_ocl->lpads)
    clReleaseMemObject(m_ocl->lpads);
  if (m_ocl->spads)
    clReleaseMemObject(m_ocl->spads);
  if (m_ocl->innerKernel)
    clReleaseKernel(m_ocl->innerKernel);
  if (m_ocl->program)
    clReleaseProgram(m_ocl->program);
  if (m_ocl->queue)
    clReleaseCommandQueue(m_ocl->queue);
  if (m_ocl->context)
    clReleaseContext(m_ocl->context);
#endif
}

bool OpenclPowBackend::computeLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                                      crypto::Hash& out)
{
  // Sync hot path: always CPU. GPU is speculative-only via submitLonghash + result cache.
  return m_cpuFallback.computeLonghash(ctx, data, length, out);
}

bool OpenclPowBackend::submitLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                                      const crypto::Hash& blockId, uint32_t blockHeight)
{
  (void)ctx;
  if (!m_ready || m_shutdown.load())
    return false;

  std::lock_guard<std::mutex> lk(m_mutex);
  if (jobKnownLocked(blockId, blockHeight))
    return true;

  GpuJob job;
  job.ctx.reset(new crypto::cn_context());
  crypto::cn_gpu_prepare_inner(*job.ctx, data, length);
  job.blockId = blockId;
  job.height = blockHeight;
  if (m_debugCrossCheck)
  {
    job.blob.resize(length);
    memcpy(job.blob.data(), data, length);
  }

  if (!m_hasOldestQueued)
  {
    m_oldestQueued = std::chrono::steady_clock::now();
    m_hasOldestQueued = true;
  }
  m_queue.push_back(std::move(job));
  m_pending[blockId] = 1;
  m_cv.notify_one();
  return true;
}

bool OpenclPowBackend::tryConsumeLonghash(uint32_t blockHeight, const crypto::Hash& blockId,
                                          crypto::Hash& out)
{
  if (!m_ready || blockHeight == UINT32_MAX)
    return false;

  if (m_resultCache.tryConsume(blockHeight, blockId, out))
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_completed.erase(blockId);
    m_pending.erase(blockId);
    return true;
  }

  {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_completed.find(blockId);
    if (it == m_completed.end() || !it->second.ok)
      return false;
    out = it->second.hash;
    m_completed.erase(it);
    m_pending.erase(blockId);
    return true;
  }
}

size_t OpenclPowBackend::pendingJobCount()
{
  std::lock_guard<std::mutex> lk(m_mutex);
  // Each in-flight job is in m_pending until the worker finishes; do not add m_queue (same jobs).
  return m_pending.size();
}

bool OpenclPowBackend::tryGetLonghash(const crypto::Hash& blockId, crypto::Hash& out)
{
  std::lock_guard<std::mutex> lk(m_mutex);
  auto it = m_completed.find(blockId);
  if (it == m_completed.end() || !it->second.ok)
    return false;
  out = it->second.hash;
  m_completed.erase(it);
  m_pending.erase(blockId);
  return true;
}

bool OpenclPowBackend::awaitLonghash(const crypto::Hash& blockId, crypto::Hash& out)
{
  std::unique_lock<std::mutex> lk(m_mutex);
  m_cv.wait(lk, [&] { return m_completed.count(blockId) > 0; });

  auto it = m_completed.find(blockId);
  if (it == m_completed.end() || !it->second.ok)
    return false;
  out = it->second.hash;
  m_completed.erase(it);
  m_pending.erase(blockId);
  return true;
}

void OpenclPowBackend::flush()
{
  if (!m_ready)
    return;
  m_cv.notify_all();
  std::unique_lock<std::mutex> lk(m_mutex);
  m_cv.wait(lk, [&] { return m_queue.empty() && m_pending.empty(); });
  m_resultCache.clear();
}

void OpenclPowBackend::pruneResultsBelow(uint32_t minHeightKept)
{
  if (!m_ready)
    return;
  m_resultCache.pruneBelow(minHeightKept);
}

void OpenclPowBackend::dropWorkAtOrBelow(uint32_t validatedHeight)
{
  if (!m_ready)
    return;
  m_validatedTip.store(validatedHeight, std::memory_order_release);
  m_resultCache.pruneBelow(validatedHeight + 1);
  std::lock_guard<std::mutex> lk(m_mutex);
  for (auto it = m_queue.begin(); it != m_queue.end();)
  {
    if (it->height != UINT32_MAX && it->height <= validatedHeight)
    {
      m_pending.erase(it->blockId);
      it = m_queue.erase(it);
    }
    else
    {
      ++it;
    }
  }
  m_cv.notify_all();
}

void OpenclPowBackend::setWorkerBatchPolicy(uint32_t minBatchSize, uint32_t maxWaitUs)
{
  if (!m_ready)
    return;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_minBatchSize = minBatchSize;
    m_maxWaitUs = maxWaitUs;
  }
  m_cv.notify_all();
}

void OpenclPowBackend::shutdown()
{
  stopWorker();
}

void OpenclPowBackend::startWorker()
{
  m_worker = std::thread([this] { workerLoop(); });
}

void OpenclPowBackend::stopWorker()
{
  if (!m_worker.joinable())
    return;
  m_cv.notify_all();
  flush();
  m_shutdown.store(true);
  m_cv.notify_all();
  m_worker.join();
}

bool OpenclPowBackend::jobKnownLocked(const crypto::Hash& blockId, uint32_t blockHeight) const
{
  if (m_completed.count(blockId))
    return true;
  if (m_pending.count(blockId))
    return true;
  if (blockHeight != UINT32_MAX && m_resultCache.hasReady(blockHeight, blockId))
    return true;
  for (const GpuJob& j : m_queue)
  {
    if (j.blockId == blockId)
      return true;
  }
  return false;
}

void OpenclPowBackend::workerLoop()
{
  for (;;)
  {
    std::vector<GpuJob> batch;
  batch_wait:
    {
      std::unique_lock<std::mutex> lk(m_mutex);
      const auto maxWait = std::chrono::microseconds(m_maxWaitUs > 0 ? m_maxWaitUs : 5000);
      m_cv.wait_for(lk, maxWait, [&] {
        if (m_shutdown.load())
          return true;
        if (m_queue.empty())
          return false;
        if (m_queue.size() >= m_batchSize)
          return true;
        if (m_minBatchSize > 0 && m_queue.size() >= m_minBatchSize)
          return true;
        if (!m_hasOldestQueued)
          return false;
        const auto age = std::chrono::steady_clock::now() - m_oldestQueued;
        return age >= maxWait;
      });

      if (m_queue.empty())
      {
        if (m_shutdown.load() && m_pending.empty())
          return;
        continue;
      }

      const size_t n = std::min(m_queue.size(), static_cast<size_t>(m_batchSize));
      batch.reserve(n);
      const uint32_t validatedTip = m_validatedTip.load(std::memory_order_acquire);
      while (batch.size() < n && !m_queue.empty())
      {
        GpuJob job = std::move(m_queue.front());
        m_queue.pop_front();
        if (job.height != UINT32_MAX && job.height <= validatedTip)
        {
          m_pending.erase(job.blockId);
          continue;
        }
        batch.push_back(std::move(job));
      }
      if (m_queue.empty())
        m_hasOldestQueued = false;
      else if (batch.empty())
        m_oldestQueued = std::chrono::steady_clock::now();

      if (!batch.empty() && m_minBatchSize > 0 && batch.size() < m_minBatchSize &&
          !m_shutdown.load() && m_hasOldestQueued)
      {
        const auto age = std::chrono::steady_clock::now() - m_oldestQueued;
        if (age < maxWait)
        {
          for (auto it = batch.rbegin(); it != batch.rend(); ++it)
            m_queue.push_front(std::move(*it));
          batch.clear();
          goto batch_wait;
        }
      }
    }

    if (!batch.empty())
    {
      uint32_t minH = UINT32_MAX;
      uint32_t maxH = 0;
      bool anyHeight = false;
      for (const GpuJob& j : batch)
      {
        if (j.height == UINT32_MAX)
          continue;
        anyHeight = true;
        minH = std::min(minH, j.height);
        maxH = std::max(maxH, j.height);
      }
      if (anyHeight && powSyncLogEnabled(minH))
      {
        std::ostringstream oss;
        oss << "CN-GPU dispatch " << batch.size() << " job(s) to GPU";
        if (minH == maxH)
          oss << " height=" << minH;
        else
          oss << " heights " << minH << ".." << maxH;
        powSyncLogDebug(oss.str());
      }
    }

    if (!executeBatch(batch))
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      const uint32_t validatedTip = m_validatedTip.load(std::memory_order_acquire);
      for (GpuJob& job : batch)
      {
        if (job.height != UINT32_MAX && job.height <= validatedTip)
        {
          m_pending.erase(job.blockId);
          continue;
        }
        GpuResult r;
        crypto::cn_gpu_run_inner_reference(*job.ctx);
        crypto::cn_gpu_finish_hash(*job.ctx, r.hash);
        r.ok = true;
        if (job.height != UINT32_MAX)
        {
          m_resultCache.store(job.height, job.blockId, r.hash, true);
          if (powSyncLogEnabled(job.height))
          {
            std::ostringstream oss;
            oss << "CN-GPU ready height=" << job.height << " id=" << powHashShort(job.blockId)
                << " (CPU fallback inner)";
            powSyncLogDebug(oss.str());
          }
        }
        m_completed[job.blockId] = r;
        m_pending.erase(job.blockId);
        ++m_metrics.cpuFallbackCount;
        ++m_metrics.jobsSubmitted;
      }
      m_cv.notify_all();
      continue;
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    const uint32_t validatedTip = m_validatedTip.load(std::memory_order_acquire);
    for (size_t i = 0; i < batch.size(); ++i)
    {
      if (batch[i].height != UINT32_MAX && batch[i].height <= validatedTip)
      {
        m_pending.erase(batch[i].blockId);
        continue;
      }
      GpuResult r;
      r.ok = true;
      crypto::cn_gpu_finish_hash(*batch[i].ctx, r.hash);
      if (m_debugCrossCheck && !batch[i].blob.empty())
      {
        crypto::Hash cpuRef;
        crypto::cn_gpu_hash_staged_reference(*batch[i].ctx, batch[i].blob.data(), batch[i].blob.size(),
                                             cpuRef);
        if (cpuRef != r.hash)
        {
          r.ok = false;
          ++m_metrics.mismatchCount;
          powLog("GPU/CPU PoW mismatch on cross-check");
        }
      }
      if (batch[i].height != UINT32_MAX && r.ok)
      {
        m_resultCache.store(batch[i].height, batch[i].blockId, r.hash, true);
        if (powSyncLogEnabled(batch[i].height))
        {
          std::ostringstream oss;
          oss << "CN-GPU ready height=" << batch[i].height << " id=" << powHashShort(batch[i].blockId);
          powSyncLogDebug(oss.str());
        }
      }
      m_completed[batch[i].blockId] = r;
      m_pending.erase(batch[i].blockId);
    }
    m_cv.notify_all();
  }
}

bool OpenclPowBackend::executeBatch(std::vector<GpuJob>& batch)
{
  if (batch.empty())
    return true;

  const auto t0 = std::chrono::steady_clock::now();

  std::vector<crypto::cn_context*> contexts;
  contexts.reserve(batch.size());
  for (GpuJob& j : batch)
    contexts.push_back(j.ctx.get());

  if (!runGpuInnerEnqueue(contexts, 0))
    return false;

  const auto t1 = std::chrono::steady_clock::now();
  m_metrics.gpuVerifyNs += static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

  ++m_metrics.batchesExecuted;
  const size_t n = batch.size();
  m_metrics.jobsSubmitted += n;
  m_metrics.avgBatchSize = (m_metrics.avgBatchSize * (m_metrics.batchesExecuted - 1) + n) /
                           m_metrics.batchesExecuted;
  return true;
}

bool OpenclPowBackend::verifyPow(crypto::cn_context& ctx, const void* data, size_t length,
                                 difficulty_type difficulty, crypto::Hash& proofOfWork)
{
  if (!computeLonghash(ctx, data, length, proofOfWork))
    return false;
  return check_hash(proofOfWork, difficulty);
}

#ifdef CONCEAL_WITH_OPENCL

bool OpenclPowBackend::initOpencl(int deviceIndex)
{
  m_ocl.reset(new OpenclState());
  std::string info;
  if (!ocl::selectDevice(deviceIndex, m_ocl->platform, m_ocl->device, info))
  {
    powLog(info);
    m_ocl.reset();
    return false;
  }
  powLog(info);

  cl_int err = CL_SUCCESS;
  m_ocl->context = clCreateContext(nullptr, 1, &m_ocl->device, nullptr, nullptr, &err);
  if (!m_ocl->context || err != CL_SUCCESS)
    return false;

  m_ocl->queue = clCreateCommandQueue(m_ocl->context, m_ocl->device, 0, &err);
  if (!m_ocl->queue || err != CL_SUCCESS)
    return false;

  std::string source = readKernelFile();
  if (source.empty())
  {
    powLog("Could not load cn_gpu_inner.cl");
    return false;
  }

  /* No -cl-fast-relaxed-math: inner_hash_3 must match CPU SSE ordering and rounding. */
  const char* buildOpts =
      "-cl-single-precision-constant -cl-fp32-correctly-rounded-divide-sqrt";
  {
    std::string buildErr;
    if (!buildOpenclProgramCached(m_ocl->context, m_ocl->device, deviceIndex, "inner", source,
                                  buildOpts, m_ocl->program, buildErr))
    {
      powLog(buildErr.empty() ? "OpenCL program build failed" : buildErr);
      return false;
    }
  }

  m_ocl->innerKernel = clCreateKernel(m_ocl->program, "cn_gpu_inner", &err);
  if (!m_ocl->innerKernel || err != CL_SUCCESS)
    return false;

  m_ocl->maxJobs = m_batchSize > 0 ? m_batchSize : 16;
  const size_t spadBytes = m_ocl->maxJobs * 25 * sizeof(uint64_t);
  const size_t lpadBytes = m_ocl->maxJobs * crypto::cn_gpu_scratchpad_bytes();

  m_ocl->spads = clCreateBuffer(m_ocl->context, CL_MEM_READ_WRITE, spadBytes, nullptr, &err);
  m_ocl->lpads = clCreateBuffer(m_ocl->context, CL_MEM_READ_WRITE, lpadBytes, nullptr, &err);
  if (!m_ocl->spads || !m_ocl->lpads || err != CL_SUCCESS)
    return false;
  m_ocl->hostSpads.resize(m_ocl->maxJobs * 25);
  m_ocl->hostLpads.resize(m_ocl->maxJobs * crypto::cn_gpu_scratchpad_bytes());
  return true;
}

namespace
{
size_t firstScratchpadDiff(const uint8_t* a, const uint8_t* b, size_t nbytes)
{
  for (size_t i = 0; i < nbytes; ++i)
  {
    if (a[i] != b[i])
      return i;
  }
  return nbytes;
}
} // namespace

bool OpenclPowBackend::runSelfTest()
{
  const char testInput[] = "conceal-gpu-opencl-selftest";
  const size_t testLen = sizeof(testInput) - 1;
  const size_t padBytes = crypto::cn_gpu_scratchpad_bytes();

#ifdef HAS_INTEL_HW
  powLog(std::string("Self-test CPU inner reference: ") +
         (check_avx2() ? "inner_hash_3_avx (production on this CPU)"
                       : "inner_hash_3 (no AVX2)"));
#elif defined(HAS_ARM_HW)
  powLog("Self-test CPU inner reference: inner_hash_3 (ARM)");
#else
  powLog("Self-test CPU inner reference: software path");
#endif

  crypto::cn_context ctxRef;
  crypto::cn_context ctxGpu;
  crypto::Hash refHash, gpuHash;

  crypto::cn_gpu_prepare_inner(ctxRef, testInput, testLen);
  crypto::cn_gpu_prepare_inner(ctxGpu, testInput, testLen);
  const size_t prepDiff = firstScratchpadDiff(crypto::cn_gpu_scratchpad_ptr(ctxRef),
                                              crypto::cn_gpu_scratchpad_ptr(ctxGpu), padBytes);
  if (prepDiff < padBytes)
  {
    powLog("Self-test: scratchpad differs after prepare_inner at offset " + std::to_string(prepDiff));
    return false;
  }
  if (memcmp(crypto::cn_gpu_state_ptr(ctxRef), crypto::cn_gpu_state_ptr(ctxGpu), 200) != 0)
  {
    powLog("Self-test: keccak state (spad) differs after prepare_inner");
    return false;
  }

  crypto::cn_gpu_run_inner_reference_iters(ctxRef, 1);
  std::vector<crypto::cn_context*> gpuOne = {&ctxGpu};
  if (!runGpuInnerEnqueue(gpuOne, 1))
    return false;
  const size_t iter1Diff = firstScratchpadDiff(crypto::cn_gpu_scratchpad_ptr(ctxRef),
                                               crypto::cn_gpu_scratchpad_ptr(ctxGpu), padBytes);
  if (iter1Diff < padBytes)
  {
    powLog("Self-test: scratchpad differs after 1 inner iter at offset " + std::to_string(iter1Diff) +
           " (GPU OpenCL vs CPU production inner)");
    crypto::cn_gpu_dump_scratchpad_words("cpu_ref", crypto::cn_gpu_scratchpad_ptr(ctxRef), 809920, 8);
    crypto::cn_gpu_dump_scratchpad_words("gpu", crypto::cn_gpu_scratchpad_ptr(ctxGpu), 809920, 8);
#ifdef HAS_INTEL_HW
    if (check_avx2())
    {
      crypto::cn_context ctxSse;
      crypto::cn_gpu_prepare_inner(ctxSse, testInput, testLen);
      crypto::cn_gpu_run_inner_sse_reference_iters(ctxSse, 1);
      const size_t avxVsSse = firstScratchpadDiff(crypto::cn_gpu_scratchpad_ptr(ctxRef),
                                                  crypto::cn_gpu_scratchpad_ptr(ctxSse), padBytes);
      const size_t gpuVsSse = firstScratchpadDiff(crypto::cn_gpu_scratchpad_ptr(ctxSse),
                                                  crypto::cn_gpu_scratchpad_ptr(ctxGpu), padBytes);
      if (avxVsSse < padBytes)
        powLog("Self-test diag: SSE inner vs AVX2 inner differ after 1 iter at offset " +
               std::to_string(avxVsSse));
      else
        powLog("Self-test diag: SSE and AVX2 inner match after 1 iter (OpenCL targets AVX shape)");
      if (gpuVsSse < padBytes)
        powLog("Self-test diag: GPU vs SSE inner differ after 1 iter at offset " +
               std::to_string(gpuVsSse));
    }
#endif
    return false;
  }

  crypto::cn_context ctxRefFull;
  crypto::cn_context ctxGpuFull;
  crypto::cn_gpu_hash_staged_reference(ctxRefFull, testInput, testLen, refHash);

  std::vector<crypto::cn_context*> contexts = {&ctxGpuFull};
  std::vector<const void*> datas = {testInput};
  std::vector<size_t> lengths = {testLen};
  std::vector<crypto::Hash> outs(1);
  if (!runBatchSync(contexts, datas, lengths, outs))
    return false;

  gpuHash = outs[0];
  if (refHash != gpuHash)
  {
    const size_t fullDiff = firstScratchpadDiff(crypto::cn_gpu_scratchpad_ptr(ctxRefFull),
                                                crypto::cn_gpu_scratchpad_ptr(ctxGpuFull), padBytes);
    powLog("Self-test mismatch (OpenCL inner vs CPU production inner reference)");
    if (fullDiff < padBytes)
      powLog("  scratchpad first diff offset " + std::to_string(fullDiff));
    std::ostringstream oss;
    oss << "  ref ";
    for (size_t i = 0; i < sizeof(refHash.data); ++i)
      oss << std::hex << (int)refHash.data[i];
    oss << " gpu ";
    for (size_t i = 0; i < sizeof(gpuHash.data); ++i)
      oss << std::hex << (int)gpuHash.data[i];
    powLog(oss.str());
    return false;
  }
  return true;
}

bool OpenclPowBackend::runGpuInnerOnly(crypto::cn_context& ctx, const void* data, size_t length,
                                       uint32_t maxIter)
{
  crypto::cn_gpu_prepare_inner(ctx, data, length);
  std::vector<crypto::cn_context*> contexts = {&ctx};
  return runGpuInnerEnqueue(contexts, maxIter);
}

bool OpenclPowBackend::runGpuInnerEnqueue(std::vector<crypto::cn_context*>& contexts,
                                          uint32_t maxIter)
{
  if (!m_ocl)
    return false;

  const size_t n = contexts.size();
  if (n == 0 || n > m_ocl->maxJobs)
    return false;

  const size_t spadBytes = n * 25 * sizeof(uint64_t);
  const size_t lpadBytes = n * crypto::cn_gpu_scratchpad_bytes();
  const size_t scratchBytes = crypto::cn_gpu_scratchpad_bytes();

  for (size_t i = 0; i < n; ++i)
  {
    memcpy(m_ocl->hostSpads.data() + i * 25, crypto::cn_gpu_state_ptr(*contexts[i]), 200);
    memcpy(m_ocl->hostLpads.data() + i * scratchBytes, crypto::cn_gpu_scratchpad_ptr(*contexts[i]),
           scratchBytes);
  }

  cl_int err = CL_SUCCESS;
  err = clEnqueueWriteBuffer(m_ocl->queue, m_ocl->spads, CL_TRUE, 0, spadBytes, m_ocl->hostSpads.data(),
                             0, nullptr, nullptr);
  if (err != CL_SUCCESS)
    return false;
  err = clEnqueueWriteBuffer(m_ocl->queue, m_ocl->lpads, CL_TRUE, 0, lpadBytes, m_ocl->hostLpads.data(),
                             0, nullptr, nullptr);
  if (err != CL_SUCCESS)
    return false;

  const uint32_t count = static_cast<uint32_t>(n);
  err = clSetKernelArg(m_ocl->innerKernel, 0, sizeof(cl_mem), &m_ocl->spads);
  err |= clSetKernelArg(m_ocl->innerKernel, 1, sizeof(cl_mem), &m_ocl->lpads);
  err |= clSetKernelArg(m_ocl->innerKernel, 2, sizeof(uint32_t), &count);
  err |= clSetKernelArg(m_ocl->innerKernel, 3, sizeof(uint32_t), &maxIter);
  if (err != CL_SUCCESS)
    return false;

  size_t gws = n;
  err = clEnqueueNDRangeKernel(m_ocl->queue, m_ocl->innerKernel, 1, nullptr, &gws, nullptr, 0,
                               nullptr, nullptr);
  if (err != CL_SUCCESS)
    return false;
  clFinish(m_ocl->queue);

  err = clEnqueueReadBuffer(m_ocl->queue, m_ocl->spads, CL_TRUE, 0, spadBytes, m_ocl->hostSpads.data(), 0,
                            nullptr, nullptr);
  err |= clEnqueueReadBuffer(m_ocl->queue, m_ocl->lpads, CL_TRUE, 0, lpadBytes, m_ocl->hostLpads.data(),
                             0, nullptr, nullptr);
  if (err != CL_SUCCESS)
    return false;

  for (size_t i = 0; i < n; ++i)
  {
    memcpy(crypto::cn_gpu_state_ptr(*contexts[i]), m_ocl->hostSpads.data() + i * 25, 200);
    memcpy(crypto::cn_gpu_scratchpad_ptr(*contexts[i]), m_ocl->hostLpads.data() + i * scratchBytes,
           scratchBytes);
  }
  return true;
}

bool OpenclPowBackend::runBatchSync(std::vector<crypto::cn_context*>& contexts,
                                    const std::vector<const void*>& datas,
                                    const std::vector<size_t>& lengths,
                                    std::vector<crypto::Hash>& outs, uint32_t maxIter,
                                    bool finishHashes)
{
  if (!m_ocl)
    return false;

  const size_t n = datas.size();
  if (n == 0 || n > m_ocl->maxJobs)
    return false;

  outs.resize(n);
  for (size_t i = 0; i < n; ++i)
    crypto::cn_gpu_prepare_inner(*contexts[i], datas[i], lengths[i]);

  if (!runGpuInnerEnqueue(contexts, maxIter))
    return false;

  for (size_t i = 0; i < n; ++i)
  {
    if (finishHashes)
      crypto::cn_gpu_finish_hash(*contexts[i], outs[i]);
    if (m_debugCrossCheck && finishHashes)
    {
      crypto::Hash cpuRef;
      crypto::cn_gpu_hash_staged_reference(*contexts[i], datas[i], lengths[i], cpuRef);
      if (cpuRef != outs[i])
      {
        ++m_metrics.mismatchCount;
        powLog("GPU/CPU PoW mismatch on cross-check");
        return false;
      }
    }
  }

  ++m_metrics.batchesExecuted;
  m_metrics.avgBatchSize = (m_metrics.avgBatchSize * (m_metrics.batchesExecuted - 1) + n) /
                           m_metrics.batchesExecuted;
  return true;
}

#else

bool OpenclPowBackend::initOpencl(int) { return false; }
bool OpenclPowBackend::runSelfTest() { return false; }
bool OpenclPowBackend::runGpuInnerEnqueue(std::vector<crypto::cn_context*>&, uint32_t)
{
  return false;
}
bool OpenclPowBackend::runGpuInnerOnly(crypto::cn_context&, const void*, size_t, uint32_t)
{
  return false;
}
bool OpenclPowBackend::runBatchSync(std::vector<crypto::cn_context*>&, const std::vector<const void*>&,
                                    const std::vector<size_t>&, std::vector<crypto::Hash>&, uint32_t,
                                    bool)
{
  return false;
}

#endif

} // namespace cn
