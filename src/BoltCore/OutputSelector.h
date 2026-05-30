// OutputSelector - picks unspent outputs for transfers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include <vector>

namespace cn
{
  class Currency;
}

namespace BoltCore
{
  class OutputSelector
  {
  public:
    OutputSelector(const cn::Currency &currency);

    // Select outputs to fund a transfer of neededAmount
    // Returns selected outputs and total amount found
    // Throws if insufficient funds
    struct Selection
    {
      std::vector<OutputInfo> outputs;
      uint64_t totalFound;
      bool enough;
    };

    Selection select(uint64_t neededAmount, const std::vector<OutputInfo> &available) const;

  private:
    const cn::Currency &m_currency;
  };
}