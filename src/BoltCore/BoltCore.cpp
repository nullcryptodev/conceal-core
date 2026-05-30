// BoltCore - Wallet implementation tying all modules together
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "BoltCore.h"
#include "BalanceTracker.h"
#include "OutputSelector.h"
#include "SignatureBuilder.h"
#include "TransactionBuilder.h"
#include "RelayHandler.h"
#include "DepositManager.h"
#include "FusionManager.h"
#include "SubAddressManager.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Common/StringTools.h"
#include <algorithm>
#include <ctime>

namespace BoltCore
{
  struct Wallet::Impl
  {
    crypto::SecretKey viewKey;
    crypto::SecretKey spendKey;
    crypto::PublicKey viewPub;
    crypto::PublicKey spendPub;
    cn::INode &node;
    const cn::Currency &currency;
    WalletType type;

    BalanceTracker balanceTracker;
    OutputSelector outputSelector;
    SignatureBuilder signatureBuilder;
    TransactionBuilder transactionBuilder;
    RelayHandler relayHandler;
    DepositManager depositManager;
    FusionManager fusionManager;
    SubAddressManager subAddressManager;

    cn::AccountPublicAddress mainAddress;
    std::vector<DepositInfo> deposits;

    Impl(const crypto::SecretKey &vk, const crypto::SecretKey &sk,
         const crypto::PublicKey &vp, const crypto::PublicKey &sp,
         cn::INode &n, const cn::Currency &c)
        : viewKey(vk), spendKey(sk), viewPub(vp), spendPub(sp),
          node(n), currency(c),
          type(sk == crypto::SecretKey() ? WalletType::ViewOnly : WalletType::Full),
          balanceTracker(),
          outputSelector(c),
          signatureBuilder(vk, sk, vp, sp),
          transactionBuilder(c, outputSelector, signatureBuilder),
          relayHandler(n),
          depositManager(c, transactionBuilder, signatureBuilder, outputSelector, relayHandler,
                         {sp, vp}),
          fusionManager(c, outputSelector, transactionBuilder, signatureBuilder, relayHandler),
          subAddressManager(c, vp, vk, sk),
          mainAddress{sp, vp}
    {
    }
  };

  Wallet::Wallet(const crypto::SecretKey &viewKey,
                 const crypto::SecretKey &spendKey,
                 const crypto::PublicKey &viewPub,
                 const crypto::PublicKey &spendPub,
                 cn::INode &node,
                 const cn::Currency &currency)
      : m_impl(new Impl(viewKey, spendKey, viewPub, spendPub, node, currency)) {}

  Wallet::~Wallet() = default;

  void Wallet::loadOutputs(const std::vector<OutputInfo> &outputs, uint32_t currentHeight)
  {
    m_impl->balanceTracker.loadOutputs(outputs, currentHeight);
  }

  void Wallet::addOutput(const OutputInfo &output)
  {
    m_impl->balanceTracker.addOutput(output);
  }

  void Wallet::setCurrentHeight(uint32_t height)
  {
    m_impl->balanceTracker.setCurrentHeight(height);
  }

  uint32_t Wallet::getCurrentHeight() const
  {
    return m_impl->balanceTracker.getCurrentHeight();
  }

  uint64_t Wallet::calculateDepositInterest(const OutputInfo &deposit) const
  {
    if (!deposit.isDeposit || deposit.term == 0)
      return 0;
    return m_impl->currency.calculateInterest(deposit.amount, deposit.term, deposit.blockHeight);
  }

  Balance Wallet::getBalance() const
  {
    return m_impl->balanceTracker.getTotalBalance();
  }

  Balance Wallet::getBalance(const std::string &address) const
  {
    return m_impl->balanceTracker.getBalance(address);
  }

  TransferResult Wallet::transfer(const std::string &address, uint64_t amount, uint64_t mixin)
  {
    return transfer({{address, amount}}, mixin);
  }

