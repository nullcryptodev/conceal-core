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
#include "OutputUtils.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "CryptoNoteCore/Account.h"
#include "CryptoNoteConfig.h"
#include "Common/StringTools.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <system_error>

namespace BoltCore
{
  namespace
  {
    uint64_t computeDustBalance(const std::vector<OutputInfo> &outputs,
                                uint32_t currentHeight,
                                uint32_t unlockWindow,
                                uint64_t dustThreshold,
                                const std::string *address = nullptr)
    {
      uint64_t dust = 0;
      for (const auto &out : outputs)
      {
        if (address && out.subAddress != *address)
          continue;
        if (!isSpendableKeyOutput(out, currentHeight, unlockWindow))
          continue;
        if (out.amount < dustThreshold)
          dust += out.amount;
      }
      return dust;
    }

    std::string ensureDaemonReady(cn::INode &node)
    {
      if (node.getLastKnownBlockHeight() > 0)
        return {};

      std::promise<std::error_code> promise;
      auto future = promise.get_future();
      node.init([&promise](std::error_code ec)
                { promise.set_value(ec); });
      if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        return "Timed out connecting to daemon (check conceald RPC on port "
               + std::to_string(cn::RPC_DEFAULT_PORT) + ")";
      const std::error_code ec = future.get();
      if (ec)
        return "Cannot connect to daemon: " + ec.message() +
               " (tx building needs conceald RPC on port " + std::to_string(cn::RPC_DEFAULT_PORT) +
               ", not P2P port " + std::to_string(cn::P2P_DEFAULT_PORT) + ")";
      if (node.getLastKnownBlockHeight() == 0)
        return "Daemon not ready (block height 0). Start conceald and wait for sync.";
      return {};
    }
  }

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
    RelayHandler relayHandler;
    cn::AccountKeys accountKeys;
    TransactionBuilder transactionBuilder;
    DepositManager depositManager;
    FusionManager fusionManager;
    SubAddressManager subAddressManager;

    cn::AccountPublicAddress mainAddress;
    std::vector<DepositInfo> deposits;

    GlobalOutputIndexResolver globalIndexResolver;

