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
  class OutputSelector;
  class TransactionBuilder;
  class SignatureBuilder;
  class RelayHandler;

  class FusionManager
  {
  public:
    FusionManager(const cn::Currency &currency,
                  OutputSelector &outputSelector,
                  TransactionBuilder &txBuilder,
                  SignatureBuilder &sigBuilder,
                  RelayHandler &relay);

    // Estimate how many outputs are ready for fusion
    FusionEstimate estimate(uint64_t threshold,
                            const std::vector<OutputInfo> &availableOutputs) const;

    // Create a fusion transaction to consolidate outputs
    TransferResult createFusion(uint64_t threshold, uint64_t mixin,
                                const std::vector<OutputInfo> &availableOutputs,
                                const cn::AccountPublicAddress &destination);

  private:
    const cn::Currency &m_currency;
    OutputSelector &m_outputSelector;
    TransactionBuilder &m_txBuilder;
    SignatureBuilder &m_sigBuilder;
    RelayHandler &m_relay;
  };
}