// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <vector>
#include <cstdint>
#include "crypto/hash.h"
#include "Serialization/ISerializer.h"
#include "Serialization/SerializationOverloads.h"

namespace cn
{
  struct CheckpointEntry
  {
    uint32_t height;
    crypto::Hash blockHash;

    void serialize(ISerializer &s)
    {
      s(height, "height");
      s.binary(blockHash.data, sizeof(blockHash.data), "blockHash");
    }
  };

  struct CheckpointList
  {
    std::vector<CheckpointEntry> checkpoints;

    void serialize(ISerializer &s)
    {
      s(checkpoints, "checkpoints");
    }

    bool empty() const { return checkpoints.empty(); }
    size_t size() const { return checkpoints.size(); }
    void clear() { checkpoints.clear(); }

    void addCheckpoint(uint32_t height, const crypto::Hash &hash)
    {
      CheckpointEntry entry;
      entry.height = height;
      entry.blockHash = hash;
      checkpoints.push_back(entry);
    }
  };
}