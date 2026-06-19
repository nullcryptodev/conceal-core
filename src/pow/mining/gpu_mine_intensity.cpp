// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "gpu_mine_intensity.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace cn
{

namespace
{
constexpr size_t kScratchPerJob = 2u * 1024u * 1024u;
constexpr double kVramCapFraction = 0.40;
constexpr uint32_t kCuCapMultiplier = 80;
constexpr uint32_t kModeSafePerCu = 64;
constexpr uint32_t kModeBoostPerCu = 72;

std::string toLower(std::string s)
{
  for (char& c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

#ifdef CONCEAL_WITH_OPENCL

uint32_t rawModeIntensity(GpuIntensityMode mode, cl_uint computeUnits)
{
  const uint32_t cu = static_cast<uint32_t>(computeUnits);
  switch (mode)
  {
  case GpuIntensityMode::Safe:
    return kModeSafePerCu * cu;
  case GpuIntensityMode::Boost:
    return kModeBoostPerCu * cu;
  default:
    return 0;
  }
}

uint32_t capByMaxAlloc(uint32_t totalIntensity, cl_ulong maxAllocBytes)
{
  if (maxAllocBytes == 0 || totalIntensity < GpuMinerConfig::kMinTotalIntensity)
    return totalIntensity;

  const uint32_t jobsPerPipeline = totalIntensity / GpuMinerConfig::kThreadsPerGpu;
  const cl_ulong scratchPerPipeline = static_cast<cl_ulong>(jobsPerPipeline) * kScratchPerJob;
  if (scratchPerPipeline <= maxAllocBytes)
    return totalIntensity;

  const uint32_t jobsPerPipelineMax =
      static_cast<uint32_t>(maxAllocBytes / kScratchPerJob);
  if (jobsPerPipelineMax == 0)
    return 0;

  return jobsPerPipelineMax * GpuMinerConfig::kThreadsPerGpu;
}

#endif // CONCEAL_WITH_OPENCL

std::string intensitySummary(const GpuDeviceSpec& spec)
{
  std::ostringstream os;
  os << spec.alignedIntensity << " -> 3x" << spec.perThreadIntensity << " (worksize "
     << GpuMinerConfig::kWorkSize << ", " << GpuMinerConfig::kThreadsPerGpu
     << " independent pipelines)";
  return os.str();
}

const char* modeLabel(GpuIntensityMode mode)
{
  switch (mode)
  {
  case GpuIntensityMode::Safe:
    return "safe";
  case GpuIntensityMode::Boost:
    return "boost";
  default:
    return nullptr;
  }
}

} // namespace

bool parseIntensityToken(const std::string& token, GpuIntensityMode& mode, uint32_t& numeric,
                         std::string& err)
{
  mode = GpuIntensityMode::Numeric;
  numeric = 0;

  const std::string key = toLower(token);
  if (key == "safe")
  {
    mode = GpuIntensityMode::Safe;
    return true;
  }
  if (key == "boost")
  {
    mode = GpuIntensityMode::Boost;
    return true;
  }
  try
  {
    numeric = static_cast<uint32_t>(std::stoul(token));
  }
  catch (const std::exception&)
  {
    err = "expected intensity number or safe|boost, got \"" + token + "\"";
    return false;
  }

  if (numeric == 0)
  {
    err = "intensity must be > 0";
    return false;
  }
  return true;
}

#ifdef CONCEAL_WITH_OPENCL

bool queryDeviceIntensityLimits(cl_device_id device, GpuDeviceIntensityLimits& out,
                                std::string& err)
{
  out = GpuDeviceIntensityLimits{};
  if (!device)
  {
    err = "invalid OpenCL device";
    return false;
  }

  cl_int clErr = CL_SUCCESS;
  cl_uint cu = 0;
  cl_ulong globalMem = 0;
  cl_ulong maxAlloc = 0;

  clErr = clGetDeviceInfo(device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cu), &cu, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "clGetDeviceInfo(CL_DEVICE_MAX_COMPUTE_UNITS) failed";
    return false;
  }

  clErr = clGetDeviceInfo(device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(globalMem), &globalMem, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "clGetDeviceInfo(CL_DEVICE_GLOBAL_MEM_SIZE) failed";
    return false;
  }

  clErr =
      clGetDeviceInfo(device, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(maxAlloc), &maxAlloc, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "clGetDeviceInfo(CL_DEVICE_MAX_MEM_ALLOC_SIZE) failed";
    return false;
  }

  out.computeUnits = cu;
  out.globalMemBytes = globalMem;
  out.maxMemAllocBytes = maxAlloc;

  const uint64_t vramBytes =
      static_cast<uint64_t>(static_cast<double>(globalMem) * kVramCapFraction);
  out.maxByVram = static_cast<uint32_t>(vramBytes / kScratchPerJob);
  out.maxByCu = kCuCapMultiplier * static_cast<uint32_t>(cu);
  out.cap = std::min(out.maxByVram, out.maxByCu);
  out.cap = capByMaxAlloc(out.cap, maxAlloc);
  out.cap = GpuMinerConfig::alignTotalIntensity(out.cap);
  return true;
}

bool resolveDeviceIntensity(GpuDeviceSpec& spec, cl_device_id device, std::string& log,
                            std::string& err)
{
  log.clear();

  GpuDeviceIntensityLimits limits;
  if (!queryDeviceIntensityLimits(device, limits, err))
    return false;

  uint32_t requested = spec.requestedIntensity;
  if (spec.intensityMode != GpuIntensityMode::Numeric)
    requested = rawModeIntensity(spec.intensityMode, limits.computeUnits);

  uint32_t target = requested;
  if (limits.cap > 0 && target > limits.cap)
    target = limits.cap;

  target = capByMaxAlloc(target, limits.maxMemAllocBytes);
  spec.alignedIntensity = GpuMinerConfig::alignTotalIntensity(target);
  if (spec.alignedIntensity < GpuMinerConfig::kMinTotalIntensity)
  {
    err = "intensity " + std::to_string(requested) + " too small after limits (minimum " +
          std::to_string(GpuMinerConfig::kMinTotalIntensity) +
          "; device cap=" + std::to_string(limits.cap) + " CU=" +
          std::to_string(limits.computeUnits) + ")";
    return false;
  }

  const uint32_t unconstrainedAligned = GpuMinerConfig::alignTotalIntensity(
      capByMaxAlloc(requested, limits.maxMemAllocBytes));
  const bool restricted = spec.alignedIntensity < unconstrainedAligned;

  spec.perThreadIntensity = spec.alignedIntensity / GpuMinerConfig::kThreadsPerGpu;
  spec.userIntensity = spec.alignedIntensity;

  std::ostringstream os;
  os << "GPU " << spec.deviceIndex << ": ";

  if (const char* label = modeLabel(spec.intensityMode))
  {
    os << "mode " << label << " ("
       << (spec.intensityMode == GpuIntensityMode::Safe ? kModeSafePerCu : kModeBoostPerCu)
       << " x CU " << limits.computeUnits << ")";
    if (restricted)
      os << " clamped from " << requested;
    os << " -> " << intensitySummary(spec);
  }
  else
  {
    os << "intensity ";
    if (restricted)
      os << spec.requestedIntensity << " clamped to ";
    os << intensitySummary(spec);
  }

  if (restricted)
  {
    os << " [cap: min(40% VRAM=" << limits.maxByVram << ", " << kCuCapMultiplier << "xCU="
       << limits.maxByCu << ")=" << limits.cap << " MiB-equivalent jobs, "
       << (limits.globalMemBytes / (1024 * 1024)) << " MiB VRAM, CU=" << limits.computeUnits << "]";
  }

  if (spec.intensityMode == GpuIntensityMode::Boost)
    os << " — high load; use stable overclock and adequate cooling";

  log = os.str();
  return true;
}

#endif // CONCEAL_WITH_OPENCL

} // namespace cn
