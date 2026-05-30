// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <cstdint>
#include <sstream>
#include <string>

#include "../Logging/ILogger.h"
#include "../Logging/LoggerRef.h"
#include "../crypto/hash.h"

namespace cn
{

/** Mainnet checkpoint zone ends at this height; CN-GPU PoW logging applies above it. */
constexpr uint32_t POW_SYNC_LOG_MIN_HEIGHT = 2070000u;

void setPowSyncLogger(logging::LoggerRef* logger);

bool powSyncLogEnabled(uint32_t blockHeight);

std::string powHashShort(const crypto::Hash& h);

void powSyncLogInfo(const std::string& message);

void powSyncLogDebug(const std::string& message);

/** Production CPU inner loop name (AVX2 when available). */
const char* powCpuInnerName();

} // namespace cn
