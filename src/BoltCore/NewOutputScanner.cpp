// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "NewOutputScanner.h"

#include <cstring>

#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/EncryptedMemo.h"
#include "BoltSync/CryptoHelpers.h"

namespace BoltCore
{

  bool NewOutputScanner::hasNewOutputs(const cn::Transaction &tx)
  {
    for (size_t i = 0; i < tx.outputs.size(); ++i)
    {
      const cn::TransactionOutputTarget &target = tx.outputs[i].target;
      if (target.type() == typeid(cn::StandardPaymentOutput) ||
          target.type() == typeid(cn::MultisigPaymentOutput) ||
          target.type() == typeid(cn::DomainRegistrationOutput) ||
          target.type() == typeid(cn::DomainDeletionOutput))
      {
        return true;
      }
    }
    return false;
  }

  void NewOutputScanner::scanTransaction(
      const cn::Transaction &tx,
      const crypto::PublicKey &txPubKey,
      const std::vector<uint32_t> &globalIndexes,
      uint32_t blockHeight,
      const crypto::SecretKey &viewSecretKey,
      const crypto::PublicKey &spendPublicKey,
      const crypto::SecretKey *spendSecretKey,
      std::vector<BoltSync::FoundOutput> &results)
  {
    // Compute derivation once per transaction
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, viewSecretKey, derivation))
      return;

    crypto::Hash txHash;
    if (!BoltSync::getTxHash(tx, txHash))
      return;

    for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
    {
      const cn::TransactionOutput &output = tx.outputs[outIdx];
      uint32_t globalIndex = (outIdx < globalIndexes.size()) ? globalIndexes[outIdx] : 0;
      const bool hasGlobalIndex = outIdx < globalIndexes.size();

      if (output.target.type() == typeid(cn::StandardPaymentOutput))
      {
        const cn::StandardPaymentOutput &stdOut = boost::get<cn::StandardPaymentOutput>(output.target);
        tryScanStandardOutput(stdOut, output.amount, static_cast<uint32_t>(outIdx),
                              globalIndex, hasGlobalIndex, blockHeight, txHash, txPubKey, viewSecretKey,
                              spendPublicKey, spendSecretKey, results);
      }
      else if (output.target.type() == typeid(cn::MultisigPaymentOutput))
      {
        const cn::MultisigPaymentOutput &msigOut = boost::get<cn::MultisigPaymentOutput>(output.target);
        tryScanMultisigOutput(msigOut, output.amount, static_cast<uint32_t>(outIdx),
                              globalIndex, hasGlobalIndex, blockHeight, txHash, txPubKey, viewSecretKey,
                              spendPublicKey, results);
      }
      // DomainRegistrationOutput (0x06) and DomainDeletionOutput (0x07)
      // are not spendable outputs — skip them entirely.
    }
  }

  bool NewOutputScanner::tryScanStandardOutput(
      const cn::StandardPaymentOutput &out,
      uint64_t amount,
      uint32_t outputIndex,
      uint32_t globalIndex,
      bool hasGlobalOutputIndex,
      uint32_t blockHeight,
      const crypto::Hash &txHash,
      const crypto::PublicKey &txPubKey,
      const crypto::SecretKey &viewSecretKey,
      const crypto::PublicKey &spendPublicKey,
      const crypto::SecretKey *spendSecretKey,
      std::vector<BoltSync::FoundOutput> &results)
  {
    // Step 1: View tag filter (on-chain, zero crypto cost)
    // Compute derivation first
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, viewSecretKey, derivation))
      return false;

    // Compute expected view tag using the derivation
    uint8_t expectedTag = BoltSync::computeWalletViewTag(derivation, outputIndex);
    if (expectedTag != out.view_tag)
      return false; // Not ours — 255/256 false outputs eliminated here

    // Step 2: Key derivation check using key_index from output
    // The key_index field tells us exactly which derived key to check.
    // No more global key counter tracking needed.
    crypto::PublicKey derivedKey;
    if (!crypto::derive_public_key(derivation, out.key_index, spendPublicKey, derivedKey))
      return false;

    if (derivedKey != out.key)
      return false; // Not ours (false positive from view tag — 1/256 chance)

    // Step 3: Output is confirmed ours — build result
    BoltSync::FoundOutput fo;
    fo.blockHeight = blockHeight;
    fo.txHash = txHash;
    fo.outputIndex = outputIndex;
    fo.amount = amount;
    fo.outputKey = out.key;
    fo.txPublicKey = txPubKey;
    fo.txExtra = std::vector<uint8_t>(); // Can be populated from tx.extra if needed
    fo.isDeposit = false;
    fo.term = 0;
    fo.keyDerivationIndex = out.key_index;
    fo.hasKeyDerivationIndex = true;
    if (hasGlobalOutputIndex)
    {
      fo.globalOutputIndex = globalIndex;
      fo.hasGlobalOutputIndex = true;
    }

    // Generate key image if spend key is available
    if (spendSecretKey)
    {
      crypto::SecretKey outputSecret = BoltSync::deriveOutputSecretKey(
          derivation, out.key_index, *spendSecretKey);
      crypto::generate_key_image(out.key, outputSecret, fo.keyImage);
    }

    // Step 4: Decrypt memo if present
    // The decrypted memo can be processed by the caller.
    // We decrypt here but don't store in FoundOutput yet (FoundOutput
    // would need a new field for decrypted memo data).
    if (out.hasMemo())
    {
      std::vector<uint8_t> decryptedMemo = cn::EncryptedMemo::decrypt(
          out.encrypted_memo, derivation, globalIndex);
      // TODO: Extend FoundOutput with a memo field, or pass decrypted memo
      // to the caller via a separate callback.
      (void)decryptedMemo;
    }

    results.push_back(std::move(fo));
    return true;
  }

  bool NewOutputScanner::tryScanMultisigOutput(
      const cn::MultisigPaymentOutput &out,
      uint64_t amount,
      uint32_t outputIndex,
      uint32_t globalIndex,
      bool hasGlobalOutputIndex,
      uint32_t blockHeight,
      const crypto::Hash &txHash,
      const crypto::PublicKey &txPubKey,
      const crypto::SecretKey &viewSecretKey,
      const crypto::PublicKey &spendPublicKey,
      std::vector<BoltSync::FoundOutput> &results)
  {
    // Step 1: View tag filter
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPubKey, viewSecretKey, derivation))
      return false;

    uint8_t expectedTag = BoltSync::computeWalletViewTag(derivation, outputIndex);
    if (expectedTag != out.view_tag)
      return false;

    // Step 2: Check each key in the multisig group
    // For multisig outputs, each key uses key_index + position within the group
    for (size_t ki = 0; ki < out.keys.size(); ++ki)
    {
      crypto::PublicKey derivedKey;
      if (!crypto::derive_public_key(derivation, out.key_index + static_cast<size_t>(ki), spendPublicKey, derivedKey))
        continue;

      if (derivedKey == out.keys[ki])
      {
        BoltSync::FoundOutput fo;
        fo.blockHeight = blockHeight;
        fo.txHash = txHash;
        fo.outputIndex = outputIndex;
        fo.amount = amount;
        fo.outputKey = out.keys[ki];
        fo.txPublicKey = txPubKey;
        fo.txExtra = std::vector<uint8_t>();
        fo.term = out.term;
        fo.keyDerivationIndex = static_cast<uint32_t>(out.key_index + ki);
        fo.hasKeyDerivationIndex = true;

        // Set deposit status based on flags
        fo.isDeposit = out.isTimeLocked();
        if (hasGlobalOutputIndex)
        {
          fo.globalOutputIndex = globalIndex;
          fo.hasGlobalOutputIndex = true;
        }

        // Note: out.isAuthorized() indicates a flexible bridge/HTLC deposit.
        // The wallet can check this flag via the MultisigPaymentOutput when
        // processing the output for spending.

        results.push_back(std::move(fo));
        return true;
      }
    }

    return false;
  }

} // namespace BoltCore