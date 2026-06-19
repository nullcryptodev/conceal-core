// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "OpenclMinerDevice.hpp"

#include "GpuMiner.hpp"
#include "gpu_mine_intensity.hpp"
#include "kernel_loader.hpp"

#include "../../CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "../../CryptoNoteCore/Difficulty.h"
#include "../../crypto/cryptonight.hpp"

#include "../opencl/raii.hpp"
#include "../opencl/opencl_program_cache.hpp"

#include <boost/utility/value_init.hpp>

#include <cstring>
#include <iostream>
#include <sstream>

namespace cn
{

namespace
{
void mineLog(const std::string& msg)
{
  std::cerr << "[pow/gpu-mining] " << msg << std::endl;
}

constexpr size_t kCnGpuMemory = 2u * 1024u * 1024u;
constexpr size_t kStateUlongs = 25;
constexpr size_t kMaxHashingBlobBytes = 128;
} // namespace

#ifndef CONCEAL_WITH_OPENCL
struct OpenclMinerDevice::OclState {};
#else

struct OpenclMinerDevice::OclState
{
  struct Pipeline
  {
    ocl::ClQueue queue;
    ocl::ClKernel cn0;
    ocl::ClKernel cn00;
    ocl::ClKernel cn1;
    ocl::ClMem inputBuf;
    ocl::ClMem statesBuf;
    ocl::ClMem scratchBuf;
    std::vector<uint64_t> hostStates;
    std::vector<uint8_t> hostScratch;
  };

