// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT — independent OpenCL RAII helpers.

#pragma once

#ifdef CONCEAL_WITH_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#include <CL/cl.h>
#include <string>

namespace cn
{
namespace ocl
{

class ClContext
{
public:
  ClContext() = default;
  explicit ClContext(cl_context ctx) : m_ctx(ctx) {}
  ~ClContext();
  ClContext(const ClContext&) = delete;
  ClContext& operator=(const ClContext&) = delete;
  ClContext(ClContext&& o) noexcept;
  ClContext& operator=(ClContext&& o) noexcept;
  cl_context get() const { return m_ctx; }
  explicit operator bool() const { return m_ctx != nullptr; }

private:
  cl_context m_ctx = nullptr;
};

class ClQueue
{
public:
  ClQueue() = default;
  ClQueue(cl_command_queue q) : m_q(q) {}
  ~ClQueue();
  ClQueue(const ClQueue&) = delete;
  ClQueue& operator=(const ClQueue&) = delete;
  ClQueue(ClQueue&& o) noexcept;
  ClQueue& operator=(ClQueue&& o) noexcept;
  cl_command_queue get() const { return m_q; }

private:
  cl_command_queue m_q = nullptr;
};

class ClProgram
{
public:
  ClProgram() = default;
  ClProgram(cl_program p) : m_p(p) {}
  ~ClProgram();
  ClProgram(const ClProgram&) = delete;
  ClProgram& operator=(const ClProgram&) = delete;
  ClProgram(ClProgram&& o) noexcept;
  ClProgram& operator=(ClProgram&& o) noexcept;
  cl_program get() const { return m_p; }

private:
  cl_program m_p = nullptr;
};

class ClKernel
{
public:
  ClKernel() = default;
  ClKernel(cl_kernel k) : m_k(k) {}
  ~ClKernel();
  ClKernel(const ClKernel&) = delete;
  ClKernel& operator=(const ClKernel&) = delete;
  ClKernel(ClKernel&& o) noexcept;
  ClKernel& operator=(ClKernel&& o) noexcept;
  cl_kernel get() const { return m_k; }

private:
  cl_kernel m_k = nullptr;
};

class ClMem
{
public:
  ClMem() = default;
  ClMem(cl_mem m) : m_m(m) {}
  ~ClMem();
  ClMem(const ClMem&) = delete;
  ClMem& operator=(const ClMem&) = delete;
  ClMem(ClMem&& o) noexcept;
  ClMem& operator=(ClMem&& o) noexcept;
  cl_mem get() const { return m_m; }

private:
  cl_mem m_m = nullptr;
};

std::string getPlatformListText();
std::string getDeviceListText();
bool selectDevice(int globalGpuIndex, cl_platform_id& plat, cl_device_id& dev, std::string& infoLog);

} // namespace ocl
} // namespace cn

#endif
