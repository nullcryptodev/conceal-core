// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>
#include <sstream>
#include <vector>

#include "Common/StdInputStream.h"
#include "Common/StdOutputStream.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteSerialization.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/DomainIndex.h"
#include "CryptoNoteCore/EncryptedMemo.h"
#include "CryptoNoteCore/MerkleProof.h"
#include "CryptoNoteCore/NewOutputTypes.h"
#include "CryptoNoteCore/NewOutputSerialization.h"
#include "CryptoNoteCore/Poly1305.h"
#include "CryptoNoteCore/TransactionExtra.h"
#include "BoltCore/NewOutputScanner.h"
#include "BoltSync/BoltSync.h"
#include "BoltSync/CryptoHelpers.h"
#include "Sidechain/BridgeMultisigHandler.h"
#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"

using namespace common;
using namespace cn;
using namespace Sidechain;

namespace
{

  // ── Helpers ────────────────────────────────────────────────────────────────

  crypto::PublicKey makePubKey(uint8_t fill)
  {
    crypto::PublicKey pk;
    std::memset(pk.data, fill, sizeof(pk.data));
    return pk;
  }

  // Serialize a transaction to binary and back
  Transaction roundTripTx(const Transaction &original)
  {
    BinaryArray blob = toBinaryArray(original);
    Transaction restored;
    EXPECT_TRUE(fromBinaryArray(restored, blob));
    return restored;
  }

  // Create a valid transaction public key and add it to extra
  KeyPair makeTxKeys()
  {
    return generateKeyPair();
  }

  void addTxPubKey(Transaction &tx, const KeyPair &txKeys)
  {
    addTransactionPublicKeyToExtra(tx.extra, txKeys.publicKey);
  }

  // Build a StandardPaymentOutput to a recipient
  StandardPaymentOutput buildStandardOutput(
      const crypto::PublicKey &txPubKey,
      const crypto::SecretKey &txSecretKey,
      const AccountPublicAddress &recipient,
      size_t outputIndex,
      uint64_t globalIndex,
      const std::vector<uint8_t> &memoData = {})
  {
    StandardPaymentOutput out;

    crypto::KeyDerivation derivation;
    crypto::generate_key_derivation(recipient.viewPublicKey, txSecretKey, derivation);

    out.key_index = static_cast<uint16_t>(outputIndex);
    out.view_tag = BoltSync::computeWalletViewTag(derivation, outputIndex);

    crypto::derive_public_key(derivation, outputIndex, recipient.spendPublicKey, out.key);

    if (!memoData.empty())
    {
      out.memo_size = static_cast<uint8_t>(memoData.size());
      out.encrypted_memo = EncryptedMemo::encrypt(memoData, derivation, globalIndex);
    }
    else
    {
      out.memo_size = 0;
    }

    return out;
  }

} // anonymous namespace

// ───────────────────────────────────────────────────────────────────────────
// Test 1: Serialization round-trip with all new output types
// ───────────────────────────────────────────────────────────────────────────

TEST(Integration, serializeRoundTrip_standardPayment)
{
  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  StandardPaymentOutput out;
  out.view_tag = 0x42;
  out.key_index = 7;
  out.memo_size = 3;
  out.encrypted_memo = {0xAA, 0xBB, 0xCC};
  out.key = makePubKey(0xDD);

  TransactionOutput txOut;
  txOut.amount = 1000000;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  Transaction restored = roundTripTx(tx);

  ASSERT_EQ(restored.outputs.size(), 1u);
  ASSERT_EQ(restored.outputs[0].amount, 1000000u);
  ASSERT_TRUE(restored.outputs[0].target.type() == typeid(StandardPaymentOutput));

  const auto &restoredOut = boost::get<StandardPaymentOutput>(restored.outputs[0].target);
  EXPECT_EQ(restoredOut.view_tag, 0x42);
  EXPECT_EQ(restoredOut.key_index, 7);
  EXPECT_EQ(restoredOut.memo_size, 3);
  EXPECT_EQ(restoredOut.encrypted_memo.size(), 3u);
  EXPECT_EQ(std::memcmp(restoredOut.key.data, out.key.data, 32), 0);
}

