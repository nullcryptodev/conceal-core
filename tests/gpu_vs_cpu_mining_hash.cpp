// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT
//
// Staged CPU vs GPU CN-GPU mining pipeline comparison.
// Usage: GpuVsCpuMiningHashTests [--device N] [--nonce N]

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../src/pow/mining/gpu_mine_staged_probe.hpp"

namespace
{
void usage(const char* argv0)
{
  std::fprintf(stderr,
               "Usage: %s [--device N] [--nonce N]\n"
               "  Compare CPU vs GPU at each CN-GPU mining stage (prepare / 1-iter inner / full inner / finish).\n"
               "  Requires OpenCL build (-DWITH_OPENCL=ON).\n",
               argv0);
}
} // namespace

int main(int argc, char** argv)
{
#ifndef CONCEAL_WITH_OPENCL
  std::fprintf(stderr, "GpuVsCpuMiningHashTests: skipped (OpenCL not enabled at build time)\n");
  return 77;
#else
  cn::GpuMineStagedProbeConfig cfg;

  for (int i = 1; i < argc; ++i)
  {
    const std::string arg = argv[i];
    if (arg == "--device" && i + 1 < argc)
      cfg.deviceIndex = std::atoi(argv[++i]);
    else if (arg == "--nonce" && i + 1 < argc)
      cfg.nonce = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    else if (arg == "--help" || arg == "-h")
    {
      usage(argv[0]);
      return 0;
    }
    else
    {
      std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
      usage(argv[0]);
      return 2;
    }
  }

  std::string err;
  const bool ok = cn::runGpuMineStagedProbe(cfg, std::cout, err);
  if (!ok)
  {
    std::fprintf(stderr, "GpuVsCpuMiningHashTests: FAILED — %s\n", err.c_str());
    return 1;
  }

  std::fprintf(stderr, "GpuVsCpuMiningHashTests: all stages matched\n");
  return 0;
#endif
}
