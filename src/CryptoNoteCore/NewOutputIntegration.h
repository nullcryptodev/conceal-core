// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "CryptoNoteBasic.h"
#include "NewOutputTypes.h"

namespace cn
{

  //
  // Extended output target variant that includes both legacy and new types.
  // This is used for post-fork serialization and transaction building.
  //

  typedef boost::variant<
      KeyOutput,
      MultisignatureOutput,
      StandardPaymentOutput,
      MultisigPaymentOutput,
      DomainRegistrationOutput,
      DomainDeletionOutput>
      ExtendedOutputTarget;

  //
  // Fork activation height for new output types.
  // TODO: Set to actual target height before deployment.
  //

  const uint64_t NEW_OUTPUT_FORK_HEIGHT = 999999999;   // Placeholder - set before deployment
  const uint64_t TESTNET_NEW_OUTPUT_FORK_HEIGHT = 100; // Early testnet activation

  inline uint64_t getNewOutputForkHeight(bool isTestnet)
  {
    return isTestnet ? TESTNET_NEW_OUTPUT_FORK_HEIGHT : NEW_OUTPUT_FORK_HEIGHT;
  }

  const uint8_t TRANSACTION_VERSION_3 = 3; // New output types

} // namespace cn