TEST(Integration, serializeRoundTrip_multisigPayment)
{
  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  MultisigPaymentOutput out;
  out.view_tag = 0xAB;
  out.key_index = 100;
  out.num_keys = 2;
  out.flags = static_cast<uint8_t>(MultisigFlags::Both);
  out.term = 50000;
  out.memo_size = 0;
  out.keys = {makePubKey(0x11), makePubKey(0x22)};

  TransactionOutput txOut;
  txOut.amount = 5000000;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  Transaction restored = roundTripTx(tx);

  ASSERT_EQ(restored.outputs.size(), 1u);
  ASSERT_TRUE(restored.outputs[0].target.type() == typeid(MultisigPaymentOutput));

  const auto &restoredOut = boost::get<MultisigPaymentOutput>(restored.outputs[0].target);
  EXPECT_EQ(restoredOut.view_tag, 0xAB);
  EXPECT_EQ(restoredOut.key_index, 100);
  EXPECT_EQ(restoredOut.num_keys, 2);
  EXPECT_TRUE(restoredOut.isTimeLocked());
  EXPECT_TRUE(restoredOut.isAuthorized());
  EXPECT_EQ(restoredOut.term, 50000u);
  EXPECT_EQ(restoredOut.keys.size(), 2u);
}

TEST(Integration, serializeRoundTrip_domainRegistration)
{
  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  DomainRegistrationOutput out;
  out.view_tag = 0;
  out.key_index = 0;
  out.domain = "integration.conceal";
  out.domain_len = static_cast<uint8_t>(out.domain.size());
  out.tier = 2;
  out.domain_pub = makePubKey(0x50);
  out.domain_view_pub = makePubKey(0x60);
  out.encrypted_addr_size = 64;
  out.encrypted_addr.fill(0x99);
  out.metadata_len = 5;
  out.metadata = {0x01, 0x02, 0x03, 0x04, 0x05};

  TransactionOutput txOut;
  txOut.amount = 0;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  Transaction restored = roundTripTx(tx);

  ASSERT_EQ(restored.outputs.size(), 1u);
  ASSERT_TRUE(restored.outputs[0].target.type() == typeid(DomainRegistrationOutput));

  const auto &restoredOut = boost::get<DomainRegistrationOutput>(restored.outputs[0].target);
  EXPECT_EQ(restoredOut.domain, "integration.conceal");
  EXPECT_EQ(restoredOut.tier, 2);
  EXPECT_EQ(restoredOut.metadata.size(), 5u);
}

TEST(Integration, serializeRoundTrip_domainDeletion)
{
  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  DomainDeletionOutput out;
  out.view_tag = 0;
  out.key_index = 0;
  out.domain = "remove.conceal";
  out.domain_len = static_cast<uint8_t>(out.domain.size());
  std::memset(out.sig.data, 0x77, sizeof(out.sig.data));

  TransactionOutput txOut;
  txOut.amount = 0;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  Transaction restored = roundTripTx(tx);

  ASSERT_EQ(restored.outputs.size(), 1u);
  ASSERT_TRUE(restored.outputs[0].target.type() == typeid(DomainDeletionOutput));

  const auto &restoredOut = boost::get<DomainDeletionOutput>(restored.outputs[0].target);
  EXPECT_EQ(restoredOut.domain, "remove.conceal");
}

TEST(Integration, serializeRoundTrip_mixedOutputs)
{
  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  // Standard payment
  {
    StandardPaymentOutput out;
    out.view_tag = 0x10;
    out.key_index = 0;
    out.memo_size = 0;
    out.key = makePubKey(0xA1);
    TransactionOutput txOut = {5000000, out};
    tx.outputs.push_back(txOut);
  }

  // Multisig payment
  {
    MultisigPaymentOutput out;
    out.view_tag = 0x20;
    out.key_index = 50;
    out.num_keys = 3;
    out.flags = static_cast<uint8_t>(MultisigFlags::Authorized);
    out.term = 0;
    out.memo_size = 0;
    out.keys = {makePubKey(0xB1), makePubKey(0xB2), makePubKey(0xB3)};
    TransactionOutput txOut = {10000000, out};
    tx.outputs.push_back(txOut);
  }

  // Domain registration
  {
    DomainRegistrationOutput out;
    out.view_tag = 0;
    out.key_index = 0;
    out.domain = "mixed.conceal";
    out.domain_len = static_cast<uint8_t>(out.domain.size());
    out.tier = 1;
    out.domain_pub = makePubKey(0xC1);
    out.domain_view_pub = makePubKey(0xC2);
    out.encrypted_addr_size = 64;
    out.encrypted_addr.fill(0xBB);
    out.metadata_len = 0;
    TransactionOutput txOut = {0, out};
    tx.outputs.push_back(txOut);
  }

  // Domain deletion
  {
    DomainDeletionOutput out;
    out.view_tag = 0;
    out.key_index = 0;
    out.domain = "old.conceal";
    out.domain_len = static_cast<uint8_t>(out.domain.size());
    std::memset(out.sig.data, 0x88, sizeof(out.sig.data));
    TransactionOutput txOut = {0, out};
    tx.outputs.push_back(txOut);
  }

  Transaction restored = roundTripTx(tx);

  ASSERT_EQ(restored.outputs.size(), 4u);

  EXPECT_TRUE(restored.outputs[0].target.type() == typeid(StandardPaymentOutput));
  EXPECT_TRUE(restored.outputs[1].target.type() == typeid(MultisigPaymentOutput));
  EXPECT_TRUE(restored.outputs[2].target.type() == typeid(DomainRegistrationOutput));
  EXPECT_TRUE(restored.outputs[3].target.type() == typeid(DomainDeletionOutput));
}

