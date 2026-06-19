// OutputUtils - spendability helpers for BoltCore UTXOs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"

namespace BoltCore
{
  inline bool isDepositUnlocked(const OutputInfo &out, uint32_t currentHeight)
  {
    return out.term > 0 && (currentHeight + 1 >= out.blockHeight + out.term);
  }

  // Index for generate_key_image_helper when signing a key input.
  // WalletGreen: legacy KeyOutput uses tx output index (TransfersConsumer idx).
  // V3 StandardPaymentOutput: on-chain key_index (NewOutputScanner).
  inline uint32_t outputSigningIndex(const OutputInfo &out)
  {
    return out.hasKeyDerivationIndex ? out.keyDerivationIndex : out.outputIndex;
  }

  // Mirrors WalletGreen IncludeKeyUnlocked: regular key outputs only, not deposits.
  inline bool isSpendableKeyOutput(const OutputInfo &out, uint32_t currentHeight,
                                   uint32_t unlockWindow = 0)
  {
    if (out.spent || out.isDeposit)
      return false;
    if (unlockWindow > 0 && currentHeight < out.blockHeight + unlockWindow)
      return false;
    return true;
  }

  // Spendable key UTXOs eligible to fund sends and deposit creation.
  inline bool isFundingKeyOutput(const OutputInfo &out, uint32_t currentHeight,
                                 uint32_t unlockWindow, uint64_t dustThreshold)
  {
    if (!isSpendableKeyOutput(out, currentHeight, unlockWindow))
      return false;
    return out.amount > dustThreshold;
  }
}