    Impl(const crypto::SecretKey &vk, const crypto::SecretKey &sk,
         const crypto::PublicKey &vp, const crypto::PublicKey &sp,
         cn::INode &n, const cn::Currency &c)
        : viewKey(vk), spendKey(sk), viewPub(vp), spendPub(sp),
          node(n), currency(c),
          type(sk == crypto::SecretKey() ? WalletType::ViewOnly : WalletType::Full),
          balanceTracker(),
          outputSelector(c),
          signatureBuilder(vk, sk, vp, sp),
          relayHandler(n),
          accountKeys({sp, vp, sk, vk}),
          transactionBuilder(c, node, outputSelector, signatureBuilder, relayHandler, accountKeys),
          depositManager(c, transactionBuilder, signatureBuilder, outputSelector, {sp, vp}),
          fusionManager(c, transactionBuilder),
          subAddressManager(c, vp, vk, sk),
          mainAddress{sp, vp}
    {
    }
  };

  bool Wallet::resolveMissingGlobalIndex(OutputInfo &out)
  {
    if (out.hasGlobalOutputIndex && out.blockHeight > 0)
      return true;
    if (out.blockHeight == 0)
      return false;

    uint32_t globalIndex = 0;
    if (m_impl->globalIndexResolver && m_impl->globalIndexResolver(out, globalIndex))
    {
      out.globalOutputIndex = globalIndex;
      out.hasGlobalOutputIndex = true;
      m_impl->balanceTracker.mergeOutput(out);
      return true;
    }

    if (m_impl->transactionBuilder.resolveGlobalOutputIndex(out, globalIndex))
    {
      out.globalOutputIndex = globalIndex;
      out.hasGlobalOutputIndex = true;
      m_impl->balanceTracker.mergeOutput(out);
      return true;
    }
    return false;
  }

  std::vector<OutputInfo> Wallet::prepareFundingOutputs(std::vector<OutputInfo> funding)
  {
    for (auto &entry : funding)
      resolveMissingGlobalIndex(entry);
    return funding;
  }

  void Wallet::refreshDeposits()
  {
    m_impl->deposits.clear();
    uint64_t id = 0;
    const uint32_t height = m_impl->balanceTracker.getCurrentHeight();
    for (const auto &out : m_impl->balanceTracker.getOutputs())
    {
      if (!out.isDeposit || out.spent)
        continue;

      DepositInfo info;
      info.id = id++;
      info.amount = out.amount;
      info.term = out.term;
      info.unlockHeight = out.blockHeight + out.term;
      info.locked = !isDepositUnlocked(out, height);
      info.interest = m_impl->currency.calculateInterest(out.amount, out.term, out.blockHeight);
      info.creatingTxHash = common::podToHex(out.txHash);
      info.outputIndex = out.outputIndex;
      m_impl->deposits.push_back(std::move(info));
    }
  }

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
    refreshDeposits();
  }

  void Wallet::addOutput(const OutputInfo &output)
  {
    m_impl->balanceTracker.addOutput(output);
    refreshDeposits();
  }

  bool Wallet::mergeOutput(const OutputInfo &output)
  {
    const bool merged = m_impl->balanceTracker.mergeOutput(output);
    refreshDeposits();
    return merged;
  }

  bool Wallet::ingestOutput(const OutputInfo &output)
  {
    const bool credited = m_impl->balanceTracker.ingestOutput(output);
    refreshDeposits();
    return credited;
  }

  void Wallet::addUnconfirmedOutput(const OutputInfo &output)
  {
    m_impl->balanceTracker.addUnconfirmedOutput(output);
    refreshDeposits();
  }

  void Wallet::markOutputSpent(const crypto::KeyImage &keyImage)
  {
    m_impl->balanceTracker.markSpent(keyImage);
    refreshDeposits();
  }

  void Wallet::markOutputSpentByRef(const crypto::Hash &txHash, uint32_t outputIndex)
  {
    m_impl->balanceTracker.markSpentByRef(txHash, outputIndex);
    refreshDeposits();
  }

  void Wallet::markDepositOutputSpent(const crypto::Hash &txHash, uint32_t outputIndex)
  {
    m_impl->balanceTracker.markDepositSpent(txHash, outputIndex);
    refreshDeposits();
  }

  void Wallet::markOutputsSpent(const std::vector<OutputInfo> &outputs)
  {
    for (const auto &out : outputs)
    {
      if (out.isDeposit)
      {
        m_impl->balanceTracker.markDepositSpent(out.txHash, out.outputIndex);
        continue;
      }

      if (!m_impl->balanceTracker.markSpent(out.keyImage))
        m_impl->balanceTracker.markSpentByRef(out.txHash, out.outputIndex);
    }
    refreshDeposits();
  }

  void Wallet::confirmPendingOutgoing(uint32_t blockHeight)
  {
    auto pending = m_impl->balanceTracker.getPendingTransactions();
    for (const auto &tx : pending)
    {
      if (!tx.incoming)
        m_impl->balanceTracker.confirmTransaction(tx.txHash, blockHeight);
    }
  }

  void Wallet::setCurrentHeight(uint32_t height)
  {
    m_impl->balanceTracker.setCurrentHeight(height);
    refreshDeposits();
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

  uint64_t Wallet::getAccruedInterest() const
  {
    uint64_t total = 0;
    for (const auto &out : m_impl->balanceTracker.getOutputs())
    {
      if (out.isDeposit && !out.spent)
        total += calculateDepositInterest(out);
    }
    return total;
  }

  uint64_t Wallet::getEarnedInterest() const
  {
    uint64_t total = 0;
    for (const auto &out : m_impl->balanceTracker.getOutputs())
    {
      if (out.isDeposit && out.spent)
        total += calculateDepositInterest(out);
    }
    return total;
  }

  Balance Wallet::getBalance() const
  {
    Balance balance = m_impl->balanceTracker.getTotalBalance();
    const uint32_t height = getCurrentHeight();
    const uint32_t unlockWindow = m_impl->currency.minedMoneyUnlockWindow();
    balance.dust = computeDustBalance(
        m_impl->balanceTracker.getOutputs(), height, unlockWindow,
        m_impl->currency.defaultDustThreshold());
    balance.accruedInterest = getAccruedInterest();
    return balance;
  }

  Balance Wallet::getBalance(const std::string &address) const
  {
    Balance balance = m_impl->balanceTracker.getBalance(address);
    const uint32_t height = getCurrentHeight();
    const uint32_t unlockWindow = m_impl->currency.minedMoneyUnlockWindow();
    balance.dust = computeDustBalance(
        m_impl->balanceTracker.getOutputs(), height, unlockWindow,
        m_impl->currency.defaultDustThreshold(), &address);
    return balance;
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

    const std::string nodeError = ensureDaemonReady(m_impl->node);
    if (!nodeError.empty())
    {
      result.error = nodeError;
      return result;
    }

    BuilderParams params;
    params.transfers = transfers;
    params.fee = m_impl->currency.minimumFeeV2();
    params.mixin = mixin != 0 ? mixin : static_cast<uint64_t>(cn::parameters::MINIMUM_MIXIN);
    params.unlockTime = 0;
    params.ttl = 0;
    params.changeAddress = m_impl->mainAddress;
    params.donationAddress = m_impl->mainAddress;
    params.donationThreshold = 0;
    params.mainAddress = m_impl->mainAddress;

    auto funding = prepareFundingOutputs(getFundingOutputs());
    result = m_impl->transactionBuilder.buildTransfer(funding, params);
    if (!result.success)
      return result;

    TransactionRecord record;
    record.txHash = getObjectHash(result.transaction);
    record.timestamp = static_cast<uint64_t>(std::time(nullptr));
    record.fee = result.fee;
    uint64_t totalAmount = 0;
    for (const auto &t : transfers)
      totalAmount += t.amount;
    record.totalSent = totalAmount + result.fee;
    record.totalReceived = 0;
    record.type = TransactionType::Outgoing;
    record.confirmed = false;
    if (!transfers.empty())
      record.address = transfers[0].address;
    m_impl->balanceTracker.addTransaction(record);

    markOutputsSpent(result.spentInputs);
    m_impl->balanceTracker.addPendingOutgoing(record.txHash, totalAmount, result.fee);

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

    const std::string nodeError = ensureDaemonReady(m_impl->node);
    if (!nodeError.empty())
    {
      result.error = nodeError;
      return result;
    }

    auto funding = prepareFundingOutputs(getFundingOutputs());
    auto depositResult = m_impl->depositManager.createDeposit(amount, term, funding);

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
      markOutputsSpent(depositResult.spentInputs);

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

    OutputInfo depositOutput;
    if (!getDepositOutput(depositId, depositOutput))
    {
      result.error = "Deposit not found";
      return result;
    }

    if (!resolveMissingGlobalIndex(depositOutput))
    {
      result.error = "Missing global output index on deposit (wait for sync or rescan wallet)";
      return result;
    }

    const DepositInfo *deposit = nullptr;
    for (const auto &dep : m_impl->deposits)
    {
      if (dep.id == depositId)
      {
        deposit = &dep;
        break;
      }
    }
    if (!deposit)
    {
      result.error = "Deposit not found";
      return result;
    }

    auto withdrawResult = m_impl->depositManager.withdrawDeposit(depositOutput, *deposit);

    result.success = withdrawResult.success;
    result.txHash = withdrawResult.txHash;
    result.fee = withdrawResult.fee;
    result.error = withdrawResult.error;
    result.transaction = withdrawResult.transaction;

    if (result.success)
    {
      markDepositOutputSpent(depositOutput.txHash, depositOutput.outputIndex);

      TransactionRecord record;
      if (common::podFromHex(result.txHash, record.txHash))
      {
        record.timestamp = static_cast<uint64_t>(std::time(nullptr));
        record.fee = withdrawResult.fee;
        record.totalSent = 0;
        record.totalReceived = deposit->amount + deposit->interest - withdrawResult.fee;
        record.type = TransactionType::Withdrawal;
        record.confirmed = false;
        m_impl->balanceTracker.addTransaction(record);
        m_impl->balanceTracker.addPendingIncoming(record.txHash, record.totalReceived);
      }
    }

    return result;
  }

  std::vector<DepositInfo> Wallet::getDeposits() const
  {
    return m_impl->deposits;
  }

  FusionEstimate Wallet::estimateFusion(uint64_t threshold, uint64_t mixin) const
  {
    return m_impl->fusionManager.estimate(threshold, mixin, getUnspentOutputs(),
                                          getCurrentHeight());
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

    const std::string nodeError = ensureDaemonReady(m_impl->node);
    if (!nodeError.empty())
    {
      result.error = nodeError;
      return result;
    }

    auto fusionResult = m_impl->fusionManager.createFusion(
        threshold, mixin, getUnspentOutputs(), m_impl->mainAddress, getCurrentHeight());

    result.success = fusionResult.success;
    result.txHash = fusionResult.txHash;
    result.fee = fusionResult.fee;
    result.error = fusionResult.error;
    result.transaction = fusionResult.transaction;
    result.spentInputs = fusionResult.spentInputs;

    if (result.success)
    {
      markOutputsSpent(fusionResult.spentInputs);

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
        m_impl->balanceTracker.addPendingOutgoing(record.txHash, 0, fusionResult.fee);
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

  std::vector<OutputInfo> Wallet::getFundingOutputs() const
  {
    const uint32_t height = getCurrentHeight();
    const uint32_t unlockWindow = m_impl->currency.minedMoneyUnlockWindow();
    const uint64_t dustThreshold = m_impl->currency.defaultDustThreshold();
    std::vector<OutputInfo> funding;
    for (const auto &out : getOutputs())
    {
      if (isFundingKeyOutput(out, height, unlockWindow, dustThreshold))
        funding.push_back(out);
    }
    return funding;
  }

  bool Wallet::getDepositOutput(uint64_t depositId, OutputInfo &output) const
  {
    const DepositInfo *deposit = nullptr;
    for (const auto &dep : m_impl->deposits)
    {
      if (dep.id == depositId)
      {
        deposit = &dep;
        break;
      }
    }
    if (!deposit)
      return false;

    crypto::Hash creatingTxHash;
    if (!common::podFromHex(deposit->creatingTxHash, creatingTxHash))
      return false;

    for (const auto &out : getOutputs())
    {
      if (out.isDeposit && !out.spent && out.txHash == creatingTxHash &&
          out.outputIndex == deposit->outputIndex && out.amount == deposit->amount &&
          out.term == deposit->term)
      {
        output = out;
        return true;
      }
    }
    return false;
  }

  std::vector<OutputInfo> Wallet::getUnspentOutputs() const
  {
    const uint32_t height = getCurrentHeight();
    const uint32_t unlockWindow = m_impl->currency.minedMoneyUnlockWindow();
    std::vector<OutputInfo> unspent;
    for (const auto &out : getOutputs())
    {
      if (isSpendableKeyOutput(out, height, unlockWindow))
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

  void Wallet::addPendingIncoming(const crypto::Hash &txHash, uint64_t amount)
  {
    m_impl->balanceTracker.addPendingIncoming(txHash, amount);
  }

  bool Wallet::hasTransaction(const crypto::Hash &txHash) const
  {
    return m_impl->balanceTracker.hasTransaction(txHash);
  }

  bool Wallet::hasPendingOutgoing(const crypto::Hash &txHash) const
  {
    return m_impl->balanceTracker.hasPendingOutgoing(txHash);
  }

  bool Wallet::incomingTxAlreadyCredited(const crypto::Hash &txHash) const
  {
    return m_impl->balanceTracker.incomingTxAlreadyCredited(txHash);
  }

  bool Wallet::txHasUnconfirmedOutputs(const crypto::Hash &txHash) const
  {
    return m_impl->balanceTracker.txHasUnconfirmedOutputs(txHash);
  }

  void Wallet::mergeExistingIncomingTransaction(const crypto::Hash &txHash,
                                                const std::vector<OutputInfo> &outputs,
                                                uint32_t blockHeight)
  {
    if (outputs.empty())
      return;

    m_impl->balanceTracker.mergeDiscoveredOutputs(txHash, outputs, blockHeight);
    m_impl->balanceTracker.confirmTransaction(txHash, blockHeight);
    refreshDeposits();
  }

  void Wallet::addDiscoveredTransaction(const crypto::Hash &txHash,
                                        const std::vector<OutputInfo> &outputs,
                                        uint32_t blockHeight)
  {
    if (outputs.empty())
      return;

    const bool known = m_impl->balanceTracker.hasTransaction(txHash);
    const bool outgoing = m_impl->balanceTracker.hasPendingOutgoing(txHash);

    // Tx already in wallet and fully confirmed — merge only, never add rows again.
    if (blockHeight > 0 && !outgoing &&
        (m_impl->balanceTracker.incomingTxBatchAlreadyRecorded(txHash, blockHeight, outputs) ||
         (m_impl->balanceTracker.hasTransaction(txHash) &&
          !m_impl->balanceTracker.txHasUnconfirmedOutputs(txHash))))
    {
      mergeExistingIncomingTransaction(txHash, outputs, blockHeight);
      return;
    }

    m_impl->balanceTracker.applyDiscoveredOutputs(txHash, outputs, blockHeight);

    if (known)
    {
      if (blockHeight > 0)
        m_impl->balanceTracker.confirmTransaction(txHash, blockHeight);
    }
    else if (blockHeight == 0)
    {
      uint64_t total = 0;
      bool incomingReceive = false;
      for (const auto &out : outputs)
      {
        if (out.isDeposit)
          continue;
        incomingReceive = true;
        if (!out.spent)
          total += out.amount;
      }
      if (incomingReceive)
        m_impl->balanceTracker.addPendingIncoming(txHash, total);
    }
    else
    {
      m_impl->balanceTracker.confirmTransaction(txHash, blockHeight);
    }

    refreshDeposits();
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

  void Wallet::setGlobalOutputIndexResolver(GlobalOutputIndexResolver resolver)
  {
    m_impl->globalIndexResolver = std::move(resolver);
  }
} // namespace BoltCore