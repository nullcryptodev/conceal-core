// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "opencl_program_cache.hpp"

#ifdef CONCEAL_WITH_OPENCL

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace cn
{

namespace
{
constexpr const char* kCacheDir = "/tmp/conceal-opencl";

void cacheLog(const std::string& msg)
{
  std::cerr << "[pow/opencl-cache] " << msg << std::endl;
}

uint64_t fnv1a64Continue(uint64_t h, const void* data, size_t len)
{
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i)
  {
    h ^= static_cast<uint64_t>(p[i]);
    h *= 0x100000001b3ULL;
  }
  return h;
}

uint64_t programCacheHash(const char* programTag, const char* buildOpts, const std::string& source)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  if (programTag)
    h = fnv1a64Continue(h, programTag, std::strlen(programTag));
  if (buildOpts)
    h = fnv1a64Continue(h, buildOpts, std::strlen(buildOpts));
  h = fnv1a64Continue(h, source.data(), source.size());
  return h;
}

std::string cachePath(int deviceIndex, uint64_t hash)
{
  std::ostringstream os;
  os << kCacheDir << '/' << deviceIndex << '-' << std::hex << std::setw(16) << std::setfill('0')
     << hash << ".bin";
  return os.str();
}

void ensureCacheDir()
{
  if (mkdir(kCacheDir, 0755) != 0 && errno != EEXIST)
    cacheLog(std::string("mkdir ") + kCacheDir + " failed: " + std::strerror(errno));
}

bool readBinaryFile(const std::string& path, std::vector<unsigned char>& out)
{
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    return false;
  const std::streamoff end = f.tellg();
  if (end <= 0)
    return false;
  out.resize(static_cast<size_t>(end));
  f.seekg(0, std::ios::beg);
  if (!f.read(reinterpret_cast<char*>(out.data()), end))
    return false;
  return true;
}

bool writeBinaryFile(const std::string& path, const unsigned char* data, size_t size)
{
  ensureCacheDir();
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
  return static_cast<bool>(f);
}

bool programBuildOk(cl_program prog, cl_device_id device, std::string& err)
{
  cl_build_status status = CL_BUILD_ERROR;
  cl_int clErr =
      clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_STATUS, sizeof(status), &status, nullptr);
  if (clErr != CL_SUCCESS || status != CL_BUILD_SUCCESS)
  {
    size_t logSize = 0;
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);
    std::vector<char> log(logSize + 1, 0);
    clGetProgramBuildInfo(prog, device, CL_PROGRAM_BUILD_LOG, logSize, log.data(), nullptr);
    err = std::string("OpenCL build log:\n") + log.data();
    return false;
  }
  return true;
}

bool buildFromSource(cl_context ctx, cl_device_id device, const std::string& source,
                     const char* buildOpts, cl_program& outProg, std::string& err)
{
  cl_int clErr = CL_SUCCESS;
  const char* srcPtr = source.c_str();
  const size_t srcLen = source.size();
  outProg = clCreateProgramWithSource(ctx, 1, &srcPtr, &srcLen, &clErr);
  if (!outProg || clErr != CL_SUCCESS)
  {
    err = "clCreateProgramWithSource failed";
    return false;
  }
  clErr = clBuildProgram(outProg, 1, &device, buildOpts, nullptr, nullptr);
  if (clErr != CL_SUCCESS || !programBuildOk(outProg, device, err))
  {
    if (err.empty())
      err = "clBuildProgram failed";
    clReleaseProgram(outProg);
    outProg = nullptr;
    return false;
  }
  return true;
}

bool loadFromCache(cl_context ctx, cl_device_id device, const std::string& path,
                   const char* buildOpts, cl_program& outProg, std::string& err)
{
  std::vector<unsigned char> binary;
  if (!readBinaryFile(path, binary))
    return false;

  cl_int clErr = CL_SUCCESS;
  cl_int binaryStatus = CL_SUCCESS;
  const unsigned char* binaryPtr = binary.data();
  const size_t binarySize = binary.size();
  outProg = clCreateProgramWithBinary(ctx, 1, &device, &binarySize, &binaryPtr, &binaryStatus,
                                      &clErr);
  if (!outProg || clErr != CL_SUCCESS || binaryStatus != CL_SUCCESS)
  {
    err = "clCreateProgramWithBinary failed";
    if (outProg)
    {
      clReleaseProgram(outProg);
      outProg = nullptr;
    }
    return false;
  }

  clErr = clBuildProgram(outProg, 1, &device, buildOpts, nullptr, nullptr);
  if (clErr != CL_SUCCESS || !programBuildOk(outProg, device, err))
  {
    if (err.empty())
      err = "cached binary clBuildProgram failed";
    clReleaseProgram(outProg);
    outProg = nullptr;
    return false;
  }
  return true;
}

bool saveProgramBinary(cl_program prog, cl_device_id device, const std::string& path)
{
  size_t binarySize = 0;
  cl_int clErr =
      clGetProgramInfo(prog, CL_PROGRAM_BINARY_SIZES, sizeof(binarySize), &binarySize, nullptr);
  if (clErr != CL_SUCCESS || binarySize == 0)
    return false;

  std::vector<unsigned char> binary(binarySize);
  unsigned char* ptr = binary.data();
  clErr = clGetProgramInfo(prog, CL_PROGRAM_BINARIES, sizeof(unsigned char*), &ptr, nullptr);
  if (clErr != CL_SUCCESS)
    return false;

  if (!writeBinaryFile(path, binary.data(), binary.size()))
    return false;
  (void)device;
  return true;
}

} // namespace

bool buildOpenclProgramCached(cl_context ctx, cl_device_id device, int deviceIndex,
                              const char* programTag, const std::string& source,
                              const char* buildOpts, cl_program& outProg, std::string& err)
{
  outProg = nullptr;
  if (!ctx || !device || !programTag || !buildOpts || source.empty())
  {
    err = "invalid buildOpenclProgramCached arguments";
    return false;
  }

  const uint64_t hash = programCacheHash(programTag, buildOpts, source);
  const std::string path = cachePath(deviceIndex, hash);

  if (loadFromCache(ctx, device, path, buildOpts, outProg, err))
  {
    cacheLog("hit " + path);
    return true;
  }

  err.clear();
  if (!buildFromSource(ctx, device, source, buildOpts, outProg, err))
    return false;

  if (saveProgramBinary(outProg, device, path))
    cacheLog("saved " + path);
  else
    cacheLog("compile ok but failed to save " + path);

  return true;
}

} // namespace cn

#endif // CONCEAL_WITH_OPENCL
