// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "NewOutputTypes.h"
#include "MerkleProof.h"

namespace cn
{

  struct Block;
  struct Transaction;

  //
  // DomainIndex maintains an in-memory mapping from domain names to their
  // registration records. It is built during initial sync and updated
  // incrementally with each new block.
  //
  // The index is tiny (roughly 100 bytes per registered domain) and can
  // be stored entirely in memory. For persistence, it can be serialized
  // to/from MDBX.
  //

  class DomainIndex
  {
  public:
    struct DomainEntry
    {
      std::string name;
      uint64_t registrationHeight;
      uint32_t transactionIndex;
      uint32_t outputIndex;
      DomainRegistrationOutput registration;

      bool isActive() const { return registrationHeight > 0; }
    };

    //
    // Domain proof bundle for light client verification.
    // Contains the domain entry, a Merkle proof of inclusion in the block,
    // and the transaction hash.
    //
    struct DomainProof
    {
      DomainEntry entry;
      MerkleProof proof;
      crypto::Hash txHash;
    };

    DomainIndex() : m_lastProcessedHeight(0) {}

    //
    // Build the index from scratch by scanning a range of blocks
    // Call this during initial sync after the fork height.
    //
    void buildFromBlocks(const std::vector<Block> &blocks, uint64_t startHeight);

    //
    // Process a single block incrementally
    // Call this for each new block as it arrives.
    //
    void processBlock(const Block &block, uint64_t height);

    //
    // Process a domain deletion
    //
    void processDeletion(const DomainDeletionOutput &deletion, uint64_t height);

    //
    // Look up a domain name
    // @return Pointer to the domain entry if found and active, nullptr otherwise.
    //
    const DomainEntry *resolve(const std::string &domain) const;

    //
    // Check if a domain name is registered and active
    //
    bool isRegistered(const std::string &domain) const;

    //
    // Get total number of registered (active) domains
    //
    size_t size() const { return m_index.size(); }

    //
    // Serialize the index to a binary blob for MDBX storage
    //
    std::vector<uint8_t> serialize() const;

    //
    // Deserialize the index from a binary blob
    //
    bool deserialize(const std::vector<uint8_t> &data);

    //
    // Clear the entire index
    //
    void clear() { m_index.clear(); }

    //
    // Resolve a domain with a Merkle proof for light client verification.
    // @param domain           Domain name to look up
    // @param blockTxHashes    All transaction hashes in the block containing the domain
    // @param getBlockHash     Function to get the block hash (used for the Merkle root)
    // @return                 Pointer to DomainProof if found, nullptr otherwise.
    //                         Caller owns the returned object.
    //
    DomainProof *resolveWithProof(
        const std::string &domain,
        const std::vector<crypto::Hash> &blockTxHashes,
        const std::function<crypto::Hash(const Block &)> &getBlockHash) const;

    // Helper: extract domain registrations from a transaction
    void extractFromTransaction(const Transaction &tx, uint64_t height,
                                uint32_t txIndex);

  private:
    // Domain name -> DomainEntry (only active domains are stored)
    std::unordered_map<std::string, DomainEntry> m_index;

    // Block height of the last processed block (for incremental updates)
    uint64_t m_lastProcessedHeight;
  };

} // namespace cn