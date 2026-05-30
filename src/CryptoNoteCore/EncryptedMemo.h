// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "CryptoTypes.h"

namespace cn
{

    //
    // EncryptedMemo handles encryption and decryption of the memo field
    // in StandardPaymentOutput and MultisigPaymentOutput.
    //
    // Encryption uses ChaCha8 with:
    //   key = H("conceal_memo" || derivation)
    //   nonce = global_output_index (8 bytes)
    //
    // Authenticated encryption appends a Poly1305 tag.
    //

    class EncryptedMemo
    {
    public:
        //
        // Derive the memo encryption key from a shared secret
        // key = H("conceal_memo" || derivation)
        //
        static crypto::SecretKey deriveMemoKey(const crypto::KeyDerivation &derivation);

        //
        // Encrypt a plaintext memo (unauthenticated)
        //
        static std::vector<uint8_t> encrypt(
            const std::vector<uint8_t> &plaintext,
            const crypto::KeyDerivation &derivation,
            uint64_t globalOutputIndex);

        //
        // Decrypt a ciphertext memo (unauthenticated)
        //
        static std::vector<uint8_t> decrypt(
            const std::vector<uint8_t> &ciphertext,
            const crypto::KeyDerivation &derivation,
            uint64_t globalOutputIndex);

        //
        // Encrypt with Poly1305 authentication tag appended.
        // Format: ciphertext[0..N-1] || tag[0..15]
        // Total size = plaintext.size() + 16
        //
        static std::vector<uint8_t> encryptAuthenticated(
            const std::vector<uint8_t> &plaintext,
            const crypto::KeyDerivation &derivation,
            uint64_t globalOutputIndex);

        //
        // Decrypt and verify Poly1305 authentication tag.
        // @return Pair of (plaintext, is_valid). If is_valid is false, the
        //         plaintext may be corrupted or tampered with.
        //
        static std::pair<std::vector<uint8_t>, bool> decryptAuthenticated(
            const std::vector<uint8_t> &ciphertextWithTag,
            const crypto::KeyDerivation &derivation,
            uint64_t globalOutputIndex);
    };

} // namespace cn