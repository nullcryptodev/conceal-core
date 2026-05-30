// BoltCoreTypes - shared types for the BoltCore library
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "crypto/crypto.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"

namespace BoltCore
{
  struct OutputInfo
  {
    uint32_t blockHeight;
    crypto::Hash txHash;
    uint32_t outputIndex;
    uint32_t globalOutputIndex;
    uint64_t amount;
    crypto::PublicKey outputKey;
    crypto::PublicKey txPublicKey;
    crypto::KeyImage keyImage;
    bool spent;
    bool isDeposit;
    uint32_t term;
    std::string subAddress;
  };

  struct Transfer
  {
    std::string address;
    uint64_t amount;
  };

  struct TransferResult
  {
    bool success;
    std::string txHash;
    uint64_t fee;
    std::string error;
    cn::Transaction transaction;
  };

  struct Balance
  {
    uint64_t actual;
    uint64_t pending;
    uint64_t lockedDeposit;
    uint64_t unlockedDeposit;
    uint64_t accruedInterest; // Interest earned on deposits not yet withdrawn
    uint32_t currentHeight;   // Current blockchain height for this balance
  };

  struct DepositInfo
  {
    uint64_t id;
    uint64_t amount;
    uint64_t interest;
    uint32_t term;
    uint32_t unlockHeight;
    bool locked;
    std::string creatingTxHash;
    std::string spendingTxHash;
  };

  struct FusionEstimate
  {
    size_t fusionReadyCount;
    size_t totalOutputCount;
  };

  struct SubAddress
  {
    std::string address;
    crypto::PublicKey spendPublicKey;
    crypto::SecretKey spendSecretKey;
  };

  enum class TransactionType : uint8_t
  {
    Incoming,
    Outgoing,
    Deposit,
    Withdrawal,
    Fusion
  };

  struct TransactionRecord
  {
    crypto::Hash txHash;
    uint32_t blockHeight = 0;
    uint64_t timestamp = 0;
    uint64_t fee = 0;
    uint64_t totalSent = 0;
    uint64_t totalReceived = 0;
    TransactionType type = TransactionType::Incoming;
    bool confirmed = false;
    std::string paymentId;
    std::string address;             // Destination for outgoing, empty for incoming
    std::vector<OutputInfo> outputs; // Outputs involved in this tx
  };

  enum class WalletType
  {
    Full,
    ViewOnly
  };
}