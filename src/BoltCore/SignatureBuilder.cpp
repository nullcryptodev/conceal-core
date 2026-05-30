// SignatureBuilder implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SignatureBuilder.h"
#include "CryptoNoteCore/CryptoNoteTools.h"

namespace BoltCore
{
  SignatureBuilder::SignatureBuilder(const crypto::SecretKey &viewSecretKey,
                                     const crypto::SecretKey &spendSecretKey,
                                     const crypto::PublicKey &viewPublicKey,
                                     const crypto::PublicKey &spendPublicKey)
      : m_viewSecretKey(viewSecretKey), m_spendSecretKey(spendSecretKey),
        m_viewPublicKey(viewPublicKey), m_spendPublicKey(spendPublicKey) {}

  crypto::KeyImage SignatureBuilder::generateKeyImage(
      const crypto::PublicKey &outputKey,
      const crypto::SecretKey &ephemeralSecretKey) const
  {
    crypto::KeyImage image;
    crypto::generate_key_image(outputKey, ephemeralSecretKey, image);
    return image;
  }

  crypto::SecretKey SignatureBuilder::deriveEphemeralSecretKey(
      const crypto::PublicKey &txPublicKey,
      size_t outputIndex) const
  {
    crypto::KeyDerivation derivation;
    crypto::generate_key_derivation(txPublicKey, m_viewSecretKey, derivation);

    crypto::SecretKey ephemeralSecretKey;
    crypto::derive_secret_key(derivation, outputIndex, m_spendSecretKey, ephemeralSecretKey);

    return ephemeralSecretKey;
  }

  SignatureBuilder::SignedInput SignatureBuilder::signInput(
      const crypto::Hash &txPrefixHash,
      const SigningInput &input) const
  {
    SignedInput result;

    // Derive ephemeral secret key for our output
    crypto::SecretKey ephemeralSecretKey = deriveEphemeralSecretKey(
        input.output.txPublicKey, input.output.outputIndex);

    // Generate key image
    result.keyImage = generateKeyImage(input.output.outputKey, ephemeralSecretKey);

    // Build ring signature
    std::vector<const crypto::PublicKey *> ringPtrs;
    for (const auto &member : input.ringMembers)
    {
      ringPtrs.push_back(&member);
    }

    result.signatures.resize(input.ringMembers.size());
    crypto::generate_ring_signature(
        txPrefixHash, result.keyImage,
        ringPtrs.data(), ringPtrs.size(),
        ephemeralSecretKey, input.realOutputIndex,
        result.signatures.data());

    return result;
  }

  crypto::Signature SignatureBuilder::signMultisigInput(
      const crypto::Hash &txPrefixHash,
      const crypto::PublicKey &txPublicKey,
      size_t outputIndex) const
  {
    crypto::KeyDerivation derivation;
    crypto::generate_key_derivation(txPublicKey, m_viewSecretKey, derivation);

    crypto::SecretKey ephemeralSecretKey;
    crypto::derive_secret_key(derivation, outputIndex, m_spendSecretKey, ephemeralSecretKey);

    crypto::PublicKey ephemeralPublicKey;
    crypto::secret_key_to_public_key(ephemeralSecretKey, ephemeralPublicKey);

    crypto::Signature sig;
    crypto::generate_signature(txPrefixHash, ephemeralPublicKey, ephemeralSecretKey, sig);

    return sig;
  }
}