  cl_platform_id platform = nullptr;
  cl_device_id device = nullptr;
  ocl::ClContext context;
  ocl::ClProgram mineProgram;
  uint32_t maxJobs = 0;
  std::array<Pipeline, GpuMinerConfig::kThreadsPerGpu> pipelines;
};

namespace
{
std::string buildMineProgramSource()
{
  const char* keccakPaths[] = {
#ifdef CONCEAL_MINE_KERNEL_DIR
      CONCEAL_MINE_KERNEL_DIR "/cn_gpu_keccak.cl",
#endif
#ifdef CONCEAL_MINE_KERNEL_SOURCE_KECCAK
      CONCEAL_MINE_KERNEL_SOURCE_KECCAK,
#endif
      "cn_gpu_keccak.cl",
      "src/pow/kernels/cn_gpu_keccak.cl",
      "../src/pow/kernels/cn_gpu_keccak.cl",
  };
  const char* minePaths[] = {
#ifdef CONCEAL_MINE_KERNEL_DIR
      CONCEAL_MINE_KERNEL_DIR "/cn_gpu_mine.cl",
#endif
#ifdef CONCEAL_MINE_KERNEL_SOURCE_MINE
      CONCEAL_MINE_KERNEL_SOURCE_MINE,
#endif
      "cn_gpu_mine.cl",
      "src/pow/kernels/cn_gpu_mine.cl",
      "../src/pow/kernels/cn_gpu_mine.cl",
  };
  return loadOpenclKernelSource(std::vector<const char*>(keccakPaths, keccakPaths + 5)) +
         loadOpenclKernelSource(std::vector<const char*>(minePaths, minePaths + 5));
}

cl_mem clMemArg(ocl::ClMem& mem) { return mem.get(); }

void setKernelMem(cl_kernel k, uint argIndex, ocl::ClMem& mem)
{
  cl_mem m = clMemArg(mem);
  clSetKernelArg(k, argIndex, sizeof(cl_mem), &m);
}

std::string clErrString(cl_int code)
{
  return std::string(" (cl ") + std::to_string(static_cast<int>(code)) + ")";
}

} // namespace

#endif // CONCEAL_WITH_OPENCL

OpenclMinerDevice::OpenclMinerDevice(GpuMiner& owner, const GpuDeviceSpec& spec)
    : m_owner(owner), m_spec(spec)
{
}

OpenclMinerDevice::~OpenclMinerDevice()
{
  stopWorkers();
}

bool OpenclMinerDevice::init(std::string& err)
{
#ifndef CONCEAL_WITH_OPENCL
  err = "OpenCL not compiled in";
  return false;
#else
  m_ocl.reset(new OclState());
  std::string info;
  if (!ocl::selectDevice(m_spec.deviceIndex, m_ocl->platform, m_ocl->device, info))
  {
    err = info;
    return false;
  }
  mineLog(info);

  std::string intensityLog;
  GpuDeviceSpec resolved = m_spec;
  if (!resolveDeviceIntensity(resolved, m_ocl->device, intensityLog, err))
    return false;
  m_spec = resolved;
  mineLog(intensityLog);

  cl_int clErr = CL_SUCCESS;
  cl_context rawCtx = clCreateContext(nullptr, 1, &m_ocl->device, nullptr, nullptr, &clErr);
  if (!rawCtx || clErr != CL_SUCCESS)
  {
    err = "clCreateContext failed";
    return false;
  }
  m_ocl->context = ocl::ClContext(rawCtx);

  const uint32_t intensity = m_spec.perThreadIntensity;
  const uint32_t workSize = GpuMinerConfig::kWorkSize;
  std::ostringstream buildOpts;
  buildOpts << "-DWORKSIZE=" << workSize << " -DMEMORY=" << kCnGpuMemory << " -DMASK="
            << (((kCnGpuMemory - 1) >> 6) << 6) << " -DITERATIONS=49152 -DCOMP_MODE=1 "
               << "-DSTRIDED_INDEX=0 -DCN_UNROLL=1 "
               << "-cl-single-precision-constant -cl-fp32-correctly-rounded-divide-sqrt";

  const std::string mineSrc = buildMineProgramSource();
  if (mineSrc.empty())
  {
    err = "Could not load cn_gpu_mine.cl / cn_gpu_keccak.cl";
    return false;
  }

  cl_program rawMine = nullptr;
  if (!buildOpenclProgramCached(m_ocl->context.get(), m_ocl->device, m_spec.deviceIndex, "mine",
                                mineSrc, buildOpts.str().c_str(), rawMine, err))
    return false;
  m_ocl->mineProgram = ocl::ClProgram(rawMine);

  clErr = CL_SUCCESS;
  for (uint32_t t = 0; t < GpuMinerConfig::kThreadsPerGpu; ++t)
  {
    OclState::Pipeline& pipe = m_ocl->pipelines[t];
    pipe.cn0 = ocl::ClKernel(clCreateKernel(rawMine, "cn0_cn_gpu", &clErr));
    pipe.cn00 = ocl::ClKernel(clCreateKernel(rawMine, "cn00_cn_gpu", &clErr));
    pipe.cn1 = ocl::ClKernel(clCreateKernel(rawMine, "cn1_cn_gpu", &clErr));
    if (!pipe.cn0.get() || !pipe.cn00.get() || !pipe.cn1.get() || clErr != CL_SUCCESS)
    {
      err = "Failed to create cn0/cn00/cn1 kernels for pipeline " + std::to_string(t);
      return false;
    }
  }

  m_ocl->maxJobs = intensity;
  const size_t stateBytes = m_ocl->maxJobs * kStateUlongs * sizeof(uint64_t);
  const size_t scratchBytes = m_ocl->maxJobs * kCnGpuMemory;

  for (uint32_t t = 0; t < GpuMinerConfig::kThreadsPerGpu; ++t)
  {
    OclState::Pipeline& pipe = m_ocl->pipelines[t];
    cl_command_queue rawQ = clCreateCommandQueue(m_ocl->context.get(), m_ocl->device, 0, &clErr);
    if (!rawQ || clErr != CL_SUCCESS)
    {
      err = "clCreateCommandQueue failed for pipeline " + std::to_string(t);
      return false;
    }
    pipe.queue = ocl::ClQueue(rawQ);

    pipe.inputBuf = ocl::ClMem(clCreateBuffer(m_ocl->context.get(), CL_MEM_READ_ONLY,
                                              kMaxHashingBlobBytes, nullptr, &clErr));
    pipe.statesBuf =
        ocl::ClMem(clCreateBuffer(m_ocl->context.get(), CL_MEM_READ_WRITE, stateBytes, nullptr, &clErr));
    pipe.scratchBuf =
        ocl::ClMem(clCreateBuffer(m_ocl->context.get(), CL_MEM_READ_WRITE, scratchBytes, nullptr, &clErr));
    if (!pipe.inputBuf.get() || !pipe.statesBuf.get() || !pipe.scratchBuf.get())
    {
      err = "Failed to allocate mining buffers for pipeline " + std::to_string(t);
      return false;
    }

    pipe.hostStates.resize(m_ocl->maxJobs * kStateUlongs);
    pipe.hostScratch.resize(scratchBytes);
  }

  if (!selfTest(err))
    return false;

  return true;
#endif
}

bool OpenclMinerDevice::selfTest(std::string& err) const
{
#ifndef CONCEAL_WITH_OPENCL
  err = "OpenCL not compiled in";
  return false;
#else
  if (!m_ocl)
  {
    err = "not initialized";
    return false;
  }

  const char sample[] = "conceal-gpu-mine-selftest-blob-v1";
  const size_t sampleLen = sizeof(sample) - 1;
  (void)sample;
  (void)sampleLen;

  Block block = boost::value_initialized<Block>();
  block.majorVersion = 8;
  block.minorVersion = 0;
  block.timestamp = 1700000000;
  block.nonce = 0;
  BinaryArray blob;
  if (!get_block_hashing_blob(block, blob))
  {
    err = "self-test get_block_hashing_blob failed";
    return false;
  }

  crypto::Hash refHash;
  crypto::cn_context refCtx;
  if (!get_block_longhash(refCtx, block, refHash))
  {
    err = "self-test get_block_longhash failed";
    return false;
  }

  size_t nonceOffset = 0;
  if (!get_hashing_blob_nonce_offset(block, nonceOffset))
  {
    err = "self-test get_hashing_blob_nonce_offset failed";
    return false;
  }

  if (blob.size() > kMaxHashingBlobBytes)
  {
    err = "self-test hashing blob too large";
    return false;
  }

  std::vector<uint8_t> blobUpload(kMaxHashingBlobBytes, 0);
  memcpy(blobUpload.data(), blob.data(), blob.size());

  OpenclMinerDevice* mut = const_cast<OpenclMinerDevice*>(this);
  OclState::Pipeline& pipe = mut->m_ocl->pipelines[0];

  cl_int clErr = clEnqueueWriteBuffer(pipe.queue.get(), pipe.inputBuf.get(), CL_TRUE, 0,
                                      kMaxHashingBlobBytes, blobUpload.data(), 0, nullptr, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "self-test input upload failed";
    return false;
  }

  const cl_uint threads = 1;
  const cl_uint nb = 0;
  const cl_uint ns = 1;
  const cl_uint blobLen = static_cast<cl_uint>(blob.size());
  const cl_uint nonceOff = static_cast<cl_uint>(nonceOffset);
  setKernelMem(pipe.cn0.get(), 0, pipe.inputBuf);
  setKernelMem(pipe.cn0.get(), 1, pipe.scratchBuf);
  setKernelMem(pipe.cn0.get(), 2, pipe.statesBuf);
  clSetKernelArg(pipe.cn0.get(), 3, sizeof(cl_uint), &threads);
  clSetKernelArg(pipe.cn0.get(), 4, sizeof(cl_uint), &nb);
  clSetKernelArg(pipe.cn0.get(), 5, sizeof(cl_uint), &ns);
  clSetKernelArg(pipe.cn0.get(), 6, sizeof(cl_uint), &blobLen);
  clSetKernelArg(pipe.cn0.get(), 7, sizeof(cl_uint), &nonceOff);

  size_t g0[2] = {8, 8};
  size_t l0[2] = {8, 8};
  clErr = clEnqueueNDRangeKernel(pipe.queue.get(), pipe.cn0.get(), 2, nullptr, g0, l0, 0, nullptr,
                                 nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "self-test cn0 failed" + clErrString(clErr);
    return false;
  }

  setKernelMem(pipe.cn00.get(), 0, pipe.scratchBuf);
  setKernelMem(pipe.cn00.get(), 1, pipe.statesBuf);
  size_t g00 = 64;
  size_t l00 = 64;
  clErr = clEnqueueNDRangeKernel(pipe.queue.get(), pipe.cn00.get(), 1, nullptr, &g00, &l00, 0,
                                 nullptr, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "self-test cn00 failed" + clErrString(clErr);
    return false;
  }

  setKernelMem(pipe.cn1.get(), 0, pipe.scratchBuf);
  setKernelMem(pipe.cn1.get(), 1, pipe.statesBuf);
  clSetKernelArg(pipe.cn1.get(), 2, sizeof(cl_uint), &threads);
  const size_t workSize = GpuMinerConfig::kWorkSize;
  size_t g1 = workSize * 16;
  size_t l1 = workSize * 16;
  clErr = clEnqueueNDRangeKernel(pipe.queue.get(), pipe.cn1.get(), 1, nullptr, &g1, &l1, 0,
                                 nullptr, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "self-test cn1 failed" + clErrString(clErr);
    return false;
  }

  clEnqueueReadBuffer(pipe.queue.get(), pipe.statesBuf.get(), CL_TRUE, 0, kStateUlongs * sizeof(uint64_t),
                      pipe.hostStates.data(), 0, nullptr, nullptr);
  clEnqueueReadBuffer(pipe.queue.get(), pipe.scratchBuf.get(), CL_TRUE, 0, kCnGpuMemory,
                      pipe.hostScratch.data(), 0, nullptr, nullptr);

  crypto::cn_context ctx;
  memcpy(crypto::cn_gpu_state_ptr(ctx), pipe.hostStates.data(), 200);
  memcpy(crypto::cn_gpu_scratchpad_ptr(ctx), pipe.hostScratch.data(), kCnGpuMemory);
  crypto::Hash gpuHash;
  crypto::cn_gpu_finish_hash(ctx, gpuHash);

  if (memcmp(refHash.data, gpuHash.data, sizeof(refHash.data)) != 0)
  {
    err = "GPU mine hash mismatch vs get_block_longhash";
    return false;
  }
  return true;
#endif
}

void OpenclMinerDevice::startWorkers()
{
  if (!m_stop.exchange(false))
    return;
  m_threads.clear();
  for (uint32_t t = 0; t < GpuMinerConfig::kThreadsPerGpu; ++t)
    m_threads.emplace_back(&OpenclMinerDevice::workerLoop, this, t);
}

void OpenclMinerDevice::stopWorkers()
{
  m_stop = true;
  for (auto& th : m_threads)
  {
    if (th.joinable())
      th.join();
  }
  m_threads.clear();
}

void OpenclMinerDevice::workerLoop(uint32_t hostThreadIndex)
{
#ifndef CONCEAL_WITH_OPENCL
  (void)hostThreadIndex;
  return;
#else
  Block block;
  difficulty_type diff = 0;
  uint32_t templateVer = 0;
  const uint32_t globalIdx = m_globalThreadBase + hostThreadIndex;
  uint32_t nonce = m_owner.starter_nonce() + globalIdx;

  uint32_t localTemplateVer = 0;

  while (!m_stop)
  {
    if (m_owner.is_paused())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (!m_owner.worker_wait_template(block, diff, templateVer))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }

    if (block.majorVersion < 8)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      continue;
    }

