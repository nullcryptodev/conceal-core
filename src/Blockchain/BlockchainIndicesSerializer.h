// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "Blockchain/Blockchain.h"
#include "Logging/LoggerRef.h"
#include "Serialization/ISerializer.h"
#include "Serialization/SerializationOverloads.h"

namespace cn
{

  class BlockchainIndicesSerializer
  {
  public:
    BlockchainIndicesSerializer(Blockchain &bs, const crypto::Hash &lastBlockHash,
                                logging::ILogger &logger)
        : m_bs(bs), m_lastBlockHash(lastBlockHash),
          logger(logger, "BlockchainIndicesSerializer") {}

    void serialize(ISerializer &s);

    bool loaded() const { return m_loaded; }

    template <class Archive>
    void serialize(Archive &ar, unsigned int version)
    {
      if (version < CURRENT_BLOCKCHAININDICES_STORAGE_ARCHIVE_VER)
        return;

      std::string operation;
      if (Archive::is_loading::value)
      {
        operation = "loading ";
        crypto::Hash blockHash;
        ar & blockHash;

        if (blockHash != m_lastBlockHash)
          return;
      }
      else
      {
        operation = "- saving ";
        ar & m_lastBlockHash;
      }

      logger(logging::INFO) << operation << "paymentID index";
      ar & m_bs.m_paymentIdIndex;

      logger(logging::INFO) << operation << "timestamp index";
      ar & m_bs.m_timestampIndex;

      logger(logging::INFO) << operation << "generated transactions index";
      ar & m_bs.m_generatedTransactionsIndex;

      m_loaded = true;
    }

  private:
    Blockchain &m_bs;
    crypto::Hash m_lastBlockHash;
    logging::LoggerRef logger;
    bool m_loaded = false;
  };

} // namespace cn