// Copyright (c) 2012-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2023 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace crypto {

struct Hash {
  uint8_t data[32];
};

struct EllipticCurvePoint
{
  uint8_t data[32];
};

struct EllipticCurveScalar
{
  uint8_t data[32];
};

struct PublicKey : public EllipticCurvePoint
{
};

struct SecretKey : public EllipticCurveScalar
{
};

struct KeyDerivation {
  uint8_t data[32];
};

struct KeyImage {
  uint8_t data[32];
};

struct Signature {
  uint8_t data[64];
};

const struct EllipticCurveScalar I = {{0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

}

namespace cn {

struct TransactionOutputStandardPaymentDetails {
  crypto::PublicKey txOutKey;
  uint8_t viewTag;
  uint16_t keyIndex;
  uint8_t memoSize;
  std::vector<uint8_t> encryptedMemo;
};

struct TransactionOutputMultisigPaymentDetails {
  std::vector<crypto::PublicKey> keys;
  uint8_t requiredSignatures;
  uint32_t term;
  uint8_t flags;
  uint8_t viewTag;
  uint16_t keyIndex;
  uint8_t memoSize;
  std::vector<uint8_t> encryptedMemo;
};

struct TransactionOutputDomainRegistrationDetails {
  std::string domain;
  uint8_t tier;
  crypto::PublicKey domainPub;
  crypto::PublicKey domainViewPub;
  std::vector<uint8_t> encryptedAddr;
  std::vector<uint8_t> metadata;
};

struct TransactionOutputDomainDeletionDetails {
  std::string domain;
  crypto::Signature sig;
};

}