    if (localTemplateVer != templateVer)
    {
      localTemplateVer = templateVer;
      nonce = m_owner.starter_nonce() + globalIdx;
    }

    const uint32_t jobs = m_spec.perThreadIntensity;
    bool found = false;
    uint32_t foundNonce = 0;
    if (runMiningBatch(hostThreadIndex, block, diff, nonce, jobs, found, foundNonce))
    {
      m_owner.add_hashes(jobs);
      if (found)
      {
        Block foundBlock = block;
        foundBlock.nonce = foundNonce;
        m_owner.worker_report_found(foundBlock);
      }
    }

    nonce += m_owner.total_host_threads() * jobs;
  }
#endif
}

bool OpenclMinerDevice::runMiningBatch(uint32_t hostThreadIndex, const Block& block,
                                       difficulty_type diff, uint32_t nonceBase, uint32_t jobCount,
                                       bool& found, uint32_t& foundNonce)
{
#ifndef CONCEAL_WITH_OPENCL
  (void)hostThreadIndex;
  (void)block;
  (void)diff;
  (void)nonceBase;
  (void)jobCount;
  found = false;
  foundNonce = 0;
  return false;
#else
  found = false;
  foundNonce = 0;
  if (!m_ocl || jobCount == 0 || jobCount > m_ocl->maxJobs ||
      hostThreadIndex >= GpuMinerConfig::kThreadsPerGpu)
    return false;

  OclState::Pipeline& pipe = m_ocl->pipelines[hostThreadIndex];

  BinaryArray blob;
  Block workBlock = block;
  workBlock.nonce = 0;
  if (!get_block_hashing_blob(workBlock, blob))
    return false;

  size_t nonceOffset = 0;
  if (!get_hashing_blob_nonce_offset(workBlock, nonceOffset))
    return false;

  if (blob.size() > kMaxHashingBlobBytes)
    return false;

  std::vector<uint8_t> blobUpload(kMaxHashingBlobBytes, 0);
  memcpy(blobUpload.data(), blob.data(), blob.size());

  cl_int clErr = CL_SUCCESS;

  clErr = clEnqueueWriteBuffer(pipe.queue.get(), pipe.inputBuf.get(), CL_TRUE, 0,
                               kMaxHashingBlobBytes, blobUpload.data(), 0, nullptr, nullptr);
  if (clErr != CL_SUCCESS)
    return false;

  const uint32_t stride = m_owner.total_host_threads();
  if (stride == 0)
    return false;

  cl_uint threads = jobCount;
  cl_uint nb = nonceBase;
  cl_uint ns = stride;
  const cl_uint blobLen = static_cast<cl_uint>(blob.size());
  const cl_uint nonceOff = static_cast<cl_uint>(nonceOffset);
  setKernelMem(pipe.cn0.get(), 0, pipe.inputBuf);
  setKernelMem(pipe.cn0.get(), 1, pipe.scratchBuf);
  setKernelMem(pipe.cn0.get(), 2, pipe.statesBuf);
  clSetKernelArg(pipe.cn0.get(), 3, sizeof(cl_uint), &threads);
  clSetKernelArg(pipe.cn0.get(), 4, sizeof(cl_uint), &nb);
  clSetKernelArg(pipe.cn0.get(), 5, sizeof(cl_uint), &ns);
  clSetKernelArg(pipe.cn0.get(), 6, sizeof(cl_uint), &blobLen);
  clSetKernelArg(pipe.cn0.get(), 7, sizeof(cl_uint), &nonceOff);

  size_t g0[2] = {jobCount, 8};
  size_t l0[2] = {8, 8};
  clErr = clEnqueueNDRangeKernel(pipe.queue.get(), pipe.cn0.get(), 2, nullptr, g0, l0, 0, nullptr,
                                 nullptr);
  if (clErr != CL_SUCCESS)
    return false;

  setKernelMem(pipe.cn00.get(), 0, pipe.scratchBuf);
  setKernelMem(pipe.cn00.get(), 1, pipe.statesBuf);
  size_t g00 = jobCount * 64;
  size_t l00 = 64;
  clErr = clEnqueueNDRangeKernel(pipe.queue.get(), pipe.cn00.get(), 1, nullptr, &g00, &l00, 0,
                                 nullptr, nullptr);
  if (clErr != CL_SUCCESS)
    return false;

  setKernelMem(pipe.cn1.get(), 0, pipe.scratchBuf);
  setKernelMem(pipe.cn1.get(), 1, pipe.statesBuf);
  clSetKernelArg(pipe.cn1.get(), 2, sizeof(cl_uint), &threads);
  const size_t workSize = GpuMinerConfig::kWorkSize;
  size_t g1 = jobCount * 16;
  size_t l1 = workSize * 16;
  clErr = clEnqueueNDRangeKernel(pipe.queue.get(), pipe.cn1.get(), 1, nullptr, &g1, &l1, 0, nullptr,
                                 nullptr);
  if (clErr != CL_SUCCESS)
    return false;

  clEnqueueReadBuffer(pipe.queue.get(), pipe.statesBuf.get(), CL_TRUE, 0,
                      jobCount * kStateUlongs * sizeof(uint64_t), pipe.hostStates.data(), 0, nullptr,
                      nullptr);
  clEnqueueReadBuffer(pipe.queue.get(), pipe.scratchBuf.get(), CL_TRUE, 0, jobCount * kCnGpuMemory,
                      pipe.hostScratch.data(), 0, nullptr, nullptr);

  for (uint32_t j = 0; j < jobCount; ++j)
  {
    crypto::cn_context ctx;
    memcpy(crypto::cn_gpu_state_ptr(ctx), pipe.hostStates.data() + j * kStateUlongs, 200);
    memcpy(crypto::cn_gpu_scratchpad_ptr(ctx), pipe.hostScratch.data() + j * kCnGpuMemory,
           kCnGpuMemory);
    crypto::Hash h;
    crypto::cn_gpu_finish_hash(ctx, h);
    if (!check_hash(h, diff))
      continue;

    found = true;
    foundNonce = nonceBase + j * stride;
    return true;
  }
  return true;
#endif
}

} // namespace cn
