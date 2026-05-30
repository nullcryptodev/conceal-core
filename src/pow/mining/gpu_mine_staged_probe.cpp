// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "gpu_mine_staged_probe.hpp"

#include "GpuMinerConfig.hpp"
#include "kernel_loader.hpp"

#include "../../CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "../../crypto/cryptonight.hpp"

#include "../opencl/raii.hpp"
#include "../opencl/opencl_program_cache.hpp"

#include <boost/utility/value_init.hpp>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

namespace cn
{

namespace
{
constexpr size_t kCnGpuMemory = 2u * 1024u * 1024u;
constexpr size_t kStateUlongs = 25;
constexpr size_t kStateBytes = 200;
constexpr size_t kMaxHashingBlobBytes = 128;

size_t firstByteDiff(const uint8_t* a, const uint8_t* b, size_t n)
{
  for (size_t i = 0; i < n; ++i)
  {
    if (a[i] != b[i])
      return i;
  }
  return n;
}

void hexLine(std::ostream& os, const char* label, const uint8_t* data, size_t len, size_t maxBytes)
{
  os << label;
  const size_t n = std::min(len, maxBytes);
  os << std::hex << std::setfill('0');
  for (size_t i = 0; i < n; ++i)
    os << std::setw(2) << static_cast<unsigned>(data[i]);
  if (len > maxBytes)
    os << "...";
  os << std::dec << "\n";
}

void dumpScratchWords(std::ostream& os, const char* tag, const uint8_t* scratch, size_t off,
                      size_t count)
{
  if (off + count * 4 > kCnGpuMemory)
    return;
  const int32_t* words = reinterpret_cast<const int32_t*>(scratch + off);
  os << "  " << tag << " scratch+" << off << ":";
  for (size_t i = 0; i < count; ++i)
    os << " " << words[i];
  os << "\n";
}

#ifdef CONCEAL_WITH_OPENCL

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

std::string buildOpts(uint32_t iterations)
{
  std::ostringstream os;
  os << "-DWORKSIZE=" << GpuMinerConfig::kWorkSize << " -DMEMORY=" << kCnGpuMemory << " -DMASK="
     << (((kCnGpuMemory - 1) >> 6) << 6) << " -DITERATIONS=" << iterations << " -DCOMP_MODE=1 "
        << "-DSTRIDED_INDEX=0 -DCN_UNROLL=1 "
        << "-cl-single-precision-constant -cl-fp32-correctly-rounded-divide-sqrt";
  return os.str();
}

void setKernelMem(cl_kernel k, uint argIndex, cl_mem mem)
{
  clSetKernelArg(k, argIndex, sizeof(cl_mem), &mem);
}

struct OclMineCtx
{
  ocl::ClContext context;
  ocl::ClQueue queue;
  ocl::ClProgram programFull;
  ocl::ClProgram programOneIter;
  ocl::ClKernel cn0;
  ocl::ClKernel cn00;
  ocl::ClKernel cn1Full;
  ocl::ClKernel cn1One;
  ocl::ClMem inputBuf;
  ocl::ClMem statesBuf;
  ocl::ClMem scratchBuf;
  std::vector<uint64_t> hostStates;
  std::vector<uint8_t> hostScratch;
};

bool initOcl(int deviceIndex, OclMineCtx& ocl, std::string& err)
{
  cl_platform_id plat = nullptr;
  cl_device_id dev = nullptr;
  std::string info;
  if (!ocl::selectDevice(deviceIndex, plat, dev, info))
  {
    err = info;
    return false;
  }

  cl_int clErr = CL_SUCCESS;
  cl_context rawCtx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &clErr);
  if (!rawCtx || clErr != CL_SUCCESS)
  {
    err = "clCreateContext failed";
    return false;
  }
  ocl.context = ocl::ClContext(rawCtx);

  cl_command_queue rawQ = clCreateCommandQueue(ocl.context.get(), dev, 0, &clErr);
  if (!rawQ || clErr != CL_SUCCESS)
  {
    err = "clCreateCommandQueue failed";
    return false;
  }
  ocl.queue = ocl::ClQueue(rawQ);

  const std::string src = buildMineProgramSource();
  if (src.empty())
  {
    err = "Could not load cn_gpu_mine.cl / cn_gpu_keccak.cl";
    return false;
  }

  const std::string optsFull = buildOpts(49152);
  const std::string optsOne = buildOpts(1);
  cl_program rawFull = nullptr;
  cl_program rawOne = nullptr;
  if (!buildOpenclProgramCached(ocl.context.get(), dev, deviceIndex, "mine", src, optsFull.c_str(),
                                rawFull, err))
    return false;
  if (!buildOpenclProgramCached(ocl.context.get(), dev, deviceIndex, "mine", src, optsOne.c_str(),
                                rawOne, err))
    return false;
  ocl.programFull = ocl::ClProgram(rawFull);
  ocl.programOneIter = ocl::ClProgram(rawOne);

