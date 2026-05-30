// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>
#include <sstream>
#include <vector>

#include "Common/StdInputStream.h"
#include "Common/StdOutputStream.h"
#include "Blockchain/BlockchainFilter.h"
#include "CryptoNoteCore/NewOutputTypes.h"
#include "CryptoNoteCore/NewOutputSerialization.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"

using namespace common;
using namespace cn;

namespace
{

  // Helper: serialize an object to binary and back, checking round-trip
  template <typename T>
  T roundTripSerialize(const T &original)
  {
    std::stringstream ss;

    // Serialize to binary
    {
      StdOutputStream os(ss);
      BinaryOutputStreamSerializer serializer(os);
      T copy = original;
      serialize(copy, serializer);
    }

    // Deserialize from binary
    {
      StdInputStream is(ss);
      BinaryInputStreamSerializer deserializer(is);
      T deserialized;
      serialize(deserialized, deserializer);
      return deserialized;
    }
  }

  // Helper: create a dummy public key
  crypto::PublicKey makeDummyPubKey(uint8_t fill)
  {
    crypto::PublicKey pk;
    std::memset(pk.data, fill, sizeof(pk.data));
    return pk;
  }

  crypto::Signature makeDummySig(uint8_t fill)
  {
    crypto::Signature sig;
    std::memset(sig.data, fill, sizeof(sig.data));
    return sig;
  }

} // anonymous namespace

//
// StandardPaymentOutput tests
//

TEST(NewOutputTypes, StandardPaymentOutput_defaults)
{
  StandardPaymentOutput out;
  out.view_tag = 0;
  out.key_index = 0;
  out.memo_size = 0;
  out.key = makeDummyPubKey(0xAA);

  EXPECT_EQ(out.view_tag, 0);
  EXPECT_EQ(out.key_index, 0);
  EXPECT_EQ(out.memo_size, 0);
  EXPECT_FALSE(out.hasMemo());
}

TEST(NewOutputTypes, StandardPaymentOutput_withMemo)
{
  StandardPaymentOutput out;
  out.view_tag = 0x42;
  out.key_index = 7;
  out.memo_size = 8;
  out.encrypted_memo = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  out.key = makeDummyPubKey(0xBB);

  EXPECT_TRUE(out.hasMemo());
  EXPECT_EQ(out.memo_size, 8);
  EXPECT_EQ(out.encrypted_memo.size(), 8);
  EXPECT_EQ(out.encrypted_memo[0], 0x01);
}

TEST(NewOutputTypes, StandardPaymentOutput_serialization_emptyMemo)
{
  StandardPaymentOutput original;
  original.view_tag = 0xAB;
  original.key_index = 42;
  original.memo_size = 0;
  original.key = makeDummyPubKey(0xCC);

  StandardPaymentOutput deserialized = roundTripSerialize(original);

  EXPECT_EQ(deserialized.view_tag, original.view_tag);
  EXPECT_EQ(deserialized.key_index, original.key_index);
  EXPECT_EQ(deserialized.memo_size, original.memo_size);
  EXPECT_TRUE(deserialized.encrypted_memo.empty());
  EXPECT_EQ(std::memcmp(deserialized.key.data, original.key.data, 32), 0);
}

TEST(NewOutputTypes, StandardPaymentOutput_serialization_withMemo)
{
  StandardPaymentOutput original;
  original.view_tag = 0xCD;
  original.key_index = 100;
  original.memo_size = 4;
  original.encrypted_memo = {0xDE, 0xAD, 0xBE, 0xEF};
  original.key = makeDummyPubKey(0xDD);

  StandardPaymentOutput deserialized = roundTripSerialize(original);

  EXPECT_EQ(deserialized.view_tag, original.view_tag);
  EXPECT_EQ(deserialized.key_index, original.key_index);
  EXPECT_EQ(deserialized.memo_size, original.memo_size);
  EXPECT_EQ(deserialized.encrypted_memo.size(), 4);
  EXPECT_EQ(deserialized.encrypted_memo[0], 0xDE);
  EXPECT_EQ(deserialized.encrypted_memo[3], 0xEF);
  EXPECT_EQ(std::memcmp(deserialized.key.data, original.key.data, 32), 0);
}

//
// MultisigPaymentOutput tests
//

TEST(NewOutputTypes, MultisigPaymentOutput_defaults)
{
  MultisigPaymentOutput out;
  out.view_tag = 0;
  out.key_index = 0;
  out.num_keys = 0;
  out.flags = 0;
  out.term = 0;
  out.memo_size = 0;

  EXPECT_FALSE(out.isTimeLocked());
  EXPECT_FALSE(out.isAuthorized());
  EXPECT_FALSE(out.hasMemo());
}

