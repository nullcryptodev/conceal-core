// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace cn
{

struct GpuMineStagedProbeConfig
{
  int deviceIndex = 0;
  uint32_t nonce = 0;
};

/** Run CPU vs GPU CN-GPU mining pipeline stage-by-stage; prints report to out. Returns true if all stages match. */
bool runGpuMineStagedProbe(const GpuMineStagedProbeConfig& cfg, std::ostream& out, std::string& err);

} // namespace cn
