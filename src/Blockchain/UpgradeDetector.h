// Copyright (c) 2011-2018, The CryptoNote developers, The Circle Foundation, Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <algorithm>
#include <cstdint>
#include "Common/StringTools.h"
#include "CryptoNoteCore/CryptoNoteBasicImpl.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteConfig.h"
#include <Logging/LoggerRef.h>

#undef ERROR

namespace cn
{

  class UpgradeDetectorBase
  {
  public:
    enum : uint32_t
    {
      UNDEF_HEIGHT = static_cast<uint32_t>(-1),
    };
  };

  static_assert(UpgradeDetectorBase::UNDEF_HEIGHT == UINT32_C(0xFFFFFFFF), "Invalid UNDEF_HEIGHT");

  class Blockchain;

  class BasicUpgradeDetector : public UpgradeDetectorBase
  {
  public:
    BasicUpgradeDetector(const Currency &currency, Blockchain *blockchain, uint8_t targetVersion, logging::ILogger &log);
    bool init();
    uint8_t targetVersion() const { return m_targetVersion; }
    uint32_t votingCompleteHeight() const { return m_votingCompleteHeight; }
    uint32_t upgradeHeight() const;
    void blockPushed();
    void blockPopped();
    size_t getNumberOfVotes(uint32_t height);

  private:
    uint32_t findVotingCompleteHeight(uint32_t probableUpgradeHeight);
    bool isVotingComplete(uint32_t height);

    logging::LoggerRef logger;
    const Currency &m_currency;
    Blockchain *m_blockchain;
    uint8_t m_targetVersion;
    uint32_t m_votingCompleteHeight;
  };

} // namespace cn