// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <string>
#include <vector>

namespace cn
{

/** Load OpenCL source from the first readable path in candidates. */
std::string loadOpenclKernelSource(const std::vector<const char*>& candidates);

} // namespace cn