  clErr = CL_SUCCESS;
  ocl.cn0 = ocl::ClKernel(clCreateKernel(rawFull, "cn0_cn_gpu", &clErr));
  ocl.cn00 = ocl::ClKernel(clCreateKernel(rawFull, "cn00_cn_gpu", &clErr));
  ocl.cn1Full = ocl::ClKernel(clCreateKernel(rawFull, "cn1_cn_gpu", &clErr));
  ocl.cn1One = ocl::ClKernel(clCreateKernel(rawOne, "cn1_cn_gpu", &clErr));
  if (!ocl.cn0.get() || !ocl.cn00.get() || !ocl.cn1Full.get() || !ocl.cn1One.get() ||
      clErr != CL_SUCCESS)
  {
    err = "Failed to create cn0/cn00/cn1 kernels";
    return false;
  }

  ocl.inputBuf = ocl::ClMem(clCreateBuffer(ocl.context.get(), CL_MEM_READ_ONLY, kMaxHashingBlobBytes,
                                           nullptr, &clErr));
  ocl.statesBuf = ocl::ClMem(clCreateBuffer(ocl.context.get(), CL_MEM_READ_WRITE,
                                            kStateUlongs * sizeof(uint64_t), nullptr, &clErr));
  ocl.scratchBuf =
      ocl::ClMem(clCreateBuffer(ocl.context.get(), CL_MEM_READ_WRITE, kCnGpuMemory, nullptr, &clErr));
  if (!ocl.inputBuf.get() || !ocl.statesBuf.get() || !ocl.scratchBuf.get())
  {
    err = "Failed to allocate OpenCL buffers";
    return false;
  }

