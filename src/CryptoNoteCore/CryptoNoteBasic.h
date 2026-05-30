// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <boost/utility/value_init.hpp>
#include <CryptoNote.h>

namespace cn {
  struct BlockHeaderPOD
  {
    uint32_t majorVersion;
    uint32_t minorVersion;
    uint64_t timestamp;
    crypto::Hash previousBlockHash;
    uint32_t nonce;
    uint64_t blockCumulativeSize;
    uint64_t cumulativeDifficulty;
    uint64_t alreadyGeneratedCoins;
    uint32_t height;
  };

  const crypto::Hash NULL_HASH = boost::value_initialized<crypto::Hash>();
  const crypto::PublicKey NULL_PUBLIC_KEY = boost::value_initialized<crypto::PublicKey>();
  const crypto::SecretKey NULL_SECRET_KEY = boost::value_initialized<crypto::SecretKey>();

  KeyPair generateKeyPair();
}
