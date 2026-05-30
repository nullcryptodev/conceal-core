// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "CryptoTypes.h"

namespace cn
{

  //
  // New output type tags for post-fork blocks (0x04 - 0x07)
  // These extend TransactionOutputTarget with self-describing fields.
  // Existing types 0x02 (KeyOutput) and 0x03 (MultisignatureOutput) remain
  // valid for pre-fork blocks only.
  //

  enum class NewOutputType : uint8_t
  {
    StandardPayment = 0x04,
    MultisigPayment = 0x05,
    DomainRegistration = 0x06,
    DomainDeletion = 0x07
  };

  //
  // Multisig flags for 0x05 outputs
  //

  enum class MultisigFlags : uint8_t
  {
    None = 0x00,
    TimeLocked = 0x01, // Fixed-term staking deposit
    Authorized = 0x02, // Flexible deposit for bridge/HTLC
    Both = 0x03        // TimeLocked | Authorized
  };

  //
  // 0x04 - Standard Payment Output
  // Replaces KeyOutput for post-fork blocks. Carries view_tag, key_index,
  // and an encrypted memo so wallets can scan without external indexes.
  //

  struct StandardPaymentOutput
  {
    uint8_t view_tag;                    // First byte of H("view_tag" || txPubKey || varint(output_index))
    uint16_t key_index;                  // Derivation index for this output
    uint8_t memo_size;                   // Length of encrypted_memo in bytes (0-255)
    std::vector<uint8_t> encrypted_memo; // Encrypted memo blob, can be empty
    crypto::PublicKey key;               // One-time public key, same as KeyOutput

    bool hasMemo() const { return memo_size > 0; }
  };

  //
  // 0x05 - Multisig Payment Output
  // Extends multisig with self-describing fields and a flags byte for
  // time-locked staking deposits and flexible authorized deposits.
  //

  struct MultisigPaymentOutput
  {
    uint8_t view_tag;                    // Same as StandardPaymentOutput
    uint16_t key_index;                  // Base derivation index for the multisig group
    uint8_t num_keys;                    // Number of keys in the multisig group
    uint8_t flags;                       // MultisigFlags bit field
    uint32_t term;                       // Block count for lockup (zero if not TimeLocked)
    uint8_t memo_size;                   // Length of encrypted_memo in bytes (0-255)
    std::vector<uint8_t> encrypted_memo; // Encrypted memo blob, can be empty
    std::vector<crypto::PublicKey> keys; // The multisig public keys

    bool isTimeLocked() const { return flags & static_cast<uint8_t>(MultisigFlags::TimeLocked); }
    bool isAuthorized() const { return flags & static_cast<uint8_t>(MultisigFlags::Authorized); }
    bool hasMemo() const { return memo_size > 0; }
  };

  //
  // 0x06 - Domain Registration Output
  // Registers a human-readable domain name on-chain. Not a spendable output.
  // The registration fee is paid via a separate 0x04 output in the same tx.
  //

  struct DomainRegistrationOutput
  {
    uint8_t view_tag;                       // Always zero
    uint16_t key_index;                     // Always zero
    uint8_t domain_len;                     // Length of the domain name string
    std::string domain;                     // ASCII domain name, e.g. "alice.conceal"
    uint8_t tier;                           // Privacy tier: 1=private, 2=shared, 3=public
    crypto::PublicKey domain_pub;           // Signing key for ownership proofs
    crypto::PublicKey domain_view_pub;      // View key for domain resolution
    uint8_t encrypted_addr_size;            // Always 64
    std::array<uint8_t, 64> encrypted_addr; // Encrypted spend pub + subaddress
    uint8_t metadata_len;                   // Length of optional metadata
    std::vector<uint8_t> metadata;          // Free-form metadata field

    bool isTier1() const { return tier == 1; }
    bool isTier2() const { return tier == 2; }
    bool isTier3() const { return tier == 3; }
  };

  //
  // 0x07 - Domain Deletion Output
  // Removes a domain from the active index. Proves ownership via signature
  // from the domain signing key.
  //

  struct DomainDeletionOutput
  {
    uint8_t view_tag;      // Always zero
    uint16_t key_index;    // Always zero
    uint8_t domain_len;    // Length of the domain name string
    std::string domain;    // The domain name to delete
    crypto::Signature sig; // Signature over domain name + deletion marker
  };

  //
  // Helper: determine which new output type a raw type byte represents
  //

  inline bool isNewOutputType(uint8_t typeByte)
  {
    return typeByte >= 0x04 && typeByte <= 0x07;
  }

  inline NewOutputType getNewOutputType(uint8_t typeByte)
  {
    switch (typeByte)
    {
    case 0x04:
      return NewOutputType::StandardPayment;
    case 0x05:
      return NewOutputType::MultisigPayment;
    case 0x06:
      return NewOutputType::DomainRegistration;
    case 0x07:
      return NewOutputType::DomainDeletion;
    default:
      throw std::runtime_error("Not a new output type byte");
    }
  }
} // namespace cn