  ocl.hostStates.resize(kStateUlongs);
  ocl.hostScratch.resize(kCnGpuMemory);
  return true;
}

bool gpuRunPrepare(OclMineCtx& ocl, const uint8_t* blob, size_t blobLen, size_t nonceOffset,
                   uint32_t nonce, std::string& err)
{
  if (blobLen > kMaxHashingBlobBytes)
  {
    err = "blob too large";
    return false;
  }

  std::vector<uint8_t> upload(kMaxHashingBlobBytes, 0);
  memcpy(upload.data(), blob, blobLen);

  cl_int clErr = clEnqueueWriteBuffer(ocl.queue.get(), ocl.inputBuf.get(), CL_TRUE, 0,
                                      kMaxHashingBlobBytes, upload.data(), 0, nullptr, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "input upload failed";
    return false;
  }

  const cl_uint threads = 1;
  const cl_uint nb = nonce;
  const cl_uint ns = 1;
  const cl_uint blobLenArg = static_cast<cl_uint>(blobLen);
  const cl_uint nonceOff = static_cast<cl_uint>(nonceOffset);
  setKernelMem(ocl.cn0.get(), 0, ocl.inputBuf.get());
  setKernelMem(ocl.cn0.get(), 1, ocl.scratchBuf.get());
  setKernelMem(ocl.cn0.get(), 2, ocl.statesBuf.get());
  clSetKernelArg(ocl.cn0.get(), 3, sizeof(cl_uint), &threads);
  clSetKernelArg(ocl.cn0.get(), 4, sizeof(cl_uint), &nb);
  clSetKernelArg(ocl.cn0.get(), 5, sizeof(cl_uint), &ns);
  clSetKernelArg(ocl.cn0.get(), 6, sizeof(cl_uint), &blobLenArg);
  clSetKernelArg(ocl.cn0.get(), 7, sizeof(cl_uint), &nonceOff);

  size_t g0[2] = {8, 8};
  size_t l0[2] = {8, 8};
  clErr = clEnqueueNDRangeKernel(ocl.queue.get(), ocl.cn0.get(), 2, nullptr, g0, l0, 0, nullptr,
                                 nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "cn0 failed";
    return false;
  }

  setKernelMem(ocl.cn00.get(), 0, ocl.scratchBuf.get());
  setKernelMem(ocl.cn00.get(), 1, ocl.statesBuf.get());
  size_t g00 = 64;
  size_t l00 = 64;
  clErr = clEnqueueNDRangeKernel(ocl.queue.get(), ocl.cn00.get(), 1, nullptr, &g00, &l00, 0,
                                 nullptr, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "cn00 failed";
    return false;
  }

  clEnqueueReadBuffer(ocl.queue.get(), ocl.statesBuf.get(), CL_TRUE, 0, kStateBytes,
                      ocl.hostStates.data(), 0, nullptr, nullptr);
  clEnqueueReadBuffer(ocl.queue.get(), ocl.scratchBuf.get(), CL_TRUE, 0, kCnGpuMemory,
                      ocl.hostScratch.data(), 0, nullptr, nullptr);
  return true;
}

bool gpuRunCn1(OclMineCtx& ocl, cl_kernel cn1, std::string& err)
{
  const cl_uint threads = 1;
  setKernelMem(cn1, 0, ocl.scratchBuf.get());
  setKernelMem(cn1, 1, ocl.statesBuf.get());
  clSetKernelArg(cn1, 2, sizeof(cl_uint), &threads);

  const size_t workSize = GpuMinerConfig::kWorkSize;
  size_t g1 = workSize * 16;
  size_t l1 = workSize * 16;
  cl_int clErr =
      clEnqueueNDRangeKernel(ocl.queue.get(), cn1, 1, nullptr, &g1, &l1, 0, nullptr, nullptr);
  if (clErr != CL_SUCCESS)
  {
    err = "cn1 failed";
    return false;
  }

  clEnqueueReadBuffer(ocl.queue.get(), ocl.statesBuf.get(), CL_TRUE, 0, kStateBytes,
                      ocl.hostStates.data(), 0, nullptr, nullptr);
  clEnqueueReadBuffer(ocl.queue.get(), ocl.scratchBuf.get(), CL_TRUE, 0, kCnGpuMemory,
                      ocl.hostScratch.data(), 0, nullptr, nullptr);
  return true;
}

bool compareStage(std::ostream& out, const char* stageName, const uint8_t* cpuState,
                  const uint8_t* gpuState, const uint8_t* cpuScratch, const uint8_t* gpuScratch,
                  size_t scratchOffHint)
{
  const size_t stateDiff = firstByteDiff(cpuState, gpuState, kStateBytes);
  const size_t scratchDiff = firstByteDiff(cpuScratch, gpuScratch, kCnGpuMemory);
  const bool stateOk = stateDiff == kStateBytes;
  const bool scratchOk = scratchDiff == kCnGpuMemory;

  out << "=== " << stageName << " ===\n";
  out << "  state(200B):   " << (stateOk ? "MATCH" : "MISMATCH") << "\n";
  if (!stateOk)
  {
    out << "  state first diff byte: " << stateDiff << "\n";
    hexLine(out, "  cpu state: ", cpuState, kStateBytes, 32);
    hexLine(out, "  gpu state: ", gpuState, kStateBytes, 32);
  }

  out << "  scratch(2MiB): " << (scratchOk ? "MATCH" : "MISMATCH") << "\n";
  if (!scratchOk)
  {
    out << "  scratch first diff byte: " << scratchDiff << "\n";
    const size_t wordOff = scratchDiff - (scratchDiff % 4);
    dumpScratchWords(out, "cpu", cpuScratch, wordOff, 8);
    dumpScratchWords(out, "gpu", gpuScratch, wordOff, 8);
    if (scratchOffHint < kCnGpuMemory)
      dumpScratchWords(out, "cpu@hint", cpuScratch, scratchOffHint, 8);
    if (scratchOffHint < kCnGpuMemory)
      dumpScratchWords(out, "gpu@hint", gpuScratch, scratchOffHint, 8);
  }
  out << "\n";
  return stateOk && scratchOk;
}

#endif // CONCEAL_WITH_OPENCL

Block makeProbeBlock()
{
  Block block = boost::value_initialized<Block>();
  block.majorVersion = 8;
  block.minorVersion = 0;
  block.timestamp = 1700000000;
  block.nonce = 0;
  return block;
}

} // namespace

