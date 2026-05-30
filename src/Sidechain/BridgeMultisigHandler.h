// BridgeMultisigHandler.h — builds and validates authorized multisig outputs for the bridge
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <vector>

#include "crypto/crypto.h"
#include "CryptoNoteCore/NewOutputTypes.h"

namespace Sidechain
{

  //
  // BridgeMultisigHandler creates and validates MultisigPaymentOutput
  // instances for the sidechain bridge. Post-fork, the bridge uses
  // authorized multisig outputs (flags = Authorized) instead of legacy
  // time-locked deposits.
  //
  // The HTLC logic stays entirely off-chain. The bridge verifies HTLC
  // conditions externally and produces the required signature. Consensus
  // only checks that the signature is valid against one of the keys in
  // the multisig group.
  //
  class BridgeMultisigHandler
  {
  public:
    //
    // Create a MultisigPaymentOutput for a bridge deposit.
    //
    // @param bridgeKey     The bridge's public key (authorized signer)
    // @param userKey       The user's public key (can reclaim after timeout)
    // @param amount        Deposit amount
    // @param lockBlocks    Number of blocks the deposit is locked (0 = flexible)
    // @param htlcData      Opaque HTLC data (stored off-chain by the bridge)
    // @return              A fully-formed MultisigPaymentOutput
    //
    static cn::MultisigPaymentOutput createBridgeDeposit(
        const crypto::PublicKey &bridgeKey,
        const crypto::PublicKey &userKey,
        uint64_t amount,
        uint32_t lockBlocks,
        const std::vector<uint8_t> &htlcData);

    //
    // Create a simple authorized output for bridge withdrawals.
    // No time lock — the bridge can sign at any time.
    //
    // @param bridgeKey     The bridge's public key
    // @param userKey       The user's public key
    // @param amount        Withdrawal amount
    // @return              A fully-formed MultisigPaymentOutput
    //
    static cn::MultisigPaymentOutput createWithdrawalOutput(
        const crypto::PublicKey &bridgeKey,
        const crypto::PublicKey &userKey,
        uint64_t amount);

    //
    // Check if an output is a bridge-authorized multisig output.
    // Returns true if flags include Authorized and the bridge key
    // is one of the multisig keys.
    //
    static bool isBridgeOutput(
        const cn::MultisigPaymentOutput &output,
        const crypto::PublicKey &bridgeKey);

    //
    // Validate that a multisig output meets bridge requirements.
    // Checks: num_keys matches keys size, at least 2 keys, valid flags.
    //
    static bool validateBridgeOutput(const cn::MultisigPaymentOutput &output);

    //
    // Get the HTLC expiry block height from a time-locked bridge output.
    // Returns 0 if not time-locked (flexible deposit).
    //
    static uint64_t getExpiryHeight(
        const cn::MultisigPaymentOutput &output,
        uint64_t creationHeight);

  private:
    BridgeMultisigHandler() {}
  };

} // namespace Sidechain