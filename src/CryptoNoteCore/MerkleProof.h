// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <vector>

#include "crypto/hash.h"

namespace cn
{

  //
  // Merkle proof that a transaction exists in a block.
  //
  // Given a block's transaction hashes and the index of a transaction,
  // this generates the branch needed to verify inclusion against the
  // block's Merkle root hash.
  //
  struct MerkleProof
  {
    uint32_t txIndex;                 // Position of tx in block (0 = coinbase)
    std::vector<crypto::Hash> branch; // Sibling hashes along the path
    crypto::Hash rootHash;            // Expected Merkle root of the block

    //
    // Generate a Merkle proof for a transaction at txIndex within a block.
    // @param txHashes  All transaction hashes in the block (including coinbase)
    // @param txIndex   Index of the transaction to prove
    // @return          MerkleProof, or empty proof if txIndex is out of range
    //
    static MerkleProof build(
        const std::vector<crypto::Hash> &txHashes,
        uint32_t txIndex);

    //
    // Verify a Merkle proof given a transaction hash.
    // @param txHash    Hash of the transaction being verified
    // @param proof     The Merkle proof
    // @return          true if txHash is proven to be in the block
    //
    static bool verify(
        const crypto::Hash &txHash,
        const MerkleProof &proof);

    bool valid() const { return !branch.empty() || txIndex == 0; }
  };

} // namespace cn