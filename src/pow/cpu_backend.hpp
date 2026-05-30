// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license.

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "backend.hpp"

namespace cn
{

class CpuPowBackend : public PowVerifyBackend
{
public:
  bool available() const override { return true; }

  bool computeLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                       crypto::Hash& out) override;

  bool submitLonghash(crypto::cn_context& ctx, const void* data, size_t length,
                      const crypto::Hash& blockId, uint32_t blockHeight) override;

  bool tryConsumeLonghash(uint32_t blockHeight, const crypto::Hash& blockId,
                          crypto::Hash& out) override;

  bool tryGetLonghash(const crypto::Hash& blockId, crypto::Hash& out) override;

  bool awaitLonghash(const crypto::Hash& blockId, crypto::Hash& out) override;

  bool verifyPow(crypto::cn_context& ctx, const void* data, size_t length,
                 difficulty_type difficulty, crypto::Hash& proofOfWork) override;

  const PowVerifyMetrics& metrics() const override { return m_metrics; }

  size_t pendingJobCount() override;

private:
  struct HeightEntry
  {
    crypto::Hash blockId;
    crypto::Hash pow;
  };

  PowVerifyMetrics m_metrics;
  std::mutex m_mutex;
  std::unordered_map<crypto::Hash, crypto::Hash, HashHasher> m_completed;
  std::unordered_map<uint32_t, HeightEntry> m_byHeight;
};

} // namespace cn
