// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "backend.hpp"
#include "cpu_backend.hpp"

#ifdef CONCEAL_WITH_OPENCL
#include "opencl/opencl_backend.hpp"
#endif

namespace cn
{

std::unique_ptr<PowVerifyBackend> createPowVerifyBackend(int gpuDeviceIndex, uint32_t batchSize,
                                                         uint32_t minBatchSize, uint32_t maxWaitUs,
                                                         bool debugCrossCheck, bool debugInnerTrace)
{
#ifdef CONCEAL_WITH_OPENCL
  if (gpuDeviceIndex >= 0)
  {
    auto ocl = std::unique_ptr<PowVerifyBackend>(
        new OpenclPowBackend(gpuDeviceIndex, batchSize, minBatchSize, maxWaitUs, debugCrossCheck,
                             debugInnerTrace));
    if (ocl->available())
      return ocl;
  }
#endif
  (void)gpuDeviceIndex;
  (void)batchSize;
  (void)minBatchSize;
  (void)maxWaitUs;
  (void)debugCrossCheck;
  (void)debugInnerTrace;
  return std::unique_ptr<PowVerifyBackend>(new CpuPowBackend());
}

} // namespace cn
