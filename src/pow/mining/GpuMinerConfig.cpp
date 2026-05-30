// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "GpuMinerConfig.hpp"
#include "gpu_mine_intensity.hpp"

#include <set>
#include <sstream>

#include "../../Common/CommandLine.h"

namespace cn
{

namespace
{
const command_line::arg_descriptor<std::string> arg_start_gpu_mining = {
    "start-gpu-mining",
    "Mine with OpenCL GPUs: <addr,deviceId:intensity[,...]> intensity = number, safe, or boost "
    "(total per GPU, 3 pipelines; capped to min(40% VRAM, 80xCU); worksize fixed at 8)",
    "",
    true};
} // namespace

void GpuMinerConfig::initOptions(boost::program_options::options_description& desc)
{
  command_line::add_arg(desc, arg_start_gpu_mining);
}

void GpuMinerConfig::init(const boost::program_options::variables_map& vm)
{
  clear();
  if (!command_line::has_arg(vm, arg_start_gpu_mining))
    return;

  const std::string value = command_line::get_arg(vm, arg_start_gpu_mining);
  if (value.empty())
    return;

  std::string err;
  if (!parseValue(value, rewardAddress, devices, err))
    throw std::runtime_error("Invalid --start-gpu-mining: " + err);
}

uint32_t GpuMinerConfig::alignTotalIntensity(uint32_t userInput)
{
  if (userInput < kMinTotalIntensity)
    return 0;
  return userInput - (userInput % kMinTotalIntensity);
}

bool GpuMinerConfig::parseDeviceSpec(const std::string& spec, GpuDeviceSpec& out, std::string& err)
{
  const auto colon = spec.find(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= spec.size())
  {
    err = "expected deviceId:intensity, got \"" + spec + "\"";
    return false;
  }

  try
  {
    out.deviceIndex = std::stoi(spec.substr(0, colon));
  }
  catch (const std::exception&)
  {
    err = "invalid device index in \"" + spec + "\"";
    return false;
  }

  if (out.deviceIndex < 0)
  {
    err = "device index must be >= 0";
    return false;
  }

  out.intensityToken = spec.substr(colon + 1);
  uint32_t numeric = 0;
  if (!parseIntensityToken(out.intensityToken, out.intensityMode, numeric, err))
    return false;

  out.requestedIntensity = numeric;
  out.userIntensity = 0;
  out.alignedIntensity = 0;
  out.perThreadIntensity = 0;

  if (out.intensityMode == GpuIntensityMode::Numeric)
  {
    out.alignedIntensity = alignTotalIntensity(numeric);
    if (out.alignedIntensity < kMinTotalIntensity)
    {
      err = "intensity " + std::to_string(numeric) + " too small after alignment (minimum " +
            std::to_string(kMinTotalIntensity) + ", must be a multiple of 3 × worksize " +
            std::to_string(kThreadsPerGpu) + " × " + std::to_string(kWorkSize) + ")";
      return false;
    }
    out.perThreadIntensity = out.alignedIntensity / kThreadsPerGpu;
    out.userIntensity = out.alignedIntensity;
  }

  return true;
}

bool GpuMinerConfig::parseValue(const std::string& value, std::string& rewardAddress,
                                std::vector<GpuDeviceSpec>& devices, std::string& err)
{
  rewardAddress.clear();
  devices.clear();

  std::vector<std::string> parts;
  std::stringstream ss(value);
  std::string item;
  while (std::getline(ss, item, ','))
  {
    if (!item.empty())
      parts.push_back(item);
  }

  if (parts.empty())
  {
    err = "empty value";
    return false;
  }

  rewardAddress = parts.front();
  if (parts.size() == 1)
  {
    err = "missing deviceId:intensity after address";
    return false;
  }

  std::set<int> seenDevices;
  for (size_t i = 1; i < parts.size(); ++i)
  {
    GpuDeviceSpec spec;
    if (!parseDeviceSpec(parts[i], spec, err))
      return false;
    if (!seenDevices.insert(spec.deviceIndex).second)
    {
      err = "duplicate device index " + std::to_string(spec.deviceIndex);
      return false;
    }
    devices.push_back(spec);
  }
  return true;
}

void GpuMinerConfig::clear()
{
  rewardAddress.clear();
  devices.clear();
}

void applyMiningModeExclusivity(const std::string& cpuStartMining, GpuMinerConfig& gpuConfig,
                                const std::function<void(const std::string&)>& warn)
{
  if (!cpuStartMining.empty() && gpuConfig.enabled())
  {
    warn("--start-gpu-mining ignored: --start-mining takes precedence (CPU mining only)");
    gpuConfig.clear();
  }
}

} // namespace cn