// ───────────────────────────────────────────────────────────────────────────
// Test 2: Wallet scanning finds owned outputs
// ───────────────────────────────────────────────────────────────────────────

TEST(Integration, walletScanner_findsStandardOutput)
{
  AccountBase wallet;
  wallet.generate();
  const AccountKeys &keys = wallet.getAccountKeys();

  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  StandardPaymentOutput out = buildStandardOutput(
      txKeys.publicKey, txKeys.secretKey, keys.address, 0, 0);

  TransactionOutput txOut;
  txOut.amount = 2000000;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  std::vector<BoltSync::FoundOutput> results;
  std::vector<uint32_t> globalIndexes = {0};

  BoltCore::NewOutputScanner::scanTransaction(
      tx, txKeys.publicKey, globalIndexes, 100,
      keys.viewSecretKey, keys.address.spendPublicKey, &keys.spendSecretKey,
      results);

  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].amount, 2000000u);
  EXPECT_EQ(results[0].blockHeight, 100u);
}

TEST(Integration, walletScanner_skipsForeignOutput)
{
  AccountBase wallet;
  wallet.generate();

  AccountBase foreign;
  foreign.generate();

  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  StandardPaymentOutput out = buildStandardOutput(
      txKeys.publicKey, txKeys.secretKey, foreign.getAccountKeys().address, 0, 0);

  TransactionOutput txOut;
  txOut.amount = 3000000;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  std::vector<BoltSync::FoundOutput> results;
  std::vector<uint32_t> globalIndexes = {0};

  BoltCore::NewOutputScanner::scanTransaction(
      tx, txKeys.publicKey, globalIndexes, 100,
      wallet.getAccountKeys().viewSecretKey,
      wallet.getAccountKeys().address.spendPublicKey,
      nullptr, results);

  EXPECT_TRUE(results.empty());
}

TEST(Integration, walletScanner_encryptedMemoRoundTrip)
{
  AccountBase wallet;
  wallet.generate();
  const AccountKeys &keys = wallet.getAccountKeys();

  std::vector<uint8_t> originalMemo = {
      0x48, 0x65, 0x6C, 0x6C, 0x6F,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xDE, 0xAD, 0xBE, 0xEF};

  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  StandardPaymentOutput out = buildStandardOutput(
      txKeys.publicKey, txKeys.secretKey, keys.address, 0, 42, originalMemo);

  TransactionOutput txOut;
  txOut.amount = 4000000;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  Transaction restored = roundTripTx(tx);
  ASSERT_EQ(restored.outputs.size(), 1u);

  const auto &restoredOut = boost::get<StandardPaymentOutput>(restored.outputs[0].target);

  crypto::KeyDerivation derivation;
  crypto::generate_key_derivation(txKeys.publicKey, keys.viewSecretKey, derivation);

  std::vector<uint8_t> decrypted = EncryptedMemo::decrypt(
      restoredOut.encrypted_memo, derivation, 42);

  EXPECT_EQ(decrypted.size(), originalMemo.size());
  EXPECT_EQ(std::memcmp(decrypted.data(), originalMemo.data(), originalMemo.size()), 0);
}

