// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain/Blockchain.h"
#include "Blockchain/UpgradeDetector.h"

namespace cn {
  // BasicUpgradeDetector implementation
  BasicUpgradeDetector::BasicUpgradeDetector(const Currency &currency, Blockchain *blockchain,
                                             uint8_t targetVersion, logging::ILogger &log)
      : logger(log, "upgrade"),
        m_currency(currency),
        m_blockchain(blockchain),
        m_targetVersion(targetVersion),
        m_votingCompleteHeight(UNDEF_HEIGHT)
  {
  }

  // Initialises the upgrade detector by scanning the blockchain to determine the current voting state
  bool BasicUpgradeDetector::init()
  {
    uint32_t upgradeH = m_currency.upgradeHeight(m_targetVersion);
    if (upgradeH == UNDEF_HEIGHT)
    {
      if (m_blockchain->blocksEmpty())
      {
        m_votingCompleteHeight = UNDEF_HEIGHT;
      }
      else if (m_targetVersion - 1 == m_blockchain->blocksBack().bl.majorVersion)
      {
        m_votingCompleteHeight = findVotingCompleteHeight(
            static_cast<uint32_t>(m_blockchain->blocksSize() - 1));
      }
      else if (m_targetVersion <= m_blockchain->blocksBack().bl.majorVersion)
      {
        uint32_t found = 0;
        for (size_t i = 0; i < m_blockchain->blocksSize(); ++i)
        {
          if (m_blockchain->blocksAt(i).bl.majorVersion == m_targetVersion)
          {
            found = static_cast<uint32_t>(i);
            break;
          }
        }
        m_votingCompleteHeight = findVotingCompleteHeight(found);
        if (m_votingCompleteHeight == UNDEF_HEIGHT)
        {
          logger(logging::ERROR, logging::BRIGHT_RED)
              << "Internal error: voting complete height isn't found, upgrade height = " << found;
          return false;
        }
      }
      else
      {
        m_votingCompleteHeight = UNDEF_HEIGHT;
      }
    }
    else if (!m_blockchain->blocksEmpty())
    {
      if (m_blockchain->blocksSize() <= upgradeH + 1)
      {
        if (m_blockchain->blocksBack().bl.majorVersion >= m_targetVersion)
        {
          logger(logging::ERROR, logging::BRIGHT_RED)
              << "Internal error: block at height " << (m_blockchain->blocksSize() - 1)
              << " has invalid version " << static_cast<int>(m_blockchain->blocksBack().bl.majorVersion)
              << ", expected " << static_cast<int>(m_targetVersion - 1) << " or less";
          return false;
        }
      }
      else
      {
        int versionAfter = m_blockchain->blocksAt(upgradeH + 1).bl.majorVersion;
        if (versionAfter != m_targetVersion)
        {
          logger(logging::ERROR, logging::BRIGHT_RED)
              << "Internal error: block at height " << (upgradeH + 1)
              << " has invalid version " << versionAfter
              << ", expected " << static_cast<int>(m_targetVersion);
          return false;
        }
      }
    }
    return true;
  }

  // Returns the height at which the upgrade will activate (or UNDEF_HEIGHT if voting is incomplete)
  uint32_t BasicUpgradeDetector::upgradeHeight() const
  {
    if (m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT)
      return (m_votingCompleteHeight == UNDEF_HEIGHT)
                 ? UNDEF_HEIGHT
                 : m_currency.calculateUpgradeHeight(m_votingCompleteHeight);
    return m_currency.upgradeHeight(m_targetVersion);
  }

