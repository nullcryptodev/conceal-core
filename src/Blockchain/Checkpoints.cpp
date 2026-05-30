// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2016-2019, The Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain/Checkpoints.h"
#include "Common/DnsTools.h"
#include "Common/StringTools.h"
#include "CryptoNoteConfig.h"
#include "CheckpointsList.h"

#include <fstream>
#include <sstream>

namespace cn
{
  //  Construction
  Checkpoints::Checkpoints(logging::ILogger &log)
      : logger(log, "checkpoints") {}

  //  Checkpoint management
  bool Checkpoints::add_checkpoint(uint32_t height, const std::string &hash_str)
  {
    crypto::Hash h = NULL_HASH;

    if (!common::podFromHex(hash_str, h))
    {
      logger(logging::ERROR) << "Incorrect hash in checkpoints";
      return false;
    }

    if (m_points.count(height) != 0)
    {
      logger(logging::DEBUGGING) << "Checkpoint already exists for height " << height;
      return false;
    }

    m_points[height] = h;
    return true;
  }

  // Validation
  bool Checkpoints::is_in_checkpoint_zone(uint32_t height) const
  {
    return !m_points.empty() && height <= m_points.rbegin()->first;
  }

  bool Checkpoints::check_block(uint32_t height, const crypto::Hash &h) const
  {
    bool ignored;
    return check_block(height, h, ignored);
  }

  bool Checkpoints::check_block(uint32_t height, const crypto::Hash &h,
                                bool &is_a_checkpoint) const
  {
    auto it = m_points.find(height);
    is_a_checkpoint = (it != m_points.end());

    if (!is_a_checkpoint)
      return true;

    if (it->second == h)
      return true;

    logger(logging::ERROR) << "Checkpoint failed for height " << height
                           << ". Expected: " << common::podToHex(it->second)
                           << ", Got: " << common::podToHex(h);
    return false;
  }

  bool Checkpoints::is_alternative_block_allowed(uint32_t blockchain_height,
                                                 uint32_t block_height) const
  {
    if (block_height == 0)
      return false;

    uint32_t lowest_height =
        blockchain_height > cn::parameters::CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW
            ? blockchain_height - cn::parameters::CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW
            : 0;

    if (block_height < lowest_height && !is_in_checkpoint_zone(block_height))
    {
      logger(logging::DEBUGGING, logging::WHITE)
          << "Reorganization depth too deep: "
          << (blockchain_height - block_height) << ". Block rejected";
      return false;
    }

    auto it = m_points.upper_bound(blockchain_height);
    if (it == m_points.begin())
      return true;

    --it;
    return it->first < block_height;
  }

  // Queries
  std::vector<uint32_t> Checkpoints::getCheckpointHeights() const
  {
    std::vector<uint32_t> heights;
    heights.reserve(m_points.size());
    for (const auto &cp : m_points)
      heights.push_back(cp.first);
    return heights;
  }

  uint32_t Checkpoints::getMaxHeight() const
  {
    return m_points.empty() ? 0 : m_points.rbegin()->first;
  }

  CheckpointList Checkpoints::getCheckpointList(uint32_t startHeight,
                                                uint32_t endHeight) const
  {
    CheckpointList result;
    for (const auto &cp : m_points)
    {
      uint32_t height = cp.first;
      const crypto::Hash &hash = cp.second;
      if (height >= startHeight && (endHeight == 0 || height <= endHeight))
        result.addCheckpoint(height, hash);
    }
    return result;
  }

  // Loading
  bool Checkpoints::load_checkpoints()
  {
    const auto &checkpoints = m_testnet ? cn::TESTNET_CHECKPOINTS
                                        : cn::CHECKPOINTS;
    for (const auto &cp : checkpoints)
      add_checkpoint(cp.height, cp.blockId);
    return true;
  }

  bool Checkpoints::load_checkpoints_from_dns()
  {
    std::string domain = m_testnet ? "testpoints.conceal.gq"
                                   : "checkpoints.conceal.id";

    std::vector<std::string> records;
    logger(logging::DEBUGGING) << "Fetching DNS checkpoint records from " << domain;

    if (!common::fetch_dns_txt(domain, records))
    {
      logger(logging::DEBUGGING) << "Failed to lookup DNS checkpoint records from "
                                 << domain;
    }

    for (const auto &record : records)
    {
      size_t del = record.find_first_of(':');
      if (del == std::string::npos)
        continue;

      std::string height_str = record.substr(0, del);
      std::string hash_str = record.substr(del + 1, 64);

      uint32_t height;
      crypto::Hash hash = NULL_HASH;
      std::stringstream ss(height_str);
      ss >> height;
      char c;
      if (ss.fail() || ss.get(c) || !common::podFromHex(hash_str, hash))
      {
        logger(logging::INFO) << "Failed to parse DNS checkpoint record: " << record;
        continue;
      }

      if (m_points.count(height) != 0)
      {
        logger(logging::DEBUGGING) << "Checkpoint already exists for height: "
                                   << height << ". Ignoring DNS checkpoint.";
      }
      else
      {
        add_checkpoint(height, hash_str);
        logger(logging::DEBUGGING) << "Added DNS checkpoint: " << height_str
                                   << ":" << hash_str;
      }
    }

    return true;
  }

  bool Checkpoints::load_checkpoints_from_file(const std::string &fileName)
  {
    std::ifstream file(fileName);
    if (!file)
    {
      logger(logging::ERROR, logging::BRIGHT_RED)
          << "Could not load checkpoints file: " << fileName;
      return false;
    }

    std::string indexString, hash;
    uint32_t height;
    while (std::getline(file, indexString, ',') && std::getline(file, hash))
    {
      try
      {
        height = std::stoi(indexString);
      }
      catch (const std::invalid_argument &)
      {
        logger(logging::ERROR, logging::BRIGHT_RED)
            << "Invalid checkpoint file format - could not parse height as a number";
        return false;
      }

      if (!add_checkpoint(height, hash))
        return false;
    }

    logger(logging::INFO) << "Loaded " << m_points.size()
                          << " checkpoints from " << fileName;
    return true;
  }

  bool Checkpoints::load_self_generated_checkpoints()
  {
    // Self-generated checkpoints are loaded by Blockchain during
    // initialisation via the MDBX storage layer and fed into m_points
    // through add_checkpoint(). This method exists so the loading
    // sequence can be expressed uniformly at the call site.
    return true;
  }

  // Saving
  bool Checkpoints::save_checkpoints_to_file(const std::string &fileName) const
  {
    std::ofstream file(fileName);
    if (!file.is_open())
    {
      logger(logging::ERROR, logging::BRIGHT_RED)
          << "Failed to open checkpoints file for writing: " << fileName;
      return false;
    }

    size_t count = 0;
    for (const auto &cp : m_points)
    {
      uint32_t height = cp.first;
      file << height << "," << common::podToHex(cp.second) << "\n";
      ++count;
    }

    logger(logging::INFO, logging::BRIGHT_GREEN)
        << "Exported " << count << " checkpoints to " << fileName;
    return true;
  }

  // Configuration
  void Checkpoints::set_testnet(bool testnet)
  {
    m_testnet = testnet;
  }

} // namespace cn