bool runGpuMineStagedProbe(const GpuMineStagedProbeConfig& cfg, std::ostream& out, std::string& err)
{
#ifndef CONCEAL_WITH_OPENCL
  err = "OpenCL not compiled in (rebuild with -DWITH_OPENCL=ON)";
  return false;
#else
  Block block = makeProbeBlock();
  Block hashBlock = block;
  hashBlock.nonce = cfg.nonce;

  BinaryArray blob;
  block.nonce = 0;
  if (!get_block_hashing_blob(block, blob))
  {
    err = "get_block_hashing_blob failed";
    return false;
  }

  size_t nonceOffset = 0;
  if (!get_hashing_blob_nonce_offset(block, nonceOffset))
  {
    err = "get_hashing_blob_nonce_offset failed";
    return false;
  }

  out << "GpuVsCpuMiningHash staged probe\n";
  out << "  device: " << cfg.deviceIndex << "\n";
  out << "  nonce:  " << cfg.nonce << "\n";
  out << "  blob:   " << blob.size() << " bytes (nonce offset " << nonceOffset << ")\n";
  hexLine(out, "  blob hex: ", blob.data(), blob.size(), 48);
  out << "\n";

  OclMineCtx ocl;
  if (!initOcl(cfg.deviceIndex, ocl, err))
    return false;

  BinaryArray nonceBlob = blob;
  if (nonceOffset + sizeof(uint32_t) <= nonceBlob.size())
  {
    const uint32_t n = cfg.nonce;
    nonceBlob[nonceOffset] = static_cast<uint8_t>(n & 0xFF);
    nonceBlob[nonceOffset + 1] = static_cast<uint8_t>((n >> 8) & 0xFF);
    nonceBlob[nonceOffset + 2] = static_cast<uint8_t>((n >> 16) & 0xFF);
    nonceBlob[nonceOffset + 3] = static_cast<uint8_t>((n >> 24) & 0xFF);
  }

  crypto::cn_context cpuPrep;
  crypto::cn_context cpuOne;
  crypto::cn_context cpuFull;

  crypto::cn_gpu_prepare_inner(cpuPrep, nonceBlob.data(), nonceBlob.size());
  crypto::cn_gpu_prepare_inner(cpuOne, nonceBlob.data(), nonceBlob.size());
  crypto::cn_gpu_prepare_inner(cpuFull, nonceBlob.data(), nonceBlob.size());

  if (!gpuRunPrepare(ocl, blob.data(), blob.size(), nonceOffset, cfg.nonce, err))
    return false;

  bool allOk = true;
  constexpr size_t kInnerHintOff = 809920;

  allOk &= compareStage(out, "after prepare (CPU cn0+cn00 vs GPU cn0+cn00)",
                        crypto::cn_gpu_state_ptr(cpuPrep), reinterpret_cast<uint8_t*>(ocl.hostStates.data()),
                        crypto::cn_gpu_scratchpad_ptr(cpuPrep), ocl.hostScratch.data(), 0);

  crypto::cn_gpu_run_inner_reference_iters(cpuOne, 1);
  if (!gpuRunPrepare(ocl, blob.data(), blob.size(), nonceOffset, cfg.nonce, err))
    return false;
  if (!gpuRunCn1(ocl, ocl.cn1One.get(), err))
    return false;

  allOk &= compareStage(out, "after 1 inner iter (CPU inner x1 vs GPU cn1 ITERATIONS=1)",
                        crypto::cn_gpu_state_ptr(cpuOne), reinterpret_cast<uint8_t*>(ocl.hostStates.data()),
                        crypto::cn_gpu_scratchpad_ptr(cpuOne), ocl.hostScratch.data(), kInnerHintOff);

  crypto::cn_gpu_run_inner_reference(cpuFull);
  if (!gpuRunPrepare(ocl, blob.data(), blob.size(), nonceOffset, cfg.nonce, err))
    return false;
  if (!gpuRunCn1(ocl, ocl.cn1Full.get(), err))
    return false;

  allOk &= compareStage(out, "after full inner (CPU inner x49152 vs GPU cn1 ITERATIONS=49152)",
                        crypto::cn_gpu_state_ptr(cpuFull), reinterpret_cast<uint8_t*>(ocl.hostStates.data()),
                        crypto::cn_gpu_scratchpad_ptr(cpuFull), ocl.hostScratch.data(), kInnerHintOff);

  crypto::Hash cpuHash;
  crypto::Hash gpuHash;
  crypto::cn_gpu_finish_hash(cpuFull, cpuHash);

  crypto::cn_context gpuFinishCtx;
  memcpy(crypto::cn_gpu_state_ptr(gpuFinishCtx), ocl.hostStates.data(), kStateBytes);
  memcpy(crypto::cn_gpu_scratchpad_ptr(gpuFinishCtx), ocl.hostScratch.data(), kCnGpuMemory);
  crypto::cn_gpu_finish_hash(gpuFinishCtx, gpuHash);

  out << "=== after finish (32-byte hash) ===\n";
  const bool hashOk = memcmp(cpuHash.data, gpuHash.data, sizeof(cpuHash.data)) == 0;
  out << "  hash: " << (hashOk ? "MATCH" : "MISMATCH") << "\n";
  hexLine(out, "  cpu: ", cpuHash.data, 32, 32);
  hexLine(out, "  gpu: ", gpuHash.data, 32, 32);
  out << "\n";

  crypto::Hash networkHash;
  crypto::cn_context networkCtx;
  if (!get_block_longhash(networkCtx, hashBlock, networkHash))
  {
    err = "get_block_longhash failed";
    return false;
  }

  out << "=== network longhash (get_block_longhash) ===\n";
  hexLine(out, "  network: ", networkHash.data, 32, 32);
  const bool networkOk = memcmp(cpuHash.data, networkHash.data, sizeof(cpuHash.data)) == 0;
  out << "  mining vs network: " << (networkOk ? "MATCH" : "MISMATCH") << "\n\n";

  allOk &= hashOk;
  allOk &= networkOk;
  if (!allOk)
    err = "one or more stages mismatched (see report above)";
  return allOk;
#endif
}

} // namespace cn
