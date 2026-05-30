// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "BlockchainFilter.h"

#include <cstring>

#include "Common/Varint.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "CryptoNoteCore/TransactionExtra.h"

namespace cn
{
  // Serialization
  void serialize(FilterEntry &entry, ISerializer &s)
  {
    s.binary(&entry.view_tag, sizeof(entry.view_tag), "view_tag");
    s(entry.output_index, "output_index");
    s(entry.tx_index, "tx_index");
    s.binary(&entry.txPubKey, sizeof(entry.txPubKey), "tx_pub_key");
  }

  void serialize(BlockFilterRecord &record, ISerializer &s)
  {
    s(record.block_height, "block_height");

    size_t entryCount = record.entries.size();
    if (!s.beginArray(entryCount, "entries"))
    {
      record.entries.clear();
      return;
    }

    if (s.type() == ISerializer::INPUT)
    {
      record.entries.resize(entryCount);
    }

    for (size_t i = 0; i < entryCount; ++i)
    {
      serialize(record.entries[i], s);
    }

    s.endArray();
  }

  // Filter computation
  uint8_t computeViewTag(const crypto::PublicKey &txPubKey,
                         size_t outputIndex)
  {
    // Matches Monero's derive_view_tag specification:
    //   view_tag = H("view_tag" || derivation || varint(outputIndex))[0]
    //
    // Daemon side: uses txPubKey in place of derivation.
    // Buffer layout:
    //   salt[8]           = "view_tag"  (domain separator)
    //   txPubKey[32]      = transaction public key
    //   varint_buf[..10]  = outputIndex as varint

    uint8_t buf[8 + 32 + 10];
    std::memcpy(buf, "view_tag", 8);
    std::memcpy(buf + 8, &txPubKey, 32);

    char varint_buf[10];
    char *end = varint_buf;
    tools::write_varint(end, outputIndex);
    size_t varint_len = end - varint_buf;
    std::memcpy(buf + 40, varint_buf, varint_len);

    crypto::Hash h;
    crypto::cn_fast_hash(buf, 40 + varint_len, reinterpret_cast<char *>(&h));
    return reinterpret_cast<const uint8_t *>(&h)[0];
  }

  // Internal helpers
  namespace
  {

    // Zeroed public key for comparison
    static const crypto::PublicKey NULL_PUBLIC_KEY = {};

    void processTransaction(const Transaction &tx,
                            uint32_t blockHeight,
                            uint16_t txIndex,
                            std::vector<FilterEntry> &entries)
    {
      crypto::PublicKey txPubKey = getTransactionPublicKeyFromExtra(tx.extra);

      // If no public key in extra, this tx has no recoverable outputs
      if (std::memcmp(&txPubKey, &NULL_PUBLIC_KEY, sizeof(crypto::PublicKey)) == 0)
      {
        return;
      }

      uint8_t outputIndex = 0;
      for (const auto &output : tx.outputs)
      {
        if (output.target.type() == typeid(KeyOutput))
        {
          FilterEntry entry;
          entry.view_tag = computeViewTag(txPubKey, outputIndex);
          entry.output_index = outputIndex;
          entry.tx_index = txIndex;
          entry.txPubKey = txPubKey;
          entries.push_back(entry);
        }
        ++outputIndex;
      }
    }

  } // anonymous namespace

  // Public API
  BlockFilterRecord buildBlockFilterRecord(
      const Block &block,
      uint32_t blockHeight,
      const std::vector<Transaction> &transactions)
  {

    BlockFilterRecord record;
    record.block_height = blockHeight;

    uint16_t txIndex = 0;

    processTransaction(block.baseTransaction, blockHeight, txIndex, record.entries);
    ++txIndex;

    for (const auto &tx : transactions)
    {
      processTransaction(tx, blockHeight, txIndex, record.entries);
      ++txIndex;
    }

    return record;
  }

  BlockFilterRecord buildBlockFilterRecord(
      const Block &block,
      uint32_t blockHeight,
      const std::function<Transaction(const crypto::Hash &)> &getTransaction)
  {

    BlockFilterRecord record;
    record.block_height = blockHeight;

    uint16_t txIndex = 0;

    processTransaction(block.baseTransaction, blockHeight, txIndex, record.entries);
    ++txIndex;

    for (const auto &txHash : block.transactionHashes)
    {
      Transaction tx = getTransaction(txHash);
      processTransaction(tx, blockHeight, txIndex, record.entries);
      ++txIndex;
    }

    return record;
  }

} // namespace cn