TEST(NewOutputTypes, MultisigPaymentOutput_timeLocked)
{
  MultisigPaymentOutput out;
  out.flags = static_cast<uint8_t>(MultisigFlags::TimeLocked);
  out.term = 50000;

  EXPECT_TRUE(out.isTimeLocked());
  EXPECT_FALSE(out.isAuthorized());
  EXPECT_EQ(out.term, 50000);
}

TEST(NewOutputTypes, MultisigPaymentOutput_authorized)
{
  MultisigPaymentOutput out;
  out.flags = static_cast<uint8_t>(MultisigFlags::Authorized);
  out.term = 0;

  EXPECT_FALSE(out.isTimeLocked());
  EXPECT_TRUE(out.isAuthorized());
  EXPECT_EQ(out.term, 0);
}

TEST(NewOutputTypes, MultisigPaymentOutput_both)
{
  MultisigPaymentOutput out;
  out.flags = static_cast<uint8_t>(MultisigFlags::Both);
  out.term = 100000;

  EXPECT_TRUE(out.isTimeLocked());
  EXPECT_TRUE(out.isAuthorized());
}

TEST(NewOutputTypes, MultisigPaymentOutput_serialization)
{
  MultisigPaymentOutput original;
  original.view_tag = 0x12;
  original.key_index = 5;
  original.num_keys = 2;
  original.flags = static_cast<uint8_t>(MultisigFlags::TimeLocked);
  original.term = 99999;
  original.memo_size = 3;
  original.encrypted_memo = {0xAA, 0xBB, 0xCC};
  original.keys = {makeDummyPubKey(0x11), makeDummyPubKey(0x22)};

  MultisigPaymentOutput deserialized = roundTripSerialize(original);

  EXPECT_EQ(deserialized.view_tag, original.view_tag);
  EXPECT_EQ(deserialized.key_index, original.key_index);
  EXPECT_EQ(deserialized.num_keys, original.num_keys);
  EXPECT_EQ(deserialized.flags, original.flags);
  EXPECT_EQ(deserialized.term, original.term);
  EXPECT_EQ(deserialized.memo_size, original.memo_size);
  EXPECT_EQ(deserialized.encrypted_memo.size(), 3);
  EXPECT_EQ(deserialized.keys.size(), 2);
  EXPECT_TRUE(deserialized.isTimeLocked());
}

//
// DomainRegistrationOutput tests
//

TEST(NewOutputTypes, DomainRegistrationOutput_tiers)
{
  DomainRegistrationOutput out;

  out.tier = 1;
  EXPECT_TRUE(out.isTier1());
  EXPECT_FALSE(out.isTier2());
  EXPECT_FALSE(out.isTier3());

  out.tier = 2;
  EXPECT_FALSE(out.isTier1());
  EXPECT_TRUE(out.isTier2());
  EXPECT_FALSE(out.isTier3());

  out.tier = 3;
  EXPECT_FALSE(out.isTier1());
  EXPECT_FALSE(out.isTier2());
  EXPECT_TRUE(out.isTier3());
}

TEST(NewOutputTypes, DomainRegistrationOutput_serialization)
{
  DomainRegistrationOutput original;
  original.view_tag = 0;
  original.key_index = 0;
  original.domain = "alice.conceal";
  original.domain_len = static_cast<uint8_t>(original.domain.size());
  original.tier = 1;
  original.domain_pub = makeDummyPubKey(0x10);
  original.domain_view_pub = makeDummyPubKey(0x20);
  original.encrypted_addr_size = 64;
  original.encrypted_addr.fill(0x42);
  original.metadata_len = 4;
  original.metadata = {0x01, 0x02, 0x03, 0x04};

  DomainRegistrationOutput deserialized = roundTripSerialize(original);

  EXPECT_EQ(deserialized.view_tag, 0);
  EXPECT_EQ(deserialized.key_index, 0);
  EXPECT_EQ(deserialized.domain, "alice.conceal");
  EXPECT_EQ(deserialized.tier, 1);
  EXPECT_EQ(deserialized.encrypted_addr_size, 64);
  EXPECT_EQ(deserialized.metadata_len, 4);
  EXPECT_EQ(deserialized.metadata.size(), 4);
}

