// TransactionBuilder - builds complete unsigned transactions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "crypto/crypto.h"
#include <memory>
#include <vector>

namespace cn
{
  class ITransaction;
  class Currency;
  struct AccountPublicAddress;
}

namespace BoltCore
{
  class OutputSelector;
  class SignatureBuilder;

  struct BuilderParams
  {
    std::vector<Transfer> transfers;
    uint64_t fee;
    uint64_t mixin;
    std::string extra;
    uint64_t unlockTime;
    uint64_t ttl;
    cn::AccountPublicAddress changeAddress;
    cn::AccountPublicAddress donationAddress;
    uint64_t donationThreshold;
    cn::AccountPublicAddress mainAddress;
  };

  class TransactionBuilder
  {
  public:
    TransactionBuilder(const cn::Currency &currency,
                       OutputSelector &outputSelector,
                       SignatureBuilder &signatureBuilder);

    struct BuildResult
    {
      std::unique_ptr<cn::ITransaction> transaction;
      crypto::SecretKey transactionSecretKey;
      std::vector<OutputInfo> selectedOutputs;
      std::vector<cn::AccountPublicAddress> destAddresses;
      uint64_t changeAmount;
      uint64_t fee;
      bool success;
      std::string error;
    };

    BuildResult build(const std::vector<OutputInfo> &availableOutputs,
                      const BuilderParams &params);

  private:
    const cn::Currency &m_currency;
    OutputSelector &m_outputSelector;
    SignatureBuilder &m_signatureBuilder;
  };
}