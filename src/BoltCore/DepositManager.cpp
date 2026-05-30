// DepositManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "DepositManager.h"
#include "TransactionBuilder.h"
#include "SignatureBuilder.h"
#include "OutputSelector.h"
#include "RelayHandler.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include <CryptoNoteCore/TransactionExtra.h>
#include "Common/StringTools.h"

namespace BoltCore
{
  DepositManager::DepositManager(const cn::Currency &currency,
                                 TransactionBuilder &txBuilder,
                                 SignatureBuilder &sigBuilder,
                                 OutputSelector &outputSelector,
                                 RelayHandler &relay,
                                 const cn::AccountPublicAddress &mainAddress)
      : m_currency(currency), m_txBuilder(txBuilder), m_sigBuilder(sigBuilder),
        m_outputSelector(outputSelector), m_relay(relay), m_mainAddress(mainAddress) {}

  uint64_t DepositManager::calculateInterest(uint64_t amount, uint32_t term, uint32_t height) const
  {
    return m_currency.calculateInterest(amount, term, height);
  }

  TransferResult DepositManager::createDeposit(uint64_t amount, uint32_t term,
                                               const std::vector<OutputInfo> &availableOutputs)
  {
    TransferResult result;
    result.success = false;

    uint64_t fee = m_currency.minimumFee();
    uint64_t neededMoney = amount + fee;

    auto selection = m_outputSelector.select(neededMoney, availableOutputs);
    if (!selection.enough)
    {
      result.error = "Insufficient funds for deposit";
      return result;
    }

    uint64_t foundMoney = selection.totalFound;
    uint64_t changeAmount = foundMoney - neededMoney;

    // Build deposit transaction
    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(0);

    // Deterministic tx key
    cn::AccountKeys dummyKeys;
    dummyKeys.address = m_mainAddress;
    tx->setDeterministicTransactionSecretKey(m_sigBuilder.deriveEphemeralSecretKey(
        m_mainAddress.viewPublicKey, 0));

    // Add deposit output (multisig with term)
    std::vector<cn::AccountPublicAddress> depositTargets = {m_mainAddress};
    tx->addOutput(amount, depositTargets, 1, term);

    // Add change outputs
    if (changeAmount > 0)
    {
      std::vector<uint64_t> changeAmounts;
      cn::decompose_amount_into_digits(
          changeAmount, m_currency.defaultDustThreshold(),
          [&changeAmounts](uint64_t chunk)
          { changeAmounts.push_back(chunk); },
          [&changeAmounts](uint64_t dust)
          { changeAmounts.push_back(dust); });

      for (auto ca : changeAmounts)
        tx->addOutput(ca, m_mainAddress);
    }

    // The caller handles signing and relaying
    result.txHash = common::podToHex(tx->getTransactionHash());
    result.fee = fee;
    result.success = true;

    return result;
  }

  TransferResult DepositManager::withdrawDeposit(uint64_t depositId,
                                                 const std::vector<OutputInfo> &availableOutputs,
                                                 const std::vector<DepositInfo> &deposits)
  {
    TransferResult result;
    result.success = false;

    if (depositId >= deposits.size())
    {
      result.error = "Deposit not found";
      return result;
    }

    const auto &deposit = deposits[depositId];
    uint64_t fee = m_currency.minimumFee();
    uint64_t totalAmount = deposit.amount + deposit.interest;

    // Build withdrawal transaction
    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(0);

    // Add multisig input for the deposit
    cn::MultisignatureInput msInput;
    msInput.amount = deposit.amount;
    msInput.signatureCount = 1;
    msInput.outputIndex = 0; // Will be set from deposit info
    msInput.term = deposit.term;
    tx->addInput(msInput);

    // Add output
    std::vector<uint64_t> outputAmounts;
    cn::decompose_amount_into_digits(
        totalAmount - fee, m_currency.defaultDustThreshold(),
        [&outputAmounts](uint64_t chunk)
        { outputAmounts.push_back(chunk); },
        [&outputAmounts](uint64_t dust)
        { outputAmounts.push_back(dust); });

    for (auto a : outputAmounts)
      tx->addOutput(a, m_mainAddress);

    result.txHash = common::podToHex(tx->getTransactionHash());
    result.fee = fee;
    result.success = true;

    return result;
  }
}