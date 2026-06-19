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
    bool hasGlobalOutputIndex = false;
    // Derivation index for generate_key_image_helper (StandardPaymentOutput key_index).
    // When unset, legacy KeyOutput uses outputIndex.
    uint32_t keyDerivationIndex = 0;
    bool hasKeyDerivationIndex = false;
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
    std::vector<OutputInfo> spentInputs;
  };

  struct Balance
  {
    uint64_t actual;
    uint64_t pending;
    uint64_t lockedDeposit;
    uint64_t unlockedDeposit;
    uint64_t accruedInterest; // Unused; use Wallet::getAccruedInterest() for display
    uint64_t dust;            // Un-mixable outputs below the dust threshold
    uint32_t currentHeight;   // Current blockchain height for this balance
  };

  // Key outputs available before deducting a tx fee (actual minus dust).
  // Callers subtract the fee for their operation (send/deposit: MINIMUM_FEE_V2, withdraw: MINIMUM_FEE, …).
  inline uint64_t spendableAmountBeforeFee(const Balance &balance)
  {
    return balance.actual > balance.dust ? balance.actual - balance.dust : 0;
  }

  struct DepositInfo
  {
    uint64_t id;
    uint64_t amount;
    uint64_t interest;
    uint32_t term;
    uint32_t outputIndex;
    uint32_t unlockHeight;
    bool locked;
    std::string creatingTxHash;
    std::string spendingTxHash;
  };

  struct FusionEstimate
  {
    size_t fusionReadyCount = 0;   // total outputs across buckets meeting min (aggregate)
    size_t totalOutputCount = 0;
    size_t readyBucketCount = 0;   // buckets with >= fusionTxMinInputCount outputs
    size_t largestReadyBucket = 0;   // max outputs in one ready bucket (one tx uses one bucket)
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