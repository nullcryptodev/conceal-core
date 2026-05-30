// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <vector>

#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "CryptoNoteCore/NewOutputTypes.h"
#include "BoltSync/BoltSync.h"

namespace BoltCore
{

  //
  // NewOutputScanner handles post-fork blocks (height >= forkHeight) where
  // outputs are self-describing with on-chain view tags and key indices.
  // No filter database is needed — the view tag is read directly from the output.
  //
  class NewOutputScanner
  {
  public:
    //
    // Scan a single post-fork transaction for owned outputs.
    //
    // @param tx              The transaction to scan
    // @param txPubKey        Transaction public key from extra
    // @param globalIndexes   Global output indexes for each output in tx
    // @param blockHeight     Height of the block containing this tx
    // @param viewSecretKey   Wallet's view secret key
    // @param spendPublicKey  Wallet's spend public key
    // @param spendSecretKey  Optional spend secret key (for key image generation)
    // @param results         Output vector to append found outputs to
    //
    static void scanTransaction(
        const cn::Transaction &tx,
        const crypto::PublicKey &txPubKey,
        const std::vector<uint32_t> &globalIndexes,
        uint32_t blockHeight,
        const crypto::SecretKey &viewSecretKey,
        const crypto::PublicKey &spendPublicKey,
        const crypto::SecretKey *spendSecretKey,
        std::vector<BoltSync::FoundOutput> &results);

    //
    // Check if any output in a transaction uses the new output types (0x04-0x07)
    //
    static bool hasNewOutputs(const cn::Transaction &tx);

  private:
    //
    // Try to recognize a StandardPaymentOutput (0x04)
    //
    static bool tryScanStandardOutput(
        const cn::StandardPaymentOutput &out,
        uint64_t amount,
        uint32_t outputIndex,
        uint32_t globalIndex,
        uint32_t blockHeight,
        const crypto::PublicKey &txPubKey,
        const crypto::SecretKey &viewSecretKey,
        const crypto::PublicKey &spendPublicKey,
        const crypto::SecretKey *spendSecretKey,
        std::vector<BoltSync::FoundOutput> &results);

    //
    // Try to recognize a MultisigPaymentOutput (0x05)
    //
    static bool tryScanMultisigOutput(
        const cn::MultisigPaymentOutput &out,
        uint64_t amount,
        uint32_t outputIndex,
        uint32_t globalIndex,
        uint32_t blockHeight,
        const crypto::PublicKey &txPubKey,
        const crypto::SecretKey &viewSecretKey,
        const crypto::PublicKey &spendPublicKey,
        std::vector<BoltSync::FoundOutput> &results);
  };

} // namespace BoltCore