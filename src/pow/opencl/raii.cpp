// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT — independent OpenCL RAII helpers.

#include "raii.hpp"

#ifdef CONCEAL_WITH_OPENCL

#include <sstream>
#include <vector>

#ifdef __linux__
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <unistd.h>
#endif

namespace cn
{
namespace ocl
{

#define OCLCHK(expr)                                                                                 \
  do                                                                                               \
  {                                                                                                \
    cl_int err = (expr);                                                                           \
    if (err != CL_SUCCESS)                                                                         \
      return false;                                                                                \
  } while (0)

ClContext::~ClContext()
{
  if (m_ctx)
    clReleaseContext(m_ctx);
}

ClContext::ClContext(ClContext&& o) noexcept : m_ctx(o.m_ctx) { o.m_ctx = nullptr; }

ClContext& ClContext::operator=(ClContext&& o) noexcept
{
  if (this != &o)
  {
    if (m_ctx)
      clReleaseContext(m_ctx);
    m_ctx = o.m_ctx;
    o.m_ctx = nullptr;
  }
  return *this;
}

ClQueue::~ClQueue()
{
  if (m_q)
    clReleaseCommandQueue(m_q);
}

ClQueue::ClQueue(ClQueue&& o) noexcept : m_q(o.m_q) { o.m_q = nullptr; }

ClQueue& ClQueue::operator=(ClQueue&& o) noexcept
{
  if (this != &o)
  {
    if (m_q)
      clReleaseCommandQueue(m_q);
    m_q = o.m_q;
    o.m_q = nullptr;
  }
  return *this;
}

ClProgram::~ClProgram()
{
  if (m_p)
    clReleaseProgram(m_p);
}

ClProgram::ClProgram(ClProgram&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }

ClProgram& ClProgram::operator=(ClProgram&& o) noexcept
{
  if (this != &o)
  {
    if (m_p)
      clReleaseProgram(m_p);
    m_p = o.m_p;
    o.m_p = nullptr;
  }
  return *this;
}

ClKernel::~ClKernel()
{
  if (m_k)
    clReleaseKernel(m_k);
}

ClKernel::ClKernel(ClKernel&& o) noexcept : m_k(o.m_k) { o.m_k = nullptr; }

ClKernel& ClKernel::operator=(ClKernel&& o) noexcept
{
  if (this != &o)
  {
    if (m_k)
      clReleaseKernel(m_k);
    m_k = o.m_k;
    o.m_k = nullptr;
  }
  return *this;
}

ClMem::~ClMem()
{
  if (m_m)
    clReleaseMemObject(m_m);
}

ClMem::ClMem(ClMem&& o) noexcept : m_m(o.m_m) { o.m_m = nullptr; }

ClMem& ClMem::operator=(ClMem&& o) noexcept
{
  if (this != &o)
  {
    if (m_m)
      clReleaseMemObject(m_m);
    m_m = o.m_m;
    o.m_m = nullptr;
  }
  return *this;
}

static const char* deviceTypeLabel(cl_device_type type)
{
  if (type & CL_DEVICE_TYPE_GPU)
    return "GPU";
  if (type & CL_DEVICE_TYPE_ACCELERATOR)
    return "ACCELERATOR";
  if (type & CL_DEVICE_TYPE_CUSTOM)
    return "CUSTOM";
  if (type & CL_DEVICE_TYPE_CPU)
    return "CPU";
  return "OTHER";
}

/** Collect non-CPU compute devices; tries GPU, then GPU|ACCELERATOR, then ALL\\CPU. */
static cl_int getComputeDevices(cl_platform_id platform, std::vector<cl_device_id>& out)
{
  out.clear();
  const cl_device_type queries[] = {
      CL_DEVICE_TYPE_GPU,
      static_cast<cl_device_type>(CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR),
      CL_DEVICE_TYPE_ALL,
  };

  for (cl_device_type query : queries)
  {
    cl_uint numDevices = 0;
    cl_int err = clGetDeviceIDs(platform, query, 0, nullptr, &numDevices);
    if (err != CL_SUCCESS || numDevices == 0)
      continue;

    std::vector<cl_device_id> devices(numDevices);
    err = clGetDeviceIDs(platform, query, numDevices, devices.data(), nullptr);
    if (err != CL_SUCCESS)
      continue;

    for (cl_device_id dev : devices)
    {
      cl_device_type type = 0;
      clGetDeviceInfo(dev, CL_DEVICE_TYPE, sizeof(type), &type, nullptr);
      if (query == CL_DEVICE_TYPE_ALL && (type & CL_DEVICE_TYPE_CPU))
        continue;
      out.push_back(dev);
    }

    if (!out.empty())
      return CL_SUCCESS;
    out.clear();
  }

  return CL_DEVICE_NOT_FOUND;
}

std::string getPlatformListText()
{
  std::ostringstream os;
  cl_uint numPlatforms = 0;
  clGetPlatformIDs(0, nullptr, &numPlatforms);
  if (numPlatforms == 0)
  {
    os << "No OpenCL platforms found.\n";
    return os.str();
  }

  std::vector<cl_platform_id> platforms(numPlatforms);
  clGetPlatformIDs(numPlatforms, platforms.data(), nullptr);

  for (cl_uint p = 0; p < numPlatforms; ++p)
  {
    char platName[256] = {};
    clGetPlatformInfo(platforms[p], CL_PLATFORM_NAME, sizeof(platName), platName, nullptr);
    os << "Platform " << p << ": " << platName << "\n";
    std::vector<cl_device_id> devices;
    if (getComputeDevices(platforms[p], devices) == CL_SUCCESS)
    {
      for (cl_device_id dev : devices)
      {
        char devName[256] = {};
        cl_device_type type = 0;
        clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(devName), devName, nullptr);
        clGetDeviceInfo(dev, CL_DEVICE_TYPE, sizeof(type), &type, nullptr);
        os << "  [" << platName << "] " << devName << " (" << deviceTypeLabel(type) << ")\n";
      }
    }
  }
  return os.str();
}

#ifdef __linux__
/** PCI display class (0x03xx) visible to the kernel — does not imply OpenCL can use it. */
static void appendPciDisplayHint(std::ostringstream& os)
{
  DIR* dir = opendir("/sys/bus/pci/devices");
  if (!dir)
    return;

  os << "  PCI display adapters (kernel view; compare to OpenCL list above):\n";
  bool any = false;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr)
  {
    if (ent->d_name[0] == '.')
      continue;

    char path[512];
    char cls[32] = {};
    char vendor[16] = {};
    char device[16] = {};
    char driver[64] = {};

    std::snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", ent->d_name);
    FILE* f = std::fopen(path, "r");
    if (!f)
      continue;
    if (!std::fgets(cls, sizeof(cls), f))
    {
      std::fclose(f);
      continue;
    }
    std::fclose(f);

    const unsigned classId = static_cast<unsigned>(std::strtoul(cls, nullptr, 16));
    if ((classId >> 16) != 0x03)
      continue;

    std::snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", ent->d_name);
    f = std::fopen(path, "r");
    if (f)
    {
      std::fgets(vendor, sizeof(vendor), f);
      std::fclose(f);
    }
    std::snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", ent->d_name);
    f = std::fopen(path, "r");
    if (f)
    {
      std::fgets(device, sizeof(device), f);
      std::fclose(f);
    }
    std::snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", ent->d_name);
    char link[256] = {};
    const ssize_t n = readlink(path, link, sizeof(link) - 1);
    if (n > 0)
    {
      link[n] = '\0';
      const char* slash = std::strrchr(link, '/');
      std::strncpy(driver, slash ? slash + 1 : link, sizeof(driver) - 1);
    }

    os << "    " << ent->d_name << ": " << vendor << ":" << device;
    if (driver[0])
      os << " driver=" << driver;
    os << "\n";
    any = true;
  }
  closedir(dir);
  if (!any)
    os << "    (none)\n";
  else
    os << "  If GPUs appear here but OpenCL is empty: fix ROCm/ICD, and ensure your user is in groups\n"
          "  `render` and `video` (for /dev/dri/renderD*). Miners often run with those permissions.\n";
}
#endif