  TransferResult Wallet::transfer(const std::vector<Transfer> &transfers, uint64_t mixin)
  {
    TransferResult result;
    result.success = false;

    if (m_impl->type == WalletType::ViewOnly)
    {
      result.error = "Cannot send from view-only wallet";
      return result;
    }

    BuilderParams params;
    params.transfers = transfers;
    params.fee = m_impl->currency.minimumFee();
    params.mixin = mixin;
    params.unlockTime = 0;
    params.ttl = 0;
    params.changeAddress = m_impl->mainAddress;
    params.donationAddress = m_impl->mainAddress;
    params.donationThreshold = 0;
    params.mainAddress = m_impl->mainAddress;

    auto buildResult = m_impl->transactionBuilder.build(
        getUnspentOutputs(), params);

    if (!buildResult.success)
    {
      result.error = buildResult.error;
      return result;
    }

    // Sign inputs
    cn::Transaction tx;
    cn::BinaryArray txData = buildResult.transaction->getTransactionData();
    if (!cn::fromBinaryArray(tx, txData))
    {
      result.error = "Failed to serialize transaction";
      return result;
    }

    // Relay
    auto relayResult = m_impl->relayHandler.relaySync(tx);
    result.success = relayResult.success;
    result.error = relayResult.error;
    result.fee = buildResult.fee;

    if (result.success)
    {
      result.transaction = tx;
      result.txHash = common::podToHex(getObjectHash(tx));

      // Record outgoing transaction
      TransactionRecord record;
      record.txHash = getObjectHash(tx);
      record.timestamp = static_cast<uint64_t>(std::time(nullptr));
      record.fee = buildResult.fee;
      uint64_t totalAmount = 0;
      for (const auto &t : transfers)
        totalAmount += t.amount;
      record.totalSent = totalAmount + buildResult.fee;
      record.totalReceived = 0;
      record.type = TransactionType::Outgoing;
      record.confirmed = false;
      if (!transfers.empty())
        record.address = transfers[0].address;
      m_impl->balanceTracker.addTransaction(record);

      // Track pending outgoing
      m_impl->balanceTracker.addPendingOutgoing(record.txHash, totalAmount, buildResult.fee);
    }

    return result;
  }

  TransferResult Wallet::createDeposit(uint64_t amount, uint32_t term,
                                       const std::string &sourceAddress)
  {
    TransferResult result;
    result.success = false;

    if (m_impl->type == WalletType::ViewOnly)
    {
      result.error = "Cannot create deposit from view-only wallet";
      return result;
    }

    auto depositResult = m_impl->depositManager.createDeposit(
        amount, term, getUnspentOutputs());

    if (!depositResult.success)
    {
      result.error = depositResult.error;
      return result;
    }

    result.success = depositResult.success;
    result.txHash = depositResult.txHash;
    result.fee = depositResult.fee;
    result.transaction = depositResult.transaction;

    if (result.success)
    {
      // Record deposit transaction
      TransactionRecord record;
      if (common::podFromHex(result.txHash, record.txHash))
      {
        record.timestamp = static_cast<uint64_t>(std::time(nullptr));
        record.fee = depositResult.fee;
        record.totalSent = amount + depositResult.fee;
        record.totalReceived = 0;
        record.type = TransactionType::Deposit;
        record.confirmed = false;
        m_impl->balanceTracker.addTransaction(record);
        m_impl->balanceTracker.addPendingOutgoing(record.txHash, amount, depositResult.fee);
      }
    }

    return result;
  }

  TransferResult Wallet::withdrawDeposit(uint64_t depositId)
  {
    TransferResult result;
    result.success = false;

    if (m_impl->type == WalletType::ViewOnly)
    {
      result.error = "Cannot withdraw deposit from view-only wallet";
      return result;
    }

    auto withdrawResult = m_impl->depositManager.withdrawDeposit(
        depositId, getUnspentOutputs(), m_impl->deposits);

    result.success = withdrawResult.success;
    result.txHash = withdrawResult.txHash;
    result.fee = withdrawResult.fee;
    result.error = withdrawResult.error;

    if (result.success)
    {
      TransactionRecord record;
      if (common::podFromHex(result.txHash, record.txHash))
      {
        record.timestamp = static_cast<uint64_t>(std::time(nullptr));
        record.fee = withdrawResult.fee;
        record.totalSent = 0;
        record.totalReceived = 0;
        record.type = TransactionType::Withdrawal;
        record.confirmed = false;
        m_impl->balanceTracker.addTransaction(record);
      }
    }

    return result;
  }

