// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include "GpuMinerConfig.hpp"

#include <cstdint>
#include <string>

#ifdef CONCEAL_WITH_OPENCL
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>
#endif

namespace cn
{

struct GpuDeviceIntensityLimits
{
  uint32_t maxByVram = 0;
  uint32_t maxByCu = 0;
  uint32_t cap = 0;
#ifdef CONCEAL_WITH_OPENCL
  cl_uint computeUnits = 0;
  cl_ulong globalMemBytes = 0;
  cl_ulong maxMemAllocBytes = 0;
#endif
};

/** Parse intensity token after `deviceId:` (number, safe, boost, max). */
bool parseIntensityToken(const std::string& token, GpuIntensityMode& mode, uint32_t& numeric,
                         std::string& err);

#ifdef CONCEAL_WITH_OPENCL
bool queryDeviceIntensityLimits(cl_device_id device, GpuDeviceIntensityLimits& out,
                                std::string& err);

/** Apply mode / numeric request, VRAM+CU cap, alignment; fills log lines for miner. */
bool resolveDeviceIntensity(GpuDeviceSpec& spec, cl_device_id device, std::string& log,
                            std::string& err);
#endif

} // namespace cn
