// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "EncryptedMemo.h"
#include "Poly1305.h"

#include <cstring>

#include "crypto/crypto.h"
#include "crypto/chacha8.h"

namespace cn
{

  crypto::SecretKey EncryptedMemo::deriveMemoKey(const crypto::KeyDerivation &derivation)
  {
    // Build preimage: "conceal_memo" || derivation
    const char *domain = "conceal_memo";
    std::vector<uint8_t> preimage;
    preimage.insert(preimage.end(), domain, domain + 12);
    preimage.insert(preimage.end(), derivation.data, derivation.data + 32);

    crypto::Hash hash;
    cn_fast_hash(preimage.data(), preimage.size(), hash);

    crypto::SecretKey key;
    std::memcpy(key.data, hash.data, 32);
    return key;
  }

  std::vector<uint8_t> EncryptedMemo::encrypt(
      const std::vector<uint8_t> &plaintext,
      const crypto::KeyDerivation &derivation,
      uint64_t globalOutputIndex)
  {

    if (plaintext.empty())
    {
      return {};
    }

    crypto::SecretKey secretKey = deriveMemoKey(derivation);

    // Convert SecretKey to chacha8_key
    crypto::chacha8_key key;
    std::memcpy(key.data, secretKey.data, sizeof(key.data));

    // Build 8-byte IV from globalOutputIndex (little-endian)
    crypto::chacha8_iv iv;
    for (int i = 0; i < 8; ++i)
    {
      iv.data[i] = static_cast<uint8_t>((globalOutputIndex >> (i * 8)) & 0xFF);
    }

    std::vector<uint8_t> ciphertext(plaintext.size());

    // chacha8 encrypts in-place: ciphertext = plaintext XOR keystream(key, iv)
    // The function takes void* data and writes to char* cipher.
    // Since we want separate plaintext/ciphertext, copy plaintext to ciphertext first
    // then encrypt in-place.
    std::memcpy(ciphertext.data(), plaintext.data(), plaintext.size());
    crypto::chacha8(ciphertext.data(), ciphertext.size(), key, iv, reinterpret_cast<char *>(ciphertext.data()));

    return ciphertext;
  }

  std::vector<uint8_t> EncryptedMemo::decrypt(
      const std::vector<uint8_t> &ciphertext,
      const crypto::KeyDerivation &derivation,
      uint64_t globalOutputIndex)
  {

    // ChaCha8/XOR is symmetric: decryption = encryption
    return encrypt(ciphertext, derivation, globalOutputIndex);
  }

  std::vector<uint8_t> EncryptedMemo::encryptAuthenticated(
      const std::vector<uint8_t> &plaintext,
      const crypto::KeyDerivation &derivation,
      uint64_t globalOutputIndex)
  {
    if (plaintext.empty())
      return {};

    crypto::SecretKey secretKey = deriveMemoKey(derivation);

    // Derive Poly1305 one-time key: H("conceal_memo_auth" || derivation || varint(globalIndex))
    const char *domain = "conceal_memo_auth";
    std::vector<uint8_t> polyKeyPreimage;
    polyKeyPreimage.insert(polyKeyPreimage.end(), domain, domain + 16);
    polyKeyPreimage.insert(polyKeyPreimage.end(), derivation.data, derivation.data + 32);

    uint64_t idx = globalOutputIndex;
    while (idx >= 0x80)
    {
      polyKeyPreimage.push_back(static_cast<uint8_t>(idx | 0x80));
      idx >>= 7;
    }
    polyKeyPreimage.push_back(static_cast<uint8_t>(idx));

    crypto::Hash polyKeyHash;
    cn_fast_hash(polyKeyPreimage.data(), polyKeyPreimage.size(), polyKeyHash);

    uint8_t polyKey[Poly1305::KEY_SIZE];
    std::memcpy(polyKey, polyKeyHash.data, Poly1305::KEY_SIZE);

    // Encrypt with ChaCha8
    crypto::chacha8_key key;
    std::memcpy(key.data, secretKey.data, sizeof(key.data));

    crypto::chacha8_iv iv;
    for (int i = 0; i < 8; ++i)
      iv.data[i] = static_cast<uint8_t>((globalOutputIndex >> (i * 8)) & 0xFF);

    std::vector<uint8_t> ciphertext(plaintext.size());
    std::memcpy(ciphertext.data(), plaintext.data(), plaintext.size());
    crypto::chacha8(ciphertext.data(), ciphertext.size(), key, iv,
                    reinterpret_cast<char *>(ciphertext.data()));

    // Compute Poly1305 tag over ciphertext
    std::vector<uint8_t> tag = Poly1305::mac(polyKey, ciphertext);

    // Append tag to ciphertext
    ciphertext.insert(ciphertext.end(), tag.begin(), tag.end());
    return ciphertext;
  }

  std::pair<std::vector<uint8_t>, bool> EncryptedMemo::decryptAuthenticated(
      const std::vector<uint8_t> &ciphertextWithTag,
      const crypto::KeyDerivation &derivation,
      uint64_t globalOutputIndex)
  {
    if (ciphertextWithTag.size() < Poly1305::TAG_SIZE)
      return std::make_pair(std::vector<uint8_t>(), false);

    // Split ciphertext and tag
    size_t ciphertextLen = ciphertextWithTag.size() - Poly1305::TAG_SIZE;
    std::vector<uint8_t> ciphertext(ciphertextWithTag.begin(),
                                    ciphertextWithTag.begin() + ciphertextLen);

    // Derive Poly1305 one-time key (same as encrypt)
    const char *domain = "conceal_memo_auth";
    std::vector<uint8_t> polyKeyPreimage;
    polyKeyPreimage.insert(polyKeyPreimage.end(), domain, domain + 16);
    polyKeyPreimage.insert(polyKeyPreimage.end(), derivation.data, derivation.data + 32);

    uint64_t idx = globalOutputIndex;
    while (idx >= 0x80)
    {
      polyKeyPreimage.push_back(static_cast<uint8_t>(idx | 0x80));
      idx >>= 7;
    }
    polyKeyPreimage.push_back(static_cast<uint8_t>(idx));

    crypto::Hash polyKeyHash;
    cn_fast_hash(polyKeyPreimage.data(), polyKeyPreimage.size(), polyKeyHash);

    uint8_t polyKey[Poly1305::KEY_SIZE];
    std::memcpy(polyKey, polyKeyHash.data, Poly1305::KEY_SIZE);

    // Verify tag
    const uint8_t *tag = ciphertextWithTag.data() + ciphertextLen;
    if (!Poly1305::verify(polyKey, ciphertext, tag))
      return std::make_pair(std::vector<uint8_t>(), false);

    // Decrypt
    std::vector<uint8_t> plaintext = decrypt(ciphertext, derivation, globalOutputIndex);
    return std::make_pair(plaintext, true);
  }

} // namespace cn