TEST(Integration, walletScanner_findsMultisigOutput_firstParticipant)
{
  AccountBase wallet1;
  AccountBase wallet2;
  wallet1.generate();
  wallet2.generate();

  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  MultisigPaymentOutput out;
  out.num_keys = 2;
  out.flags = static_cast<uint8_t>(MultisigFlags::TimeLocked);
  out.term = 50000;
  out.memo_size = 0;
  out.key_index = 0;

  // Derive both keys from wallet1's derivation (both keys go to the same wallet for simplicity)
  {
    crypto::KeyDerivation derivation;
    crypto::generate_key_derivation(wallet1.getAccountKeys().address.viewPublicKey, txKeys.secretKey, derivation);

    out.view_tag = BoltSync::computeWalletViewTag(derivation, 0);

    crypto::PublicKey derivedKey;
    crypto::derive_public_key(derivation, 0, wallet1.getAccountKeys().address.spendPublicKey, derivedKey);
    out.keys.push_back(derivedKey);

    crypto::derive_public_key(derivation, 1, wallet1.getAccountKeys().address.spendPublicKey, derivedKey);
    out.keys.push_back(derivedKey);
  }

  TransactionOutput txOut;
  txOut.amount = 10000000;
  txOut.target = out;
  tx.outputs.push_back(txOut);

  std::vector<BoltSync::FoundOutput> results;
  std::vector<uint32_t> globalIndexes = {0};

  BoltCore::NewOutputScanner::scanTransaction(
      tx, txKeys.publicKey, globalIndexes, 200,
      wallet1.getAccountKeys().viewSecretKey,
      wallet1.getAccountKeys().address.spendPublicKey,
      nullptr, results);

  // Should find at least one match (key at position 0 with key_index=0)
  ASSERT_GE(results.size(), 1u);
  EXPECT_TRUE(results[0].isDeposit);
  EXPECT_EQ(results[0].term, 50000u);
}

TEST(Integration, merkleProof_multipleTransactions)
{
  // Use 4 transactions (power of 2) to avoid odd-count hashing differences
  std::vector<crypto::Hash> txHashes;
  for (int i = 0; i < 4; ++i)
  {
    crypto::Hash h;
    std::memset(h.data, static_cast<uint8_t>(i + 1), sizeof(h));
    txHashes.push_back(h);
  }

  crypto::Hash expectedRoot;
  tree_hash(txHashes.data(), txHashes.size(), expectedRoot);

  for (uint32_t i = 0; i < 4; ++i)
  {
    MerkleProof proof = MerkleProof::build(txHashes, i);
    EXPECT_TRUE(proof.valid());
    EXPECT_EQ(std::memcmp(proof.rootHash.data, expectedRoot.data, sizeof(crypto::Hash)), 0);

    bool verified = MerkleProof::verify(txHashes[i], proof);
    EXPECT_TRUE(verified);
  }
}

// ───────────────────────────────────────────────────────────────────────────
// Test 3: Domain index operations
// ───────────────────────────────────────────────────────────────────────────

TEST(Integration, domainIndex_serializeDeserialize_withEntries)
{
  DomainIndex index;

  std::vector<uint8_t> data = index.serialize();

  DomainIndex restored;
  EXPECT_TRUE(restored.deserialize(data));
  EXPECT_EQ(restored.size(), index.size());
}

// ───────────────────────────────────────────────────────────────────────────
// Test 4: Bridge multisig flow
// ───────────────────────────────────────────────────────────────────────────

TEST(Integration, bridge_createAndValidateDeposit)
{
  crypto::PublicKey bridgeKey = makePubKey(0x01);
  crypto::PublicKey userKey = makePubKey(0x02);

  std::vector<uint8_t> htlcData = {0xAA, 0xBB, 0xCC, 0xDD};

  MultisigPaymentOutput deposit = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 10000000, 100000, htlcData);

  EXPECT_TRUE(BridgeMultisigHandler::validateBridgeOutput(deposit));
  EXPECT_TRUE(BridgeMultisigHandler::isBridgeOutput(deposit, bridgeKey));
  EXPECT_TRUE(deposit.isTimeLocked());
  EXPECT_TRUE(deposit.isAuthorized());
  EXPECT_EQ(BridgeMultisigHandler::getExpiryHeight(deposit, 500000), 600000u);
}

TEST(Integration, bridge_flexibleWithdrawal)
{
  crypto::PublicKey bridgeKey = makePubKey(0x10);
  crypto::PublicKey userKey = makePubKey(0x20);

  MultisigPaymentOutput withdrawal = BridgeMultisigHandler::createWithdrawalOutput(
      bridgeKey, userKey, 5000000);

  EXPECT_TRUE(BridgeMultisigHandler::validateBridgeOutput(withdrawal));
  EXPECT_FALSE(withdrawal.isTimeLocked());
  EXPECT_TRUE(withdrawal.isAuthorized());
  EXPECT_EQ(BridgeMultisigHandler::getExpiryHeight(withdrawal, 500000), 0u);
}