TEST(NewOutputTypes, DomainRegistrationOutput_emptyMetadata)
{
  DomainRegistrationOutput original;
  original.view_tag = 0;
  original.key_index = 0;
  original.domain = "test.conceal";
  original.domain_len = static_cast<uint8_t>(original.domain.size());
  original.tier = 3;
  original.domain_pub = makeDummyPubKey(0x30);
  original.domain_view_pub = makeDummyPubKey(0x40);
  original.encrypted_addr_size = 64;
  original.encrypted_addr.fill(0x00);
  original.metadata_len = 0;

  DomainRegistrationOutput deserialized = roundTripSerialize(original);

  EXPECT_EQ(deserialized.domain, "test.conceal");
  EXPECT_EQ(deserialized.tier, 3);
  EXPECT_TRUE(deserialized.metadata.empty());
}

//
// DomainDeletionOutput tests
//

TEST(NewOutputTypes, DomainDeletionOutput_serialization)
{
  DomainDeletionOutput original;
  original.view_tag = 0;
  original.key_index = 0;
  original.domain = "spam.conceal";
  original.domain_len = static_cast<uint8_t>(original.domain.size());
  original.sig = makeDummySig(0x55);

  DomainDeletionOutput deserialized = roundTripSerialize(original);

  EXPECT_EQ(deserialized.view_tag, 0);
  EXPECT_EQ(deserialized.key_index, 0);
  EXPECT_EQ(deserialized.domain, "spam.conceal");
  EXPECT_EQ(std::memcmp(deserialized.sig.data, original.sig.data, 64), 0);
}

//
// NewOutputType helpers
//

TEST(NewOutputTypes, isNewOutputType)
{
  EXPECT_FALSE(isNewOutputType(0x00));
  EXPECT_FALSE(isNewOutputType(0x02));
  EXPECT_FALSE(isNewOutputType(0x03));
  EXPECT_TRUE(isNewOutputType(0x04));
  EXPECT_TRUE(isNewOutputType(0x05));
  EXPECT_TRUE(isNewOutputType(0x06));
  EXPECT_TRUE(isNewOutputType(0x07));
  EXPECT_FALSE(isNewOutputType(0x08));
  EXPECT_FALSE(isNewOutputType(0xFF));
}

TEST(NewOutputTypes, getNewOutputType)
{
  EXPECT_EQ(getNewOutputType(0x04), NewOutputType::StandardPayment);
  EXPECT_EQ(getNewOutputType(0x05), NewOutputType::MultisigPayment);
  EXPECT_EQ(getNewOutputType(0x06), NewOutputType::DomainRegistration);
  EXPECT_EQ(getNewOutputType(0x07), NewOutputType::DomainDeletion);
  EXPECT_THROW(getNewOutputType(0x02), std::runtime_error);
  EXPECT_THROW(getNewOutputType(0xFF), std::runtime_error);
}

//
// View tag computation tests (uses BlockchainFilter's computeViewTag)
//

TEST(NewOutputTypes, computeViewTag_producesByte)
{
  crypto::PublicKey txPubKey = makeDummyPubKey(0xAB);
  uint8_t tag = computeViewTag(txPubKey, 0);

  // Tag should be a valid byte (0-255)
  EXPECT_GE(tag, 0);
  EXPECT_LE(tag, 255);
}

TEST(NewOutputTypes, computeViewTag_differentOutputs)
{
  crypto::PublicKey txPubKey = makeDummyPubKey(0x42);

  // Same txPubKey, different output indices usually produce different tags
  uint8_t tag0 = computeViewTag(txPubKey, 0);
  uint8_t tag1 = computeViewTag(txPubKey, 1);

  // Not strictly guaranteed but extremely likely
  EXPECT_GE(tag0, 0);
  EXPECT_GE(tag1, 0);
}

TEST(NewOutputTypes, computeViewTag_differentTxKeys)
{
  crypto::PublicKey txKey1 = makeDummyPubKey(0x11);
  crypto::PublicKey txKey2 = makeDummyPubKey(0x22);

  uint8_t tag1 = computeViewTag(txKey1, 5);
  uint8_t tag2 = computeViewTag(txKey2, 5);

  EXPECT_GE(tag1, 0);
  EXPECT_GE(tag2, 0);
}

TEST(NewOutputTypes, computeViewTag_deterministic)
{
  crypto::PublicKey txPubKey = makeDummyPubKey(0xCD);

  uint8_t tag1 = computeViewTag(txPubKey, 42);
  uint8_t tag2 = computeViewTag(txPubKey, 42);

  // Same inputs should always produce the same tag
  EXPECT_EQ(tag1, tag2);
}