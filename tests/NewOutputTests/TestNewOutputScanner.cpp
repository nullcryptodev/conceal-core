// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>

#include "BoltCore/NewOutputScanner.h"
#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/TransactionExtra.h"

using namespace cn;
using namespace BoltCore;
using namespace BoltSync;

namespace
{

  // Helper: create a dummy public key filled with a byte pattern
  crypto::PublicKey makePubKey(uint8_t fill)
  {
    crypto::PublicKey pk;
    std::memset(pk.data, fill, sizeof(pk.data));
    return pk;
  }

  crypto::SecretKey makeSecKey(uint8_t fill)
  {
    crypto::SecretKey sk;
    std::memset(sk.data, fill, sizeof(sk.data));
    return sk;
  }

} // anonymous namespace

TEST(NewOutputScanner, hasNewOutputs_empty)
{
  Transaction tx;
  tx.outputs.clear();
  EXPECT_FALSE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, hasNewOutputs_legacyKeyOutput)
{
  Transaction tx;
  TransactionOutput out;
  out.target = KeyOutput{};
  tx.outputs.push_back(out);
  EXPECT_FALSE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, hasNewOutputs_standardPayment)
{
  Transaction tx;
  TransactionOutput out;
  out.target = StandardPaymentOutput{};
  tx.outputs.push_back(out);
  EXPECT_TRUE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, hasNewOutputs_multisigPayment)
{
  Transaction tx;
  TransactionOutput out;
  out.target = MultisigPaymentOutput{};
  tx.outputs.push_back(out);
  EXPECT_TRUE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, hasNewOutputs_domainRegistration)
{
  Transaction tx;
  TransactionOutput out;
  out.target = DomainRegistrationOutput{};
  tx.outputs.push_back(out);
  EXPECT_TRUE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, hasNewOutputs_domainDeletion)
{
  Transaction tx;
  TransactionOutput out;
  out.target = DomainDeletionOutput{};
  tx.outputs.push_back(out);
  EXPECT_TRUE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, hasNewOutputs_mixed)
{
  Transaction tx;
  TransactionOutput out1;
  out1.target = KeyOutput{};
  tx.outputs.push_back(out1);

  TransactionOutput out2;
  out2.target = StandardPaymentOutput{};
  tx.outputs.push_back(out2);

  EXPECT_TRUE(NewOutputScanner::hasNewOutputs(tx));
}

TEST(NewOutputScanner, scanTransaction_emptyTx)
{
  Transaction tx;
  tx.outputs.clear();

  crypto::PublicKey txPubKey = makePubKey(0x01);
  std::vector<uint32_t> globalIndexes;
  std::vector<FoundOutput> results;

  NewOutputScanner::scanTransaction(
      tx, txPubKey, globalIndexes, 100,
      makeSecKey(0xAA), makePubKey(0xBB), NULL, results);

  EXPECT_TRUE(results.empty());
}

TEST(NewOutputScanner, scanTransaction_skipsDomainOutputs)
{
  // Domain outputs (0x06, 0x07) are not spendable and should be skipped
  Transaction tx;

  TransactionOutput out;
  out.amount = 0;
  out.target = DomainRegistrationOutput{};
  tx.outputs.push_back(out);

  crypto::PublicKey txPubKey = makePubKey(0x01);
  std::vector<uint32_t> globalIndexes = {0};
  std::vector<FoundOutput> results;

  NewOutputScanner::scanTransaction(
      tx, txPubKey, globalIndexes, 100,
      makeSecKey(0xAA), makePubKey(0xBB), NULL, results);

  // Domain outputs should not produce FoundOutput entries
  EXPECT_TRUE(results.empty());
}