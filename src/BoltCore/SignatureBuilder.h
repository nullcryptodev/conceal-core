// SignatureBuilder - key images, ring signatures, and input signing
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "crypto/crypto.h"
#include <vector>

namespace cn
{
  struct KeyInput;
}

namespace BoltCore
{
  struct SigningInput
  {
    OutputInfo output;
    std::vector<crypto::PublicKey> ringMembers; // real output + mixins
    size_t realOutputIndex;                     // position in ringMembers
  };

  class SignatureBuilder
  {
  public:
    SignatureBuilder(const crypto::SecretKey &viewSecretKey,
                     const crypto::SecretKey &spendSecretKey,
                     const crypto::PublicKey &viewPublicKey,
                     const crypto::PublicKey &spendPublicKey);

    // Generate key image for a single output
    crypto::KeyImage generateKeyImage(const crypto::PublicKey &outputKey,
                                      const crypto::SecretKey &ephemeralSecretKey) const;

    // Derive the ephemeral secret key for an output we own
    crypto::SecretKey deriveEphemeralSecretKey(const crypto::PublicKey &txPublicKey,
                                               size_t outputIndex) const;

    // Sign a set of inputs for a transaction
    // Returns the ring signature for each input
    struct SignedInput
    {
      crypto::KeyImage keyImage;
      std::vector<crypto::Signature> signatures; // one per ring member
    };

    SignedInput signInput(const crypto::Hash &txPrefixHash,
                          const SigningInput &input) const;

    // Sign a multisignature input (for deposits)
    crypto::Signature signMultisigInput(const crypto::Hash &txPrefixHash,
                                        const crypto::PublicKey &txPublicKey,
                                        size_t outputIndex) const;

  private:
    crypto::SecretKey m_viewSecretKey;
    crypto::SecretKey m_spendSecretKey;
    crypto::PublicKey m_viewPublicKey;
    crypto::PublicKey m_spendPublicKey;
  };
}