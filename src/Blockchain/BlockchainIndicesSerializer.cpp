// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain/BlockchainIndicesSerializer.h"

namespace cn
{

  void BlockchainIndicesSerializer::serialize(ISerializer &s)
  {
    uint8_t version = CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER;
    KV_MEMBER(version);

    if (version != CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER)
      return;

    std::string operation;
    if (s.type() == ISerializer::INPUT)
    {
      operation = "loading ";
      crypto::Hash blockHash;
      s(blockHash, "blockHash");

      if (blockHash != m_lastBlockHash)
        return;
    }
    else
    {
      operation = "- saving ";
      s(m_lastBlockHash, "blockHash");
    }

    logger(logging::INFO) << operation << "paymentID index";
    s(m_bs.m_paymentIdIndex, "paymentIdIndex");

    logger(logging::INFO) << operation << "timestamp index";
    s(m_bs.m_timestampIndex, "timestampIndex");

    logger(logging::INFO) << operation << "generated transactions index";
    s(m_bs.m_generatedTransactionsIndex, "generatedTransactionsIndex");

    m_loaded = true;
  }

} // namespace cn