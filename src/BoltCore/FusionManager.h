// FusionManager - optimization fusion transactions
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
  class TransactionBuilder;

  class FusionManager
  {
  public:
    FusionManager(const cn::Currency &currency, TransactionBuilder &txBuilder);

    // Estimate how many outputs are ready for fusion.
    // When mixin == 0 (anonymity 1), dust outputs are included as candidates.
    FusionEstimate estimate(uint64_t threshold, uint64_t mixin,
                            const std::vector<OutputInfo> &availableOutputs,
                            uint32_t height) const;

    // Create a fusion transaction to consolidate outputs
    TransferResult createFusion(uint64_t threshold, uint64_t mixin,
                                const std::vector<OutputInfo> &availableOutputs,
                                const cn::AccountPublicAddress &destination,
                                uint32_t height);

  private:
    const cn::Currency &m_currency;
    TransactionBuilder &m_txBuilder;
  };
}