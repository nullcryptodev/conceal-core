// OwnedOutputScanner implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "OwnedOutputScanner.h"
#include "NewOutputScanner.h"

#include "BoltSync/CryptoHelpers.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"

namespace BoltCore
{
  namespace
  {
    void appendLegacyOwnedOutputs(
        const cn::Transaction &tx,
        const crypto::Hash &txHash,
        const crypto::PublicKey &txPubKey,
        uint32_t blockHeight,
        const crypto::SecretKey &viewKey,
        const crypto::PublicKey &spendPub,
        const crypto::SecretKey *spendKey,
        std::vector<OutputInfo> &outputs)
    {
      crypto::KeyDerivation derivation;
      if (!crypto::generate_key_derivation(txPubKey, viewKey, derivation))
        return;

      for (size_t o = 0; o < tx.outputs.size(); ++o)
      {
        const auto &out = tx.outputs[o];
        if (out.target.type() == typeid(cn::KeyOutput))
        {
          const auto &keyOut = boost::get<cn::KeyOutput>(out.target);
          crypto::PublicKey derivedKey;
          if (!crypto::derive_public_key(derivation, o, spendPub, derivedKey) || derivedKey != keyOut.key)
            continue;

          OutputInfo info = {};
          info.blockHeight = blockHeight;
          info.txHash = txHash;
          info.amount = out.amount;
          info.outputIndex = static_cast<uint32_t>(o);
          info.outputKey = keyOut.key;
          info.txPublicKey = txPubKey;
          info.spent = false;
          info.isDeposit = false;
          info.term = 0;
          if (spendKey)
          {
            const crypto::SecretKey outSec = BoltSync::deriveOutputSecretKey(derivation, o, *spendKey);
            crypto::generate_key_image(keyOut.key, outSec, info.keyImage);
          }
          outputs.push_back(std::move(info));
        }
        else if (out.target.type() == typeid(cn::MultisignatureOutput))
        {
          const auto &msigOut = boost::get<cn::MultisignatureOutput>(out.target);
          for (size_t ki = 0; ki < msigOut.keys.size(); ++ki)
          {
            crypto::PublicKey recoveredSpend;
            if (!crypto::underive_public_key(derivation, o, msigOut.keys[ki], recoveredSpend) ||
                recoveredSpend != spendPub)
              continue;

            OutputInfo info = {};
            info.blockHeight = blockHeight;
            info.txHash = txHash;
            info.amount = out.amount;
            info.outputIndex = static_cast<uint32_t>(o);
            info.outputKey = msigOut.keys[ki];
            info.txPublicKey = txPubKey;
            info.spent = false;
            info.isDeposit = (msigOut.term > 0);
            info.term = msigOut.term;
            outputs.push_back(std::move(info));
            break;
          }
        }
      }
    }
  }

  std::vector<OutputInfo> scanOwnedOutputsInTransaction(
      const cn::Transaction &tx,
      uint32_t blockHeight,
      const crypto::SecretKey &viewKey,
      const crypto::PublicKey &spendPub,
      const crypto::SecretKey *spendKey)
  {
    std::vector<OutputInfo> outputs;

    static const crypto::PublicKey NULL_KEY = {};
    const crypto::PublicKey txPubKey = cn::getTransactionPublicKeyFromExtra(tx.extra);
    if (txPubKey == NULL_KEY)
      return outputs;

    crypto::Hash txHash = cn::getObjectHash(tx);

    if (NewOutputScanner::hasNewOutputs(tx))
    {
      std::vector<BoltSync::FoundOutput> found;
      NewOutputScanner::scanTransaction(
          tx, txPubKey, {}, blockHeight, viewKey, spendPub, spendKey, found);
      for (const auto &fo : found)
      {
        OutputInfo info = {};
        info.blockHeight = fo.blockHeight;
        info.txHash = fo.txHash;
        info.outputIndex = fo.outputIndex;
        info.amount = fo.amount;
        info.outputKey = fo.outputKey;
        info.txPublicKey = fo.txPublicKey;
        info.keyImage = fo.keyImage;
        info.spent = false;
        info.isDeposit = fo.isDeposit;
        info.term = fo.term;
        info.keyDerivationIndex = fo.keyDerivationIndex;
        info.hasKeyDerivationIndex = fo.hasKeyDerivationIndex;
        outputs.push_back(std::move(info));
      }
    }

    appendLegacyOwnedOutputs(tx, txHash, txPubKey, blockHeight, viewKey, spendPub, spendKey, outputs);
    return outputs;
  }
}
