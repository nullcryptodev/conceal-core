// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// MIT

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "../crypto/hash.h"
#include "backend.hpp"

namespace cn
{

/** Height-keyed GPU PoW results for non-blocking sync verification. */
class GpuPowResultCache
{
public:
  void store(uint32_t height, const crypto::Hash& blockId, const crypto::Hash& proofOfWork, bool ok);

  /** Non-blocking: returns true when a ready result matches @a height and @a blockId. */
  bool tryConsume(uint32_t height, const crypto::Hash& blockId, crypto::Hash& out);

  bool hasReady(uint32_t height, const crypto::Hash& blockId) const;

  void clear();

  void pruneBelow(uint32_t minHeightKept);

private:
  struct Entry
  {
    crypto::Hash blockId;
    crypto::Hash proofOfWork;
    bool ready = false;
    bool ok = false;
  };

  mutable std::mutex m_mutex;
  std::unordered_map<uint32_t, Entry> m_byHeight;
};

} // namespace cn
