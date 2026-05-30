// TransactionBuilder implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "TransactionBuilder.h"
#include "OutputSelector.h"
#include "SignatureBuilder.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Account.h"
#include <CryptoNoteCore/TransactionExtra.h>
#include "Common/StringTools.h"
#include <algorithm>
#include <random>
#include <cmath>

namespace BoltCore
{
  TransactionBuilder::TransactionBuilder(const cn::Currency &currency,
                                         OutputSelector &outputSelector,
                                         SignatureBuilder &signatureBuilder)
      : m_currency(currency), m_outputSelector(outputSelector),
        m_signatureBuilder(signatureBuilder) {}

  namespace
  {
    void decomposeAmount(uint64_t amount, uint64_t dustThreshold, std::vector<uint64_t> &amounts)
    {
      cn::decompose_amount_into_digits(
          amount, dustThreshold,
          [&amounts](uint64_t chunk)
          { amounts.push_back(chunk); },
          [&amounts](uint64_t dust)
          { amounts.push_back(dust); });
    }

    uint64_t countNeededMoney(const std::vector<Transfer> &transfers, uint64_t fee)
    {
      uint64_t needed = fee;
      for (const auto &t : transfers)
        needed += t.amount;
      return needed;
    }
  }

  TransactionBuilder::BuildResult TransactionBuilder::build(
      const std::vector<OutputInfo> &availableOutputs,
      const BuilderParams &params)
  {
    BuildResult result;
    result.success = false;

    uint64_t neededMoney = countNeededMoney(params.transfers, params.fee);

    auto selection = m_outputSelector.select(neededMoney, availableOutputs);
    if (!selection.enough)
    {
      result.error = "Insufficient funds";
      return result;
    }

    uint64_t foundMoney = selection.totalFound;
    uint64_t changeAmount = foundMoney - neededMoney;

    // Donation
    uint64_t donationAmount = 0;
    if (params.donationThreshold > 0)
    {
      donationAmount = std::min(changeAmount, params.donationThreshold);
      changeAmount -= donationAmount;
    }

    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(params.unlockTime);

    // Deterministic tx key
    tx->setDeterministicTransactionSecretKey(m_signatureBuilder.deriveEphemeralSecretKey(
        params.mainAddress.viewPublicKey, 0));
    crypto::SecretKey txKey;
    tx->getTransactionSecretKey(txKey);

    // Parse all destination addresses first so pointers stay valid
    result.destAddresses.clear();
    result.destAddresses.reserve(params.transfers.size());
    for (const auto &t : params.transfers)
    {
      cn::AccountPublicAddress destAddr;
      if (!m_currency.parseAccountAddressString(t.address, destAddr))
      {
        result.error = "Invalid destination address: " + t.address;
        return result;
      }
      result.destAddresses.push_back(destAddr);
    }

    // Build output list with correct destination addresses
    struct AmountToAddress
    {
      const cn::AccountPublicAddress *addr;
      uint64_t amount;
    };
    std::vector<AmountToAddress> outputs;

    for (size_t i = 0; i < params.transfers.size(); ++i)
    {
      std::vector<uint64_t> amounts;
      decomposeAmount(params.transfers[i].amount, m_currency.defaultDustThreshold(), amounts);
      for (auto a : amounts)
        outputs.push_back({&result.destAddresses[i], a});
    }

    if (changeAmount > 0)
    {
      std::vector<uint64_t> amounts;
      decomposeAmount(changeAmount, m_currency.defaultDustThreshold(), amounts);
      for (auto a : amounts)
        outputs.push_back({&params.changeAddress, a});
    }

    if (donationAmount > 0)
    {
      outputs.push_back({&params.donationAddress, donationAmount});
    }

    std::shuffle(outputs.begin(), outputs.end(),
                 std::default_random_engine{crypto::rand<std::default_random_engine::result_type>()});
    std::sort(outputs.begin(), outputs.end(),
              [](const AmountToAddress &l, const AmountToAddress &r)
              { return l.amount < r.amount; });

    for (const auto &out : outputs)
      tx->addOutput(out.amount, *out.addr);

    // Extra
    if (params.ttl != 0)
    {
      cn::BinaryArray ba;
      cn::appendTTLToExtra(ba, params.ttl);
      tx->appendExtra(ba);
    }

    if (!params.extra.empty())
      tx->appendExtra(common::asBinaryArray(params.extra));

    result.transaction = std::move(tx);
    result.transactionSecretKey = txKey;
    result.selectedOutputs = selection.outputs;
    result.changeAmount = changeAmount;
    result.fee = params.fee;
    result.success = true;

    return result;
  }
}