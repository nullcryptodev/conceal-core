// BridgeMultisigHandler.cpp — implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BridgeMultisigHandler.h"

#include <cstring>
#include <stdexcept>

namespace Sidechain
{

  cn::MultisigPaymentOutput BridgeMultisigHandler::createBridgeDeposit(
      const crypto::PublicKey &bridgeKey,
      const crypto::PublicKey &userKey,
      uint64_t amount,
      uint32_t lockBlocks,
      const std::vector<uint8_t> &htlcData)
  {
    cn::MultisigPaymentOutput output;

    // View tag and key_index will be set by the transaction builder
    output.view_tag = 0;
    output.key_index = 0;
    output.num_keys = 2;

    // Set flags based on whether there's a time lock
    if (lockBlocks > 0)
    {
      output.flags = static_cast<uint8_t>(cn::MultisigFlags::Both); // TimeLocked + Authorized
      output.term = lockBlocks;
    }
    else
    {
      output.flags = static_cast<uint8_t>(cn::MultisigFlags::Authorized);
      output.term = 0;
    }

    // First key is the bridge (authorized signer), second is the user
    output.keys.push_back(bridgeKey);
    output.keys.push_back(userKey);

    // Store HTLC data in encrypted memo
    if (!htlcData.empty())
    {
      output.memo_size = static_cast<uint8_t>(htlcData.size());
      output.encrypted_memo = htlcData;
    }
    else
    {
      output.memo_size = 0;
    }

    (void)amount; // Amount is stored at the TransactionOutput level

    return output;
  }

  cn::MultisigPaymentOutput BridgeMultisigHandler::createWithdrawalOutput(
      const crypto::PublicKey &bridgeKey,
      const crypto::PublicKey &userKey,
      uint64_t amount)
  {
    // A withdrawal is just an authorized output with no time lock
    // and no HTLC data — the bridge signs it immediately.
    return createBridgeDeposit(bridgeKey, userKey, amount, 0, {});
  }

  bool BridgeMultisigHandler::isBridgeOutput(
      const cn::MultisigPaymentOutput &output,
      const crypto::PublicKey &bridgeKey)
  {
    if (!output.isAuthorized())
      return false;

    // Check if bridge key is in the multisig group
    for (size_t i = 0; i < output.keys.size(); ++i)
    {
      if (std::memcmp(output.keys[i].data, bridgeKey.data, sizeof(crypto::PublicKey)) == 0)
        return true;
    }
    return false;
  }

  bool BridgeMultisigHandler::validateBridgeOutput(const cn::MultisigPaymentOutput &output)
  {
    // Must have at least 2 keys (bridge + user)
    if (output.num_keys < 2)
      return false;

    // num_keys must match actual keys size
    if (output.num_keys != output.keys.size())
      return false;

    // Must be authorized (bridge needs to sign)
    if (!output.isAuthorized())
      return false;

    // If time-locked, term must be non-zero
    if (output.isTimeLocked() && output.term == 0)
      return false;

    // Memo size must match actual memo data
    if (output.memo_size != output.encrypted_memo.size())
      return false;

    return true;
  }

  uint64_t BridgeMultisigHandler::getExpiryHeight(
      const cn::MultisigPaymentOutput &output,
      uint64_t creationHeight)
  {
    if (output.isTimeLocked())
      return creationHeight + output.term;
    return 0; // No expiry — flexible authorized deposit
  }

} // namespace Sidechain