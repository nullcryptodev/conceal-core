// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#ifdef CONCEAL_WITH_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>

#include <string>

namespace cn
{

/** Build OpenCL program from source, or load a cached device binary from
 *  /tmp/conceal-opencl/<deviceIndex>-<hash>.bin when present.
 *  @a programTag distinguishes programs (e.g. "mine", "inner") in the cache key. */
bool buildOpenclProgramCached(cl_context ctx, cl_device_id device, int deviceIndex,
                              const char* programTag, const std::string& source,
                              const char* buildOpts, cl_program& outProg, std::string& err);

} // namespace cn

#endif // CONCEAL_WITH_OPENCL
