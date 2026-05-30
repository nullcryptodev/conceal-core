// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "kernel_loader.hpp"

#include <fstream>
#include <sstream>

namespace cn
{

std::string loadOpenclKernelSource(const std::vector<const char*>& candidates)
{
  for (const char* p : candidates)
  {
    if (!p || !*p)
      continue;
    std::ifstream f(p);
    if (f)
    {
      std::ostringstream ss;
      ss << f.rdbuf();
      return ss.str();
    }
  }
  return {};
}

} // namespace cn