static const char* clErrorString(cl_int err)
{
  switch (err)
  {
  case CL_SUCCESS: return "CL_SUCCESS";
  case CL_DEVICE_NOT_FOUND: return "CL_DEVICE_NOT_FOUND";
  case CL_INVALID_VALUE: return "CL_INVALID_VALUE";
  case CL_OUT_OF_HOST_MEMORY: return "CL_OUT_OF_HOST_MEMORY";
  default: return "OpenCL error";
  }
}

std::string getDeviceListText()
{
  std::ostringstream os;
  os << "OpenCL compute devices (global index for --gpu-device N):\n";

  cl_uint numPlatforms = 0;
  cl_int platErr = clGetPlatformIDs(0, nullptr, &numPlatforms);
  if (platErr != CL_SUCCESS || numPlatforms == 0)
  {
    os << "  No OpenCL platforms";
    if (platErr != CL_SUCCESS)
      os << " (" << clErrorString(platErr) << ")";
    os << ".\n";
    os << "  Install a GPU driver and OpenCL ICD (e.g. nvidia-driver + nvidia-opencl-icd, or rocm-opencl).\n";
    return os.str();
  }

  std::vector<cl_platform_id> platforms(numPlatforms);
  clGetPlatformIDs(numPlatforms, platforms.data(), nullptr);

  int globalIdx = 0;
  for (cl_uint p = 0; p < numPlatforms; ++p)
  {
    char platName[256] = {};
    clGetPlatformInfo(platforms[p], CL_PLATFORM_NAME, sizeof(platName), platName, nullptr);

    std::vector<cl_device_id> devices;
    cl_int devErr = getComputeDevices(platforms[p], devices);
    if (devErr != CL_SUCCESS || devices.empty())
    {
      os << "  Platform " << p << " \"" << platName << "\": 0 compute devices";
      if (devErr != CL_SUCCESS)
        os << " (" << clErrorString(devErr) << ")";
      os << "\n";
      continue;
    }

    for (cl_device_id dev : devices)
    {
      char devName[256] = {};
      cl_ulong mem = 0;
      cl_uint cu = 0;
      cl_device_type type = 0;
      clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(devName), devName, nullptr);
      clGetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(mem), &mem, nullptr);
      clGetDeviceInfo(dev, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cu), &cu, nullptr);
      clGetDeviceInfo(dev, CL_DEVICE_TYPE, sizeof(type), &type, nullptr);
      os << "  " << globalIdx << ": " << platName << " / " << devName << " (" << deviceTypeLabel(type)
         << ", CU=" << cu << ", global_mem=" << (mem / (1024 * 1024)) << " MiB)\n";
      ++globalIdx;
    }
  }

  if (globalIdx == 0)
  {
    os << "  No OpenCL compute devices on any platform (tried GPU, GPU|ACCELERATOR, ALL except CPU).\n";
    os << "  Run `clinfo` — if it also shows 0 devices, install a working GPU OpenCL stack\n";
    os << "  (AMD: rocm-opencl / amdgpu-pro; NVIDIA: proprietary driver + opencl-icd).\n";
#ifdef __linux__
    appendPciDisplayHint(os);
#endif
  }
  return os.str();
}