  // Called after a block is pushed — updates voting state and logs upgrade progress
  void BasicUpgradeDetector::blockPushed()
  {
    assert(!m_blockchain->blocksEmpty());

    if (m_currency.upgradeHeight(m_targetVersion) != UNDEF_HEIGHT)
    {
      if (m_blockchain->blocksSize() <= m_currency.upgradeHeight(m_targetVersion) + 1)
        assert(m_blockchain->blocksBack().bl.majorVersion <= m_targetVersion - 1);
      else
        assert(m_blockchain->blocksBack().bl.majorVersion >= m_targetVersion);
    }
    else if (m_votingCompleteHeight != UNDEF_HEIGHT)
    {
      assert(m_blockchain->blocksSize() > m_votingCompleteHeight);
      if (m_blockchain->blocksSize() <= upgradeHeight())
      {
        assert(m_blockchain->blocksBack().bl.majorVersion == m_targetVersion - 1);
        if (m_blockchain->blocksSize() % (60 * 60 / m_currency.difficultyTarget()) == 0)
        {
          auto interval = m_currency.difficultyTarget() *
                          (upgradeHeight() - m_blockchain->blocksSize() + 2);
          time_t upgradeTimestamp = time(nullptr) + static_cast<time_t>(interval);
          std::string upgradeTime = common::formatTimestamp(upgradeTimestamp);
          logger(logging::TRACE, logging::BRIGHT_GREEN)
              << "###### UPGRADE is going to happen after block index "
              << upgradeHeight() << " at about " << upgradeTime
              << " (in " << common::timeIntervalToString(interval)
              << ")! Current last block index "
              << (m_blockchain->blocksSize() - 1)
              << ", hash " << get_block_hash(m_blockchain->blocksBack().bl);
        }
      }
      else if (m_blockchain->blocksSize() == upgradeHeight() + 1)
      {
        assert(m_blockchain->blocksBack().bl.majorVersion == m_targetVersion - 1);
        logger(logging::TRACE, logging::BRIGHT_GREEN)
            << "###### UPGRADE has happened! Starting from block index "
            << (upgradeHeight() + 1)
            << " blocks with major version below " << static_cast<int>(m_targetVersion)
            << " will be rejected!";
      }
      else
      {
        assert(m_blockchain->blocksBack().bl.majorVersion == m_targetVersion);
      }
    }
    else
    {
      uint32_t lastBlockHeight = static_cast<uint32_t>(m_blockchain->blocksSize() - 1);
      if (isVotingComplete(lastBlockHeight))
      {
        m_votingCompleteHeight = lastBlockHeight;
        logger(logging::TRACE, logging::BRIGHT_GREEN)
            << "###### UPGRADE voting complete at block index "
            << m_votingCompleteHeight
            << "! UPGRADE is going to happen after block index "
            << upgradeHeight() << "!";
      }
    }
  }

  // Called after a block is popped — may cancel a pending upgrade if voting is no longer complete
  void BasicUpgradeDetector::blockPopped()
  {
    if (m_votingCompleteHeight != UNDEF_HEIGHT)
    {
      assert(m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT);
      if (m_blockchain->blocksSize() == m_votingCompleteHeight)
      {
        logger(logging::TRACE, logging::BRIGHT_YELLOW)
            << "###### UPGRADE after block index " << upgradeHeight()
            << " has been canceled!";
        m_votingCompleteHeight = UNDEF_HEIGHT;
      }
      else
      {
        assert(m_blockchain->blocksSize() > m_votingCompleteHeight);
      }
    }
  }

  // Counts how many blocks in the voting window have voted for this upgrade
  size_t BasicUpgradeDetector::getNumberOfVotes(uint32_t height)
  {
    if (height < m_currency.upgradeVotingWindow() - 1)
      return 0;
    size_t voteCounter = 0;
    for (size_t i = height + 1 - m_currency.upgradeVotingWindow(); i <= height; ++i)
    {
      const auto &b = m_blockchain->blocksAt(i).bl;
      voteCounter += (b.majorVersion == m_targetVersion - 1 && b.minorVersion == BLOCK_MINOR_VERSION_1) ? 1 : 0;
    }
    return voteCounter;
  }

  // Scans backwards from a probable upgrade height to find where voting first became complete
  uint32_t BasicUpgradeDetector::findVotingCompleteHeight(uint32_t probableUpgradeHeight)
  {
    assert(m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT);
    uint32_t start = (probableUpgradeHeight > m_currency.maxUpgradeDistance())
                         ? probableUpgradeHeight - m_currency.maxUpgradeDistance()
                         : 0;
    for (size_t i = start; i <= probableUpgradeHeight; ++i)
    {
      if (isVotingComplete(static_cast<uint32_t>(i)))
        return static_cast<uint32_t>(i);
    }
    return UNDEF_HEIGHT;
  }

  // Checks whether the voting threshold has been met at the given height
  bool BasicUpgradeDetector::isVotingComplete(uint32_t height)
  {
    assert(m_currency.upgradeHeight(m_targetVersion) == UNDEF_HEIGHT);
    assert(m_currency.upgradeVotingWindow() > 1);
    assert(m_currency.upgradeVotingThreshold() > 0 && m_currency.upgradeVotingThreshold() <= 100);
    size_t voteCounter = getNumberOfVotes(height);
    return (size_t)m_currency.upgradeVotingThreshold() * m_currency.upgradeVotingWindow() <= 100 * voteCounter;
  }
}