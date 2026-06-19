// DepositManager - create and withdraw time-locked deposits
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "crypto/crypto.h"
#include <vector>

namespace cn
{
  class Currency;
}

namespace BoltCore
{
  class TransactionBuilder;
  class SignatureBuilder;
  class OutputSelector;

  class DepositManager
  {
  public:
    DepositManager(const cn::Currency &currency,
                   TransactionBuilder &txBuilder,
                   SignatureBuilder &sigBuilder,
                   OutputSelector &outputSelector,
                   const cn::AccountPublicAddress &mainAddress);

    TransferResult createDeposit(uint64_t amount, uint32_t term,
                                 const std::vector<OutputInfo> &fundingOutputs);

    TransferResult withdrawDeposit(const OutputInfo &depositOutput,
                                   const DepositInfo &deposit);

    uint64_t calculateInterest(uint64_t amount, uint32_t term, uint32_t height) const;

  private:
    const cn::Currency &m_currency;
    TransactionBuilder &m_txBuilder;
    SignatureBuilder &m_sigBuilder;
    OutputSelector &m_outputSelector;
    cn::AccountPublicAddress m_mainAddress;
  };
}