bool selectDevice(int globalGpuIndex, cl_platform_id& plat, cl_device_id& dev, std::string& infoLog)
{
  if (globalGpuIndex < 0)
    return false;

  cl_uint numPlatforms = 0;
  if (clGetPlatformIDs(0, nullptr, &numPlatforms) != CL_SUCCESS || numPlatforms == 0)
    return false;

  std::vector<cl_platform_id> platforms(numPlatforms);
  clGetPlatformIDs(numPlatforms, platforms.data(), nullptr);

  int idx = 0;
  for (cl_uint p = 0; p < numPlatforms; ++p)
  {
    std::vector<cl_device_id> devices;
    if (getComputeDevices(platforms[p], devices) != CL_SUCCESS)
      continue;

    for (cl_device_id device : devices)
    {
      if (idx == globalGpuIndex)
      {
        plat = platforms[p];
        dev = device;
        char buf[512] = {};
        cl_device_type type = 0;
        clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(buf), buf, nullptr);
        clGetDeviceInfo(dev, CL_DEVICE_TYPE, sizeof(type), &type, nullptr);
        infoLog = std::string("Selected OpenCL device (") + deviceTypeLabel(type) + "): " + buf;
        return true;
      }
      ++idx;
    }
  }

  infoLog = "OpenCL device index out of range";
  return false;
}

} // namespace ocl
} // namespace cn

#endif
