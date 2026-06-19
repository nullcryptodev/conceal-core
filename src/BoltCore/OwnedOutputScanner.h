// OwnedOutputScanner - scan transactions for wallet-owned outputs (mempool + blocks)
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "crypto/crypto.h"

#include <vector>

namespace BoltCore
{
  // blockHeight == 0 marks mempool / unconfirmed outputs (WalletGreen WALLET_UNCONFIRMED height).
  std::vector<OutputInfo> scanOwnedOutputsInTransaction(
      const cn::Transaction &tx,
      uint32_t blockHeight,
      const crypto::SecretKey &viewKey,
      const crypto::PublicKey &spendPub,
      const crypto::SecretKey *spendKey);
}
