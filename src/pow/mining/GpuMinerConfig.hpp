// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

namespace cn
{

enum class GpuIntensityMode
{
  Numeric,
  Safe,
  Boost,
};

struct GpuDeviceSpec
{
  int deviceIndex = -1;
  GpuIntensityMode intensityMode = GpuIntensityMode::Numeric;
  /** Numeric request before device limits (0 for safe/boost/max until resolved). */
  uint32_t requestedIntensity = 0;
  /** Total after limits and alignment (effective intensity). */
  uint32_t userIntensity = 0;
  uint32_t alignedIntensity = 0;
  /** Per host thread: alignedIntensity / 3 (OpenCL Threads / numThreads). */
  uint32_t perThreadIntensity = 0;
  std::string intensityToken;
};

struct GpuMinerConfig
{
  /** OpenCL cn1 work group factor (xmr-stak worksize); not user-configurable. */
  static constexpr uint32_t kWorkSize = 8;
  static constexpr uint32_t kThreadsPerGpu = 3;
  static constexpr uint32_t kMinTotalIntensity = kThreadsPerGpu * kWorkSize;

  static void initOptions(boost::program_options::options_description& desc);
  void init(const boost::program_options::variables_map& vm);

  static uint32_t alignTotalIntensity(uint32_t userInput);
  static bool parseDeviceSpec(const std::string& spec, GpuDeviceSpec& out, std::string& err);
  static bool parseValue(const std::string& value, std::string& rewardAddress,
                         std::vector<GpuDeviceSpec>& devices, std::string& err);

  bool enabled() const { return !rewardAddress.empty() && !devices.empty(); }
  void clear();

  std::string rewardAddress;
  std::vector<GpuDeviceSpec> devices;
};

/** If CPU mining is configured, discard GPU mining config (CPU takes precedence). */
void applyMiningModeExclusivity(const std::string& cpuStartMining, GpuMinerConfig& gpuConfig,
                                const std::function<void(const std::string&)>& warn);

} // namespace cn
