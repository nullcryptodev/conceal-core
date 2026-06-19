// DepositManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "DepositManager.h"
#include "TransactionBuilder.h"
#include "SignatureBuilder.h"
#include "OutputSelector.h"
#include "OutputUtils.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteConfig.h"
#include "Common/StringTools.h"
#include <algorithm>
#include <random>
#include <string>

namespace BoltCore
{
  DepositManager::DepositManager(const cn::Currency &currency,
                                 TransactionBuilder &txBuilder,
                                 SignatureBuilder &sigBuilder,
                                 OutputSelector &outputSelector,
                                 const cn::AccountPublicAddress &mainAddress)
      : m_currency(currency), m_txBuilder(txBuilder), m_sigBuilder(sigBuilder),
        m_outputSelector(outputSelector), m_mainAddress(mainAddress) {}

  uint64_t DepositManager::calculateInterest(uint64_t amount, uint32_t term, uint32_t height) const
  {
    return m_currency.calculateInterest(amount, term, height);
  }

  TransferResult DepositManager::createDeposit(uint64_t amount, uint32_t term,
                                               const std::vector<OutputInfo> &fundingOutputs)
  {
    TransferResult result;
    result.success = false;

    if (amount < m_currency.depositMinAmount())
    {
      result.error = "Deposit amount below minimum "
                     + m_currency.formatAmount(m_currency.depositMinAmount());
      return result;
    }

    const uint32_t minTerm = m_currency.depositMinTermV3();
    const uint32_t maxTerm = m_currency.depositMaxTermV3();
    if (term < minTerm || term > maxTerm || term % minTerm != 0)
    {
      result.error = "Invalid deposit term (must be a multiple of "
                     + std::to_string(minTerm) + " blocks / 1 month)";
      return result;
    }

    // WalletGreen::createDeposit uses fee = 1000 (MINIMUM_FEE_V2), not minimumFee().
    const uint64_t fee = m_currency.minimumFeeV2();
    const uint64_t neededMoney = amount + fee;

    const auto selection = m_outputSelector.select(neededMoney, fundingOutputs);
    if (!selection.enough)
    {
      result.error = "Insufficient funds for deposit";
      return result;
    }

    const uint64_t changeAmount = selection.totalFound - neededMoney;
    const uint64_t depositAmount = neededMoney - fee; // == amount; matches WalletGreen

    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(0);

    // Deposit multisig output first (WalletGreen order).
    tx->addOutput(depositAmount, std::vector<cn::AccountPublicAddress>{m_mainAddress}, 1, term);

    if (changeAmount > 0)
    {
      using AmountToAddress = std::pair<const cn::AccountPublicAddress *, uint64_t>;
      std::vector<AmountToAddress> changeOutputs;

      cn::decompose_amount_into_digits(
          changeAmount, m_currency.defaultDustThreshold(),
          [&changeOutputs, this](uint64_t chunk)
          { changeOutputs.emplace_back(&m_mainAddress, chunk); },
          [&changeOutputs, this](uint64_t dust)
          { changeOutputs.emplace_back(&m_mainAddress, dust); });

      std::shuffle(changeOutputs.begin(), changeOutputs.end(),
                   std::default_random_engine{
                       crypto::rand<std::default_random_engine::result_type>()});
      std::sort(changeOutputs.begin(), changeOutputs.end(),
                [](const AmountToAddress &left, const AmountToAddress &right)
                { return left.second < right.second; });

      for (const auto &changeOutput : changeOutputs)
        tx->addOutput(changeOutput.second, *changeOutput.first);
    }

    result = m_txBuilder.fundKeyInputs(tx, selection.outputs, 0);
    if (result.success)
      result.fee = fee;
    return result;
  }

  TransferResult DepositManager::withdrawDeposit(const OutputInfo &depositOutput,
                                                 const DepositInfo &deposit)
  {
    TransferResult result;
    result.success = false;

    if (deposit.locked)
    {
      result.error = "Deposit is still locked";
      return result;
    }

    if (!depositOutput.isDeposit || depositOutput.spent)
    {
      result.error = "Deposit output is not available";
      return result;
    }

    if (!depositOutput.hasGlobalOutputIndex || depositOutput.blockHeight == 0)
    {
      result.error = "Missing global output index on deposit (wait for sync or rescan wallet)";
      return result;
    }

    const uint32_t globalOutputIndex = depositOutput.globalOutputIndex;

    const uint64_t fee = m_currency.minimumFeeV2();
    const uint64_t totalAmount = deposit.amount + deposit.interest;

    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(0);

    cn::MultisignatureInput msInput;
    msInput.amount = deposit.amount;
    msInput.signatureCount = 1;
    msInput.outputIndex = globalOutputIndex;
    msInput.term = deposit.term;
    tx->addInput(msInput);

    std::vector<uint64_t> outputAmounts;
    cn::decompose_amount_into_digits(
        totalAmount - fee, m_currency.defaultDustThreshold(),
        [&outputAmounts](uint64_t chunk)
        { outputAmounts.push_back(chunk); },
        [&outputAmounts](uint64_t dust)
        { outputAmounts.push_back(dust); });

    for (auto outputAmount : outputAmounts)
      tx->addOutput(outputAmount, m_txBuilder.accountKeys().address);

    tx->signInputMultisignature(0, depositOutput.txPublicKey, depositOutput.outputIndex,
                               m_txBuilder.accountKeys());

    result = m_txBuilder.finalizeAndRelay(*tx);
    if (result.success)
    {
      result.fee = fee;
      result.spentInputs = {depositOutput};
    }
    return result;
  }
}
