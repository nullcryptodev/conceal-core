// BoltCore - transaction engine for Conceal wallets
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "BalanceTracker.h"
#include "INode.h"
#include <functional>
#include <string>
#include <vector>

namespace cn
{
  class Currency;
}

namespace BoltCore
{
  class Wallet
  {
  public:
    Wallet(const crypto::SecretKey &viewKey,
           const crypto::SecretKey &spendKey,
           const crypto::PublicKey &viewPub,
           const crypto::PublicKey &spendPub,
           cn::INode &node,
           const cn::Currency &currency);

    ~Wallet();

    // Load outputs from BoltSync scan or other source
    void loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight = 0);
    void addOutput(const OutputInfo &output);

    void setCurrentHeight(uint32_t height);
    uint32_t getCurrentHeight() const;

    // Balances
    Balance getBalance() const;
    Balance getBalance(const std::string &address) const;

    // Send
    TransferResult transfer(const std::string &address, uint64_t amount, uint64_t mixin = 0);
    TransferResult transfer(const std::vector<Transfer> &transfers, uint64_t mixin = 0);

    // Deposits
    TransferResult createDeposit(uint64_t amount, uint32_t term, const std::string &sourceAddress = "");
    TransferResult withdrawDeposit(uint64_t depositId);
    std::vector<DepositInfo> getDeposits() const;
    uint64_t calculateDepositInterest(const OutputInfo &deposit) const;

    // Fusion
    FusionEstimate estimateFusion(uint64_t threshold) const;
    TransferResult createFusion(uint64_t threshold, uint64_t mixin);

    // Sub-addresses
    SubAddress generateSubAddress();
    std::vector<SubAddress> getSubAddresses() const;
    std::string getMainAddress() const;

    // Transaction history
    std::vector<OutputInfo> getOutputs() const;
    std::vector<OutputInfo> getUnspentOutputs() const;
    std::vector<OutputInfo> getTransactions(uint32_t startHeight = 0, uint32_t limit = 100) const;

    // Wallet info
    WalletType getType() const;
    crypto::PublicKey getViewPublicKey() const;
    crypto::PublicKey getSpendPublicKey() const;

    // Pending
    void addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee);
    void confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight);
    uint64_t getPendingOutgoingAmount() const;
    std::vector<BalanceTracker::PendingTx> getPendingTransactions() const;

    void addTransaction(const TransactionRecord &tx);
    std::vector<TransactionRecord> getTransactionHistory(uint32_t offset = 0, uint32_t limit = 50) const;
    uint32_t getTransactionCount() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };
}