// NullNode - stub INode for offline wallet mode
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "INode.h"

namespace ClientWallet
{

  class NullNode : public cn::INode
  {
  public:
    NullNode() = default;

    bool addObserver(cn::INodeObserver *) override { return true; }
    bool removeObserver(cn::INodeObserver *) override { return true; }
    void init(const Callback &cb) override { cb(std::error_code()); }
    bool shutdown() override { return true; }

    size_t getPeerCount() const override { return 0; }
    uint32_t getLastLocalBlockHeight() const override { return 0; }
    uint32_t getLastKnownBlockHeight() const override { return 0; }
    uint32_t getLocalBlockCount() const override { return 0; }
    uint32_t getKnownBlockCount() const override { return 0; }
    uint64_t getLastLocalBlockTimestamp() const override { return 0; }

    void relayTransaction(const cn::Transaction &, const Callback &cb) override
    {
      cb(std::make_error_code(std::errc::not_connected));
    }
    void getRandomOutsByAmounts(std::vector<uint64_t> &&, uint64_t,
                                std::vector<cn::COMMAND_RPC_GET_RANDOM_OUTPUTS_FOR_AMOUNTS::outs_for_amount> &,
                                const Callback &cb) override
    {
      cb(std::make_error_code(std::errc::not_connected));
    }
    void getNewBlocks(std::vector<crypto::Hash> &&, std::vector<cn::block_complete_entry> &,
                      uint32_t &, const Callback &cb) override { cb(std::error_code()); }
    void getTransactionOutsGlobalIndices(const crypto::Hash &, std::vector<uint32_t> &,
                                         const Callback &cb) override { cb(std::error_code()); }
    void queryBlocks(std::vector<crypto::Hash> &&, uint64_t, std::vector<cn::BlockShortEntry> &,
                     uint32_t &, const Callback &cb) override { cb(std::error_code()); }
    void getPoolSymmetricDifference(std::vector<crypto::Hash> &&, crypto::Hash, bool &,
                                    std::vector<std::unique_ptr<cn::ITransactionReader>> &,
                                    std::vector<crypto::Hash> &, const Callback &cb) override
    {
      cb(std::error_code());
    }
    void getMultisignatureOutputByGlobalIndex(uint64_t, uint32_t, cn::MultisignatureOutput &,
                                              const Callback &cb) override { cb(std::error_code()); }
    void getTransaction(const crypto::Hash &, cn::Transaction &, const Callback &cb) override
    {
      cb(std::error_code());
    }
    void getBlocks(const std::vector<uint32_t> &, std::vector<std::vector<cn::BlockDetails>> &,
                   const Callback &cb) override { cb(std::error_code()); }
    void getBlocks(const std::vector<crypto::Hash> &, std::vector<cn::BlockDetails> &,
                   const Callback &cb) override { cb(std::error_code()); }
    void getBlocks(uint64_t, uint64_t, uint32_t, std::vector<cn::BlockDetails> &, uint32_t &,
                   const Callback &cb) override { cb(std::error_code()); }
    void getTransactions(const std::vector<crypto::Hash> &, std::vector<cn::TransactionDetails> &,
                         const Callback &cb) override { cb(std::error_code()); }
    void getTransactionsByPaymentId(const crypto::Hash &, std::vector<cn::TransactionDetails> &,
                                    const Callback &cb) override { cb(std::error_code()); }
    void getPoolTransactions(uint64_t, uint64_t, uint32_t, std::vector<cn::TransactionDetails> &,
                             uint64_t &, const Callback &cb) override { cb(std::error_code()); }
    void isSynchronized(bool &sync, const Callback &cb) override
    {
      sync = true;
      cb(std::error_code());
    }
    std::vector<crypto::Hash> getPoolTransactions() override { return {}; }
    bool getTransactionSync(const crypto::Hash &, cn::Transaction &) override { return false; }
  };

} // namespace ClientWallet