// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#include "pow_sync_log.hpp"

#include <iostream>

#include "../Common/StringTools.h"
#include "../crypto/pow_hash/hw_detect.hpp"

namespace cn
{

namespace
{
logging::LoggerRef* g_syncLogger = nullptr;
} // namespace

void setPowSyncLogger(logging::LoggerRef* logger) { g_syncLogger = logger; }

bool powSyncLogEnabled(uint32_t blockHeight)
{
  return blockHeight != UINT32_MAX && blockHeight > POW_SYNC_LOG_MIN_HEIGHT;
}

std::string powHashShort(const crypto::Hash& h)
{
  const std::string hex = common::podToHex(h);
  return hex.size() > 8 ? hex.substr(0, 8) : hex;
}

void powSyncLogInfo(const std::string& message)
{
  if (g_syncLogger)
    (*g_syncLogger)(logging::INFO) << message;
  else
    std::cerr << "[pow] " << message << std::endl;
}

void powSyncLogDebug(const std::string& message)
{
  if (g_syncLogger)
    (*g_syncLogger)(logging::DEBUGGING) << message;
  else
    std::cerr << "[pow] " << message << std::endl;
}

const char* powCpuInnerName()
{
#if defined(HAS_INTEL_HW)
  return check_avx2() ? "inner_hash_3_avx" : "inner_hash_3";
#elif defined(HAS_ARM_HW)
  return "inner_hash_3";
#else
  return "inner_hash_3";
#endif
}

} // namespace cn
