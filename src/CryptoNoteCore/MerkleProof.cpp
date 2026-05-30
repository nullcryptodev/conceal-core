// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "MerkleProof.h"

#include <cstring>

namespace cn
{

  MerkleProof MerkleProof::build(
      const std::vector<crypto::Hash> &txHashes,
      uint32_t txIndex)
  {
    MerkleProof proof;
    proof.txIndex = txIndex;

    if (txIndex >= txHashes.size())
      return proof; // Invalid — empty branch

    // Build the Merkle tree path
    // The tree_hash function uses a binary tree structure.
    // We need to collect sibling hashes at each level.
    std::vector<crypto::Hash> tree(txHashes.begin(), txHashes.end());
    size_t idx = txIndex;

    while (tree.size() > 1)
    {
      // If odd number of elements, duplicate the last one
      if (tree.size() % 2 != 0)
        tree.push_back(tree.back());

      // Find sibling
      size_t siblingIdx = (idx % 2 == 0) ? idx + 1 : idx - 1;
      if (siblingIdx < tree.size())
        proof.branch.push_back(tree[siblingIdx]);

      // Move up one level
      idx /= 2;
      std::vector<crypto::Hash> nextLevel;
      for (size_t i = 0; i < tree.size(); i += 2)
      {
        crypto::Hash combined;
        crypto::Hash hashes[2] = {tree[i], tree[i + 1]};
        tree_hash(hashes, 2, combined);
        nextLevel.push_back(combined);
      }
      tree = std::move(nextLevel);
    }

    if (!tree.empty())
      proof.rootHash = tree[0];

    return proof;
  }

  bool MerkleProof::verify(
      const crypto::Hash &txHash,
      const MerkleProof &proof)
  {
    if (!proof.valid())
      return false;

    crypto::Hash current = txHash;
    size_t idx = proof.txIndex;

    for (size_t i = 0; i < proof.branch.size(); ++i)
    {
      crypto::Hash hashes[2];
      if (idx % 2 == 0)
      {
        hashes[0] = current;
        hashes[1] = proof.branch[i];
      }
      else
      {
        hashes[0] = proof.branch[i];
        hashes[1] = current;
      }
      tree_hash(hashes, 2, current);
      idx /= 2;
    }

    return std::memcmp(current.data, proof.rootHash.data, sizeof(crypto::Hash)) == 0;
  }

} // namespace cn