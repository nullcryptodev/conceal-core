// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include <cstdio>
#include <cstring>
#include <vector>

#include "../src/crypto/cryptonight.hpp"
#include "../src/pow/GpuPowConfig.h"
#include "../src/pow/backend.hpp"
#include "../src/pow/pow_service.hpp"

using namespace cn;

static bool hashEq(const crypto::Hash& a, const crypto::Hash& b)
{
  return memcmp(a.data, b.data, sizeof(a.data)) == 0;
}

static int testSyncLonghash(PowVerifyBackend& backend)
{
  crypto::cn_context ctx;
  const char* samples[] = {
      "conceal-gpu-equivalence-test-0",
      "conceal-gpu-equivalence-test-1-longer-input-blob",
  };

  int failures = 0;
  for (const char* sample : samples)
  {
    const size_t len = strlen(sample);
    crypto::Hash cpuHash, backendHash;
    crypto::cn_gpu_hash_staged_reference(ctx, sample, len, cpuHash);
    if (!backend.computeLonghash(ctx, sample, len, backendHash))
    {
      ++failures;
      continue;
    }
    if (!hashEq(cpuHash, backendHash))
      ++failures;
  }
  return failures;
}

static int testAsyncSubmit(PowVerifyBackend& backend)
{
  if (!backend.gpuAsyncOffload())
    return 0;

  crypto::cn_context unused;
  const char sample[] = "conceal-gpu-async-submit-equivalence";
  const size_t len = sizeof(sample) - 1;

  crypto::Hash blockId;
  memset(blockId.data, 0xab, sizeof(blockId.data));

  if (!backend.submitLonghash(unused, sample, len, blockId, 2070500u))
  {
    std::fprintf(stderr, "pow_gpu_equivalence: async submit failed\n");
    return 1;
  }

  crypto::Hash gpuHash;
  if (!backend.awaitLonghash(blockId, gpuHash))
  {
    std::fprintf(stderr, "pow_gpu_equivalence: async await failed\n");
    return 1;
  }

  crypto::cn_context refCtx;
  crypto::Hash cpuHash;
  crypto::cn_gpu_hash_staged_reference(refCtx, sample, len, cpuHash);
  if (!hashEq(cpuHash, gpuHash))
  {
    std::fprintf(stderr, "pow_gpu_equivalence: async GPU hash mismatch\n");
    return 1;
  }
  return 0;
}

static int testConcurrentSubmits(PowVerifyBackend& backend)
{
  if (!backend.gpuAsyncOffload())
    return 0;

  crypto::cn_context unused;
  const char* samples[] = {
      "conceal-gpu-concurrent-a",
      "conceal-gpu-concurrent-b",
      "conceal-gpu-concurrent-c",
      "conceal-gpu-concurrent-d",
  };

  std::vector<crypto::Hash> ids(4);
  for (size_t i = 0; i < 4; ++i)
  {
    memset(ids[i].data, static_cast<int>(0x10 + i), sizeof(ids[i].data));
    if (!backend.submitLonghash(unused, samples[i], strlen(samples[i]), ids[i], 2070600u + static_cast<uint32_t>(i)))
    {
      std::fprintf(stderr, "pow_gpu_equivalence: concurrent submit %zu failed\n", i);
      return 1;
    }
  }

  crypto::cn_context refCtx;
  for (size_t i = 0; i < 4; ++i)
  {
    crypto::Hash gpuHash;
    if (!backend.awaitLonghash(ids[i], gpuHash))
    {
      std::fprintf(stderr, "pow_gpu_equivalence: concurrent await %zu failed\n", i);
      return 1;
    }
    crypto::Hash cpuHash;
    crypto::cn_gpu_hash_staged_reference(refCtx, samples[i], strlen(samples[i]), cpuHash);
    if (!hashEq(cpuHash, gpuHash))
    {
      std::fprintf(stderr, "pow_gpu_equivalence: concurrent mismatch at %zu\n", i);
      return 1;
    }
  }
  return 0;
}

int main()
{
  GpuPowConfig cfg;
  cfg.deviceIndex = 0;
  cfg.batchSize = 32;
  cfg.maxWaitUs = 5000;
  cfg.backlogThreshold = 0;
  cfg.prefetchQueueDepth = 64;
  PowService::instance().init(
      createPowVerifyBackend(0, cfg.batchSize, cfg.minBatchSize, cfg.maxWaitUs, false, false), cfg);
  PowVerifyBackend& backend = PowService::instance().backend();

  int failures = 0;
  failures += testSyncLonghash(backend);
  failures += testAsyncSubmit(backend);
  failures += testConcurrentSubmits(backend);

  if (failures != 0)
  {
    std::fprintf(stderr, "pow_gpu_equivalence: %d failures\n", failures);
    return 1;
  }

  std::printf("pow_gpu_equivalence: OK (sync + async + concurrent, backend=%s)\n",
              backend.available() ? "OpenCL" : "CPU");
  return 0;
}
