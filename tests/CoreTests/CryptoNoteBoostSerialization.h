// Copyright (c) 2012-2017 The Cryptonote developers
// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <boost/serialization/vector.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/variant.hpp>
#include <boost/serialization/set.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/array.hpp>
#include <boost/foreach.hpp>
#include <boost/serialization/is_bitwise_serializable.hpp>
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "UnorderedContainersBoostSerialization.h"
#include "crypto/crypto.h"

//namespace cn {
namespace boost
{
  namespace serialization
  {

  //---------------------------------------------------
  template <class Archive>
  inline void serialize(Archive &a, crypto::PublicKey &x, const boost::serialization::version_type ver)
  {
    a & reinterpret_cast<char (&)[sizeof(crypto::PublicKey)]>(x);
  }
  template <class Archive>
  inline void serialize(Archive &a, crypto::SecretKey &x, const boost::serialization::version_type ver)
  {
    a & reinterpret_cast<char (&)[sizeof(crypto::SecretKey)]>(x);
  }
  template <class Archive>
  inline void serialize(Archive &a, crypto::KeyDerivation &x, const boost::serialization::version_type ver)
  {
    a & reinterpret_cast<char (&)[sizeof(crypto::KeyDerivation)]>(x);
  }
  template <class Archive>
  inline void serialize(Archive &a, crypto::KeyImage &x, const boost::serialization::version_type ver)
  {
    a & reinterpret_cast<char (&)[sizeof(crypto::KeyImage)]>(x);
  }

  template <class Archive>
  inline void serialize(Archive &a, crypto::Signature &x, const boost::serialization::version_type ver)
  {
    a & reinterpret_cast<char (&)[sizeof(crypto::Signature)]>(x);
  }
  template <class Archive>
  inline void serialize(Archive &a, crypto::Hash &x, const boost::serialization::version_type ver)
  {
    a & reinterpret_cast<char (&)[sizeof(crypto::Hash)]>(x);
  }
  
  template <class Archive> void serialize(Archive& archive, cn::MultisignatureInput &output, unsigned int version) {
    archive & output.amount;
    archive & output.signatureCount;
    archive & output.outputIndex;
  }

  template <class Archive> void serialize(Archive& archive, cn::MultisignatureOutput &output, unsigned int version) {
    archive & output.keys;
    archive & output.requiredSignatureCount;
  }

  template <class Archive> void serialize(Archive& archive, cn::StandardPaymentOutput &output, unsigned int version) {
    archive & output.view_tag;
    archive & output.key_index;
    archive & output.memo_size;
    archive & output.encrypted_memo;
    archive & output.key;
  }

  template <class Archive> void serialize(Archive& archive, cn::MultisigPaymentOutput &output, unsigned int version) {
    archive & output.view_tag;
    archive & output.key_index;
    archive & output.num_keys;
    archive & output.flags;
    archive & output.term;
    archive & output.memo_size;
    archive & output.encrypted_memo;
    archive & output.keys;
  }

  template <class Archive> void serialize(Archive& archive, cn::DomainRegistrationOutput &output, unsigned int version) {
    archive & output.view_tag;
    archive & output.key_index;
    archive & output.domain_len;
    archive & output.domain;
    archive & output.tier;
    archive & output.domain_pub;
    archive & output.domain_view_pub;
    archive & output.encrypted_addr_size;
    archive & output.encrypted_addr;
    archive & output.metadata_len;
    archive & output.metadata;
  }

  template <class Archive> void serialize(Archive& archive, cn::DomainDeletionOutput &output, unsigned int version) {
    archive & output.view_tag;
    archive & output.key_index;
    archive & output.domain_len;
    archive & output.domain;
    archive & output.sig;
  }

  template <class Archive>
  inline void serialize(Archive &a, cn::KeyOutput &x, const boost::serialization::version_type ver)
  {
    a & x.key;
  }

  template <class Archive>
  inline void serialize(Archive &a, cn::BaseInput &x, const boost::serialization::version_type ver)
  {
    a & x.blockIndex;
  }

  template <class Archive>
  inline void serialize(Archive &a, cn::KeyInput &x, const boost::serialization::version_type ver)
  {
    a & x.amount;
    a & x.outputIndexes;
    a & x.keyImage;
  }

  template <class Archive>
  inline void serialize(Archive &a, cn::TransactionOutput &x, const boost::serialization::version_type ver)
  {
    a & x.amount;
    a & x.target;
  }


  template <class Archive>
  inline void serialize(Archive &a, cn::Transaction &x, const boost::serialization::version_type ver)
  {
    a & x.version;
    a & x.unlockTime;
    a & x.inputs;
    a & x.outputs;
    a & x.extra;
    a & x.signatures;
  }


  template <class Archive>
  inline void serialize(Archive &a, cn::Block &b, const boost::serialization::version_type ver)
  {
    a & b.majorVersion;
    a & b.minorVersion;
    a & b.timestamp;
    a & b.previousBlockHash;
    a & b.nonce;
    //------------------
    a & b.baseTransaction;
    a & b.transactionHashes;
  }
}
}

//}
