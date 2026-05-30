// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "gtest/gtest.h"

#include <cstring>
#include <vector>

#include "CryptoNoteCore/EncryptedMemo.h"
#include "crypto/crypto.h"

using namespace cn;

namespace
{

  crypto::KeyDerivation makeDummyDerivation(uint8_t fill)
  {
    crypto::KeyDerivation d;
    std::memset(d.data, fill, sizeof(d.data));
    return d;
  }

} // anonymous namespace

TEST(EncryptedMemo, emptyPlaintext)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0xAA);

  std::vector<uint8_t> plaintext;
  std::vector<uint8_t> ciphertext = EncryptedMemo::encrypt(plaintext, derivation, 0);

  // Empty plaintext should produce empty ciphertext
  EXPECT_TRUE(ciphertext.empty());
}

TEST(EncryptedMemo, encryptDecrypt_roundTrip)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0xBB);
  std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

  std::vector<uint8_t> ciphertext = EncryptedMemo::encrypt(plaintext, derivation, 0);

  // Ciphertext should be same length as plaintext (ChaCha20/XOR)
  EXPECT_EQ(ciphertext.size(), plaintext.size());

  // Ciphertext should be different from plaintext (unless keystream is all zeros)
  EXPECT_NE(std::memcmp(ciphertext.data(), plaintext.data(), plaintext.size()), 0);

  std::vector<uint8_t> decrypted = EncryptedMemo::decrypt(ciphertext, derivation, 0);

  // Decrypted should match original plaintext
  EXPECT_EQ(decrypted.size(), plaintext.size());
  EXPECT_EQ(std::memcmp(decrypted.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(EncryptedMemo, differentDerivationDifferentCiphertext)
{
  crypto::KeyDerivation d1 = makeDummyDerivation(0x11);
  crypto::KeyDerivation d2 = makeDummyDerivation(0x22);
  std::vector<uint8_t> plaintext = {0xDE, 0xAD, 0xBE, 0xEF};

  std::vector<uint8_t> c1 = EncryptedMemo::encrypt(plaintext, d1, 0);
  std::vector<uint8_t> c2 = EncryptedMemo::encrypt(plaintext, d2, 0);

  // Same plaintext, different derivations -> different ciphertext
  EXPECT_EQ(c1.size(), c2.size());
  EXPECT_NE(std::memcmp(c1.data(), c2.data(), c1.size()), 0);

  // Each decrypts correctly with its own derivation
  std::vector<uint8_t> p1 = EncryptedMemo::decrypt(c1, d1, 0);
  std::vector<uint8_t> p2 = EncryptedMemo::decrypt(c2, d2, 0);
  EXPECT_EQ(std::memcmp(p1.data(), plaintext.data(), plaintext.size()), 0);
  EXPECT_EQ(std::memcmp(p2.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(EncryptedMemo, differentIndexDifferentCiphertext)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0x33);
  std::vector<uint8_t> plaintext = {0xCA, 0xFE, 0xBA, 0xBE};

  std::vector<uint8_t> c1 = EncryptedMemo::encrypt(plaintext, derivation, 0);
  std::vector<uint8_t> c2 = EncryptedMemo::encrypt(plaintext, derivation, 1);

  // Same derivation, different indices -> different ciphertext
  EXPECT_EQ(c1.size(), c2.size());
  EXPECT_NE(std::memcmp(c1.data(), c2.data(), c1.size()), 0);

  // Each decrypts correctly with its own index
  std::vector<uint8_t> p1 = EncryptedMemo::decrypt(c1, derivation, 0);
  std::vector<uint8_t> p2 = EncryptedMemo::decrypt(c2, derivation, 1);
  EXPECT_EQ(std::memcmp(p1.data(), plaintext.data(), plaintext.size()), 0);
  EXPECT_EQ(std::memcmp(p2.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(EncryptedMemo, decryptWrongDerivation)
{
  crypto::KeyDerivation dGood = makeDummyDerivation(0x44);
  crypto::KeyDerivation dBad = makeDummyDerivation(0x55);
  std::vector<uint8_t> plaintext = {0x01, 0x02, 0x03, 0x04};

  std::vector<uint8_t> ciphertext = EncryptedMemo::encrypt(plaintext, dGood, 0);
  std::vector<uint8_t> decrypted = EncryptedMemo::decrypt(ciphertext, dBad, 0);

  // Decrypting with wrong derivation should NOT produce the plaintext
  EXPECT_NE(std::memcmp(decrypted.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(EncryptedMemo, decryptWrongIndex)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0x66);
  std::vector<uint8_t> plaintext = {0xFF, 0xEE, 0xDD, 0xCC};

  std::vector<uint8_t> ciphertext = EncryptedMemo::encrypt(plaintext, derivation, 10);
  std::vector<uint8_t> decrypted = EncryptedMemo::decrypt(ciphertext, derivation, 20);

  // Decrypting with wrong index should NOT produce the plaintext
  EXPECT_NE(std::memcmp(decrypted.data(), plaintext.data(), plaintext.size()), 0);
}

TEST(EncryptedMemo, deriveMemoKey_deterministic)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0x77);

  crypto::SecretKey key1 = EncryptedMemo::deriveMemoKey(derivation);
  crypto::SecretKey key2 = EncryptedMemo::deriveMemoKey(derivation);

  // Same derivation should always produce the same key
  EXPECT_EQ(std::memcmp(key1.data, key2.data, 32), 0);
}

TEST(EncryptedMemo, deriveMemoKey_differentDerivations)
{
  crypto::KeyDerivation d1 = makeDummyDerivation(0x88);
  crypto::KeyDerivation d2 = makeDummyDerivation(0x99);

  crypto::SecretKey key1 = EncryptedMemo::deriveMemoKey(d1);
  crypto::SecretKey key2 = EncryptedMemo::deriveMemoKey(d2);

  // Different derivations should produce different keys
  EXPECT_NE(std::memcmp(key1.data, key2.data, 32), 0);
}

TEST(EncryptedMemo, largePlaintext)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0xAA);

  // 255 bytes - the maximum memo size
  std::vector<uint8_t> plaintext(255);
  for (size_t i = 0; i < 255; ++i)
  {
    plaintext[i] = static_cast<uint8_t>(i);
  }

  std::vector<uint8_t> ciphertext = EncryptedMemo::encrypt(plaintext, derivation, 42);
  EXPECT_EQ(ciphertext.size(), 255);

  std::vector<uint8_t> decrypted = EncryptedMemo::decrypt(ciphertext, derivation, 42);
  EXPECT_EQ(decrypted.size(), 255);
  EXPECT_EQ(std::memcmp(decrypted.data(), plaintext.data(), 255), 0);
}

TEST(EncryptedMemo, authenticated_roundTrip)
{
  crypto::KeyDerivation derivation = makeDummyDerivation(0xBB);
  std::vector<uint8_t> plaintext = {0x10, 0x20, 0x30, 0x40};

  std::vector<uint8_t> ciphertextWithTag = EncryptedMemo::encryptAuthenticated(plaintext, derivation, 7);
  EXPECT_FALSE(ciphertextWithTag.empty());

  auto [decrypted, isValid] = EncryptedMemo::decryptAuthenticated(ciphertextWithTag, derivation, 7);
  EXPECT_TRUE(isValid);
  EXPECT_EQ(decrypted.size(), plaintext.size());
  EXPECT_EQ(std::memcmp(decrypted.data(), plaintext.data(), plaintext.size()), 0);
}