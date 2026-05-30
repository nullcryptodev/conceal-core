// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>

#include "CryptoNoteCore/NewOutputTypes.h"
#include "Sidechain/BridgeMultisigHandler.h"

using namespace cn;
using namespace Sidechain;

namespace
{

  crypto::PublicKey makePubKey(uint8_t fill)
  {
    crypto::PublicKey pk;
    std::memset(pk.data, fill, sizeof(pk.data));
    return pk;
  }

} // anonymous namespace

TEST(BridgeMultisigHandler, createBridgeDeposit_timeLocked)
{
  crypto::PublicKey bridgeKey = makePubKey(0x01);
  crypto::PublicKey userKey = makePubKey(0x02);
  std::vector<uint8_t> htlcData = {0xAA, 0xBB, 0xCC};

  MultisigPaymentOutput out = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 1000000, 50000, htlcData);

  EXPECT_EQ(out.num_keys, 2);
  EXPECT_EQ(out.keys.size(), 2u);
  EXPECT_TRUE(out.isAuthorized());
  EXPECT_TRUE(out.isTimeLocked());
  EXPECT_EQ(out.term, 50000u);
  EXPECT_EQ(out.memo_size, 3);
  EXPECT_EQ(out.encrypted_memo.size(), 3u);
}

TEST(BridgeMultisigHandler, createBridgeDeposit_flexible)
{
  crypto::PublicKey bridgeKey = makePubKey(0x0A);
  crypto::PublicKey userKey = makePubKey(0x0B);

  MultisigPaymentOutput out = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 500000, 0, {});

  EXPECT_EQ(out.num_keys, 2);
  EXPECT_TRUE(out.isAuthorized());
  EXPECT_FALSE(out.isTimeLocked());
  EXPECT_EQ(out.term, 0u);
  EXPECT_EQ(out.memo_size, 0);
  EXPECT_TRUE(out.encrypted_memo.empty());
}

TEST(BridgeMultisigHandler, createWithdrawalOutput)
{
  crypto::PublicKey bridgeKey = makePubKey(0x10);
  crypto::PublicKey userKey = makePubKey(0x20);

  MultisigPaymentOutput out = BridgeMultisigHandler::createWithdrawalOutput(
      bridgeKey, userKey, 2000000);

  EXPECT_TRUE(out.isAuthorized());
  EXPECT_FALSE(out.isTimeLocked());
  EXPECT_EQ(out.keys[0].data[0], 0x10); // bridge key first
  EXPECT_EQ(out.keys[1].data[0], 0x20); // user key second
}

TEST(BridgeMultisigHandler, isBridgeOutput_true)
{
  crypto::PublicKey bridgeKey = makePubKey(0x42);
  crypto::PublicKey userKey = makePubKey(0x43);

  MultisigPaymentOutput out = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 100, 1000, {});

  EXPECT_TRUE(BridgeMultisigHandler::isBridgeOutput(out, bridgeKey));
}

TEST(BridgeMultisigHandler, isBridgeOutput_wrongKey)
{
  crypto::PublicKey bridgeKey = makePubKey(0x42);
  crypto::PublicKey userKey = makePubKey(0x43);
  crypto::PublicKey otherKey = makePubKey(0x99);

  MultisigPaymentOutput out = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 100, 1000, {});

  EXPECT_FALSE(BridgeMultisigHandler::isBridgeOutput(out, otherKey));
}

TEST(BridgeMultisigHandler, isBridgeOutput_notAuthorized)
{
  crypto::PublicKey key1 = makePubKey(0x50);
  crypto::PublicKey key2 = makePubKey(0x51);

  MultisigPaymentOutput out;
  out.num_keys = 2;
  out.keys = {key1, key2};
  out.flags = static_cast<uint8_t>(MultisigFlags::TimeLocked); // Only time-locked, NOT authorized
  out.term = 1000;

  EXPECT_FALSE(BridgeMultisigHandler::isBridgeOutput(out, key1));
}

TEST(BridgeMultisigHandler, validateBridgeOutput_valid)
{
  crypto::PublicKey bridgeKey = makePubKey(0x60);
  crypto::PublicKey userKey = makePubKey(0x61);

  MultisigPaymentOutput out = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 100, 1000, {0x01, 0x02});

  EXPECT_TRUE(BridgeMultisigHandler::validateBridgeOutput(out));
}

TEST(BridgeMultisigHandler, validateBridgeOutput_tooFewKeys)
{
  MultisigPaymentOutput out;
  out.num_keys = 1;
  out.keys = {makePubKey(0x70)};
  out.flags = static_cast<uint8_t>(MultisigFlags::Authorized);

  EXPECT_FALSE(BridgeMultisigHandler::validateBridgeOutput(out));
}

TEST(BridgeMultisigHandler, validateBridgeOutput_mismatchedSizes)
{
  MultisigPaymentOutput out;
  out.num_keys = 3; // Says 3 but only 2 keys
  out.keys = {makePubKey(0x80), makePubKey(0x81)};
  out.flags = static_cast<uint8_t>(MultisigFlags::Authorized);

  EXPECT_FALSE(BridgeMultisigHandler::validateBridgeOutput(out));
}

TEST(BridgeMultisigHandler, getExpiryHeight_timeLocked)
{
  crypto::PublicKey bridgeKey = makePubKey(0x90);
  crypto::PublicKey userKey = makePubKey(0x91);

  MultisigPaymentOutput out = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 100, 50000, {});

  uint64_t expiry = BridgeMultisigHandler::getExpiryHeight(out, 1000000);
  EXPECT_EQ(expiry, 1050000u);
}

TEST(BridgeMultisigHandler, getExpiryHeight_flexible)
{
  crypto::PublicKey bridgeKey = makePubKey(0xA0);
  crypto::PublicKey userKey = makePubKey(0xA1);

  MultisigPaymentOutput out = BridgeMultisigHandler::createWithdrawalOutput(
      bridgeKey, userKey, 100);

  uint64_t expiry = BridgeMultisigHandler::getExpiryHeight(out, 1000000);
  EXPECT_EQ(expiry, 0u); // No expiry for flexible deposits
}