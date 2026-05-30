// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "DomainIndex.h"

#include <cstring>

#include "CryptoNoteBasic.h"

namespace cn
{

  void DomainIndex::buildFromBlocks(const std::vector<Block> &blocks, uint64_t startHeight)
  {
    for (size_t i = 0; i < blocks.size(); ++i)
    {
      processBlock(blocks[i], startHeight + i);
    }
  }

  void DomainIndex::processBlock(const Block &block, uint64_t height)
  {
    extractFromTransaction(block.baseTransaction, height, 0);
    m_lastProcessedHeight = height;
  }

  void DomainIndex::extractFromTransaction(const Transaction &tx, uint64_t height, uint32_t txIndex)
  {
    // Scan outputs for DomainRegistrationOutput and DomainDeletionOutput.
    for (size_t outIdx = 0; outIdx < tx.outputs.size(); ++outIdx)
    {
      const TransactionOutput &output = tx.outputs[outIdx];

      if (output.target.type() == typeid(DomainRegistrationOutput))
      {
        const DomainRegistrationOutput &reg = boost::get<DomainRegistrationOutput>(output.target);

        DomainEntry entry;
        entry.name = reg.domain;
        entry.registrationHeight = height;
        entry.transactionIndex = txIndex;
        entry.outputIndex = static_cast<uint32_t>(outIdx);
        entry.registration = reg;

        m_index[reg.domain] = entry;
      }
      else if (output.target.type() == typeid(DomainDeletionOutput))
      {
        const DomainDeletionOutput &del = boost::get<DomainDeletionOutput>(output.target);
        std::unordered_map<std::string, DomainEntry>::iterator it = m_index.find(del.domain);
        if (it != m_index.end())
        {
          m_index.erase(it);
        }
      }
    }
  }

  void DomainIndex::processDeletion(const DomainDeletionOutput &deletion, uint64_t height)
  {
    std::unordered_map<std::string, DomainEntry>::iterator it = m_index.find(deletion.domain);
    if (it != m_index.end())
    {
      m_index.erase(it);
    }
    (void)height;
  }

  const DomainIndex::DomainEntry *DomainIndex::resolve(const std::string &domain) const
  {
    std::unordered_map<std::string, DomainEntry>::const_iterator it = m_index.find(domain);
    if (it != m_index.end() && it->second.isActive())
    {
      return &it->second;
    }
    return NULL;
  }

  bool DomainIndex::isRegistered(const std::string &domain) const
  {
    return resolve(domain) != NULL;
  }

  std::vector<uint8_t> DomainIndex::serialize() const
  {
    std::vector<uint8_t> data;

    uint32_t count = static_cast<uint32_t>(m_index.size());
    data.insert(data.end(),
                reinterpret_cast<const uint8_t *>(&count),
                reinterpret_cast<const uint8_t *>(&count) + sizeof(count));

    for (std::unordered_map<std::string, DomainEntry>::const_iterator it = m_index.begin();
         it != m_index.end(); ++it)
    {
      const std::string &name = it->first;
      const DomainEntry &entry = it->second;

      uint8_t nameLen = static_cast<uint8_t>(name.size());
      data.push_back(nameLen);
      data.insert(data.end(), name.begin(), name.end());

      const uint8_t *heightPtr = reinterpret_cast<const uint8_t *>(&entry.registrationHeight);
      data.insert(data.end(), heightPtr, heightPtr + sizeof(uint64_t));

      const uint8_t *txIdxPtr = reinterpret_cast<const uint8_t *>(&entry.transactionIndex);
      data.insert(data.end(), txIdxPtr, txIdxPtr + sizeof(uint32_t));

      const uint8_t *outIdxPtr = reinterpret_cast<const uint8_t *>(&entry.outputIndex);
      data.insert(data.end(), outIdxPtr, outIdxPtr + sizeof(uint32_t));
    }

    return data;
  }

  bool DomainIndex::deserialize(const std::vector<uint8_t> &data)
  {
    m_index.clear();

    if (data.size() < 4)
    {
      return false;
    }

    size_t pos = 0;

    uint32_t count;
    std::memcpy(&count, data.data() + pos, sizeof(count));
    pos += sizeof(count);

    for (uint32_t i = 0; i < count; ++i)
    {
      if (pos >= data.size())
        return false;

      uint8_t nameLen = data[pos++];

      if (pos + nameLen > data.size())
        return false;
      std::string name(data.begin() + pos, data.begin() + pos + nameLen);
      pos += nameLen;

      if (pos + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint32_t) > data.size())
      {
        return false;
      }

      DomainEntry entry;
      entry.name = name;
      std::memcpy(&entry.registrationHeight, data.data() + pos, sizeof(uint64_t));
      pos += sizeof(uint64_t);
      std::memcpy(&entry.transactionIndex, data.data() + pos, sizeof(uint32_t));
      pos += sizeof(uint32_t);
      std::memcpy(&entry.outputIndex, data.data() + pos, sizeof(uint32_t));
      pos += sizeof(uint32_t);

      m_index[name] = entry;
    }

    return true;
  }

  DomainIndex::DomainProof *DomainIndex::resolveWithProof(
      const std::string &domain,
      const std::vector<crypto::Hash> &blockTxHashes,
      const std::function<crypto::Hash(const Block &)> &getBlockHash) const
  {
    std::unordered_map<std::string, DomainEntry>::const_iterator it = m_index.find(domain);
    if (it == m_index.end() || !it->second.isActive())
      return NULL;

    DomainProof *result = new DomainProof();
    result->entry = it->second;

    // Build Merkle proof for the transaction containing this domain registration
    result->proof = MerkleProof::build(blockTxHashes, result->entry.transactionIndex);

    (void)getBlockHash; // Reserved for future use (block hash verification)

    return result;
  }

} // namespace cn