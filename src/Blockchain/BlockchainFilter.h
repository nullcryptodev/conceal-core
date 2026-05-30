// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "CryptoNote.h"
#include "Serialization/ISerializer.h"

namespace cn
{

  // A single output's filter data for the view-tag recognition protocol.
  //
  // The daemon precomputes these for every KeyOutput in every block.
  // Wallets download filter records and run the view-tag check locally,
  // only fetching full blocks for outputs that pass.
  //
  // view_tag = H("view_tag" || txPubKey || varint(outputIndex))[0]
  // 1-byte filter — eliminates ~255/256 non-owned outputs.
  //
  // The txPubKey is included so the wallet can recompute the view tag using
  // the key derivation (derivation = txPubKey * viewSecretKey) without
  // needing the full transaction data.
  struct FilterEntry
  {
    uint8_t view_tag;           // 1 byte  - view tag filter
    uint8_t output_index;       // 1 byte  - position inside the transaction
    uint16_t tx_index;          // 2 bytes - position inside the block
    crypto::PublicKey txPubKey; // 32 bytes - tx public key (for wallet-side derivation)
  };

  struct BlockFilterRecord
  {
    uint32_t block_height;
    std::vector<FilterEntry> entries;
  };

  // Serialization
  void serialize(FilterEntry &entry, ISerializer &s);
  void serialize(BlockFilterRecord &record, ISerializer &s);

  // Filter computation
  //
  // Builds a filter record for a block given its full transaction data.
  // Called during block processing when transactions are in memory.
  BlockFilterRecord buildBlockFilterRecord(const Block &block,
                                           uint32_t blockHeight,
                                           const std::vector<Transaction> &transactions);

  // Overload for retroactive building when transactions must be fetched from storage.
  BlockFilterRecord buildBlockFilterRecord(
      const Block &block,
      uint32_t blockHeight,
      const std::function<Transaction(const crypto::Hash &)> &getTransaction);

  // View tag computation — matches Monero's derive_view_tag specification.
  //
  // Daemon side: uses txPubKey since it lacks the recipient's view secret key.
  //   view_tag = H("view_tag" || txPubKey || varint(outputIndex))[0]
  //
  // Wallet side: the wallet recomputes using the key derivation:
  //   derivation = txPubKey * viewSecretKey
  //   view_tag = H("view_tag" || derivation || varint(outputIndex))[0]
  //
  // Both produce the same first byte because the derivation is a deterministic
  // function of (txPubKey, viewSecretKey), and the wallet holds viewSecretKey.
  uint8_t computeViewTag(const crypto::PublicKey &txPubKey, size_t outputIndex);

} // namespace cn