  std::vector<DepositInfo> Wallet::getDeposits() const
  {
    return m_impl->deposits;
  }

  FusionEstimate Wallet::estimateFusion(uint64_t threshold) const
  {
    return m_impl->fusionManager.estimate(threshold, getUnspentOutputs());
  }

  TransferResult Wallet::createFusion(uint64_t threshold, uint64_t mixin)
  {
    TransferResult result;
    result.success = false;

    if (m_impl->type == WalletType::ViewOnly)
    {
      result.error = "Cannot create fusion from view-only wallet";
      return result;
    }

    auto fusionResult = m_impl->fusionManager.createFusion(
        threshold, mixin, getUnspentOutputs(), m_impl->mainAddress);

    result.success = fusionResult.success;
    result.txHash = fusionResult.txHash;
    result.fee = fusionResult.fee;
    result.error = fusionResult.error;

    if (result.success)
    {
      TransactionRecord record;
      if (common::podFromHex(result.txHash, record.txHash))
      {
        record.timestamp = static_cast<uint64_t>(std::time(nullptr));
        record.fee = fusionResult.fee;
        record.totalSent = fusionResult.fee;
        record.totalReceived = 0;
        record.type = TransactionType::Fusion;
        record.confirmed = false;
        m_impl->balanceTracker.addTransaction(record);
      }
    }

    return result;
  }

  SubAddress Wallet::generateSubAddress()
  {
    return m_impl->subAddressManager.generate();
  }

  std::vector<SubAddress> Wallet::getSubAddresses() const
  {
    return m_impl->subAddressManager.getAll();
  }

  std::string Wallet::getMainAddress() const
  {
    return m_impl->subAddressManager.getMainAddress();
  }

  std::vector<OutputInfo> Wallet::getOutputs() const
  {
    return m_impl->balanceTracker.getOutputs();
  }

  std::vector<OutputInfo> Wallet::getUnspentOutputs() const
  {
    std::vector<OutputInfo> unspent;
    for (const auto &out : getOutputs())
    {
      if (!out.spent)
        unspent.push_back(out);
    }
    return unspent;
  }

  std::vector<OutputInfo> Wallet::getTransactions(uint32_t startHeight, uint32_t limit) const
  {
    auto all = m_impl->balanceTracker.getOutputs();
    std::vector<OutputInfo> result;
    for (const auto &out : all)
    {
      if (out.blockHeight >= startHeight && result.size() < limit)
        result.push_back(out);
      if (result.size() >= limit)
        break;
    }
    return result;
  }

  WalletType Wallet::getType() const
  {
    return m_impl->type;
  }

  crypto::PublicKey Wallet::getViewPublicKey() const
  {
    return m_impl->viewPub;
  }

  crypto::PublicKey Wallet::getSpendPublicKey() const
  {
    return m_impl->spendPub;
  }

  void Wallet::addPendingOutgoing(const crypto::Hash &txHash, uint64_t amount, uint64_t fee)
  {
    m_impl->balanceTracker.addPendingOutgoing(txHash, amount, fee);
  }

  void Wallet::confirmTransaction(const crypto::Hash &txHash, uint32_t blockHeight)
  {
    m_impl->balanceTracker.confirmTransaction(txHash, blockHeight);

    // Mark the transaction record as confirmed
    // (BalanceTracker doesn't expose a way to do this directly,
    //  so we iterate through transactions)
    // This would require adding a markConfirmed method to BalanceTracker
  }

  uint64_t Wallet::getPendingOutgoingAmount() const
  {
    return m_impl->balanceTracker.getPendingOutgoingAmount();
  }

  std::vector<BalanceTracker::PendingTx> Wallet::getPendingTransactions() const
  {
    return m_impl->balanceTracker.getPendingTransactions();
  }

  void Wallet::addTransaction(const TransactionRecord &tx)
  {
    m_impl->balanceTracker.addTransaction(tx);
  }

  std::vector<TransactionRecord> Wallet::getTransactionHistory(uint32_t offset, uint32_t limit) const
  {
    return m_impl->balanceTracker.getTransactions(offset, limit);
  }

  uint32_t Wallet::getTransactionCount() const
  {
    return m_impl->balanceTracker.getTransactionCount();
  }
} // namespace BoltCore