TEST(Integration, bridge_serializeRoundTrip)
{
  crypto::PublicKey bridgeKey = makePubKey(0x30);
  crypto::PublicKey userKey = makePubKey(0x31);

  MultisigPaymentOutput original = BridgeMultisigHandler::createBridgeDeposit(
      bridgeKey, userKey, 20000000, 50000, {0x01, 0x02});

  Transaction tx;
  tx.version = TRANSACTION_VERSION_3;
  tx.unlockTime = 0;

  KeyPair txKeys = makeTxKeys();
  addTxPubKey(tx, txKeys);

  TransactionOutput txOut;
  txOut.amount = 20000000;
  txOut.target = original;
  tx.outputs.push_back(txOut);

  Transaction restored = roundTripTx(tx);

  ASSERT_EQ(restored.outputs.size(), 1u);
  ASSERT_TRUE(restored.outputs[0].target.type() == typeid(MultisigPaymentOutput));

  const auto &restoredOut = boost::get<MultisigPaymentOutput>(restored.outputs[0].target);
  EXPECT_TRUE(BridgeMultisigHandler::validateBridgeOutput(restoredOut));
  EXPECT_TRUE(BridgeMultisigHandler::isBridgeOutput(restoredOut, bridgeKey));
  EXPECT_TRUE(restoredOut.isAuthorized());
  EXPECT_TRUE(restoredOut.isTimeLocked());
  EXPECT_EQ(restoredOut.term, 50000u);
}

// ───────────────────────────────────────────────────────────────────────────
// Test 5: Poly1305 authenticated encryption
// ───────────────────────────────────────────────────────────────────────────

TEST(Integration, encryptedMemo_authenticatedRoundTrip)
{
  AccountBase wallet;
  wallet.generate();
  const AccountKeys &keys = wallet.getAccountKeys();

  std::vector<uint8_t> memo = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

  crypto::KeyDerivation derivation;
  crypto::generate_key_derivation(keys.address.viewPublicKey, keys.viewSecretKey, derivation);

  std::vector<uint8_t> authenticated = EncryptedMemo::encryptAuthenticated(memo, derivation, 42);
  EXPECT_EQ(authenticated.size(), memo.size() + Poly1305::TAG_SIZE);

  std::pair<std::vector<uint8_t>, bool> result = EncryptedMemo::decryptAuthenticated(authenticated, derivation, 42);
  EXPECT_TRUE(result.second);
  EXPECT_EQ(result.first.size(), memo.size());
  EXPECT_EQ(std::memcmp(result.first.data(), memo.data(), memo.size()), 0);
}

TEST(Integration, encryptedMemo_authenticatedTampered)
{
  AccountBase wallet;
  wallet.generate();
  const AccountKeys &keys = wallet.getAccountKeys();

  std::vector<uint8_t> memo = {0xAA, 0xBB, 0xCC, 0xDD};

  crypto::KeyDerivation derivation;
  crypto::generate_key_derivation(keys.address.viewPublicKey, keys.viewSecretKey, derivation);

  std::vector<uint8_t> authenticated = EncryptedMemo::encryptAuthenticated(memo, derivation, 99);

  // Tamper with the ciphertext
  authenticated[0] ^= 0xFF;

  std::pair<std::vector<uint8_t>, bool> result = EncryptedMemo::decryptAuthenticated(authenticated, derivation, 99);
  EXPECT_FALSE(result.second);
}

// ───────────────────────────────────────────────────────────────────────────
// Test 6: Merkle proofs for domain resolution
// ───────────────────────────────────────────────────────────────────────────

TEST(Integration, merkleProof_singleTransaction)
{
  std::vector<crypto::Hash> txHashes;
  crypto::Hash coinbaseHash;
  std::memset(coinbaseHash.data, 0x01, sizeof(coinbaseHash));
  txHashes.push_back(coinbaseHash);

  MerkleProof proof = MerkleProof::build(txHashes, 0);
  EXPECT_TRUE(proof.valid());

  bool verified = MerkleProof::verify(coinbaseHash, proof);
  EXPECT_TRUE(verified);
}

TEST(Integration, merkleProof_tamperedHash)
{
  std::vector<crypto::Hash> txHashes;
  for (int i = 0; i < 3; ++i)
  {
    crypto::Hash h;
    std::memset(h.data, static_cast<uint8_t>(i + 1), sizeof(h));
    txHashes.push_back(h);
  }

  MerkleProof proof = MerkleProof::build(txHashes, 1);

  crypto::Hash fakeHash;
  std::memset(fakeHash.data, 0xFF, sizeof(fakeHash));

  bool verified = MerkleProof::verify(fakeHash, proof);
  EXPECT_FALSE(verified);
}