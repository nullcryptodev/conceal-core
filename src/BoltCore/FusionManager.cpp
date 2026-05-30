// FusionManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "FusionManager.h"
#include "OutputSelector.h"
#include "TransactionBuilder.h"
#include "SignatureBuilder.h"
#include "RelayHandler.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include <CryptoNoteCore/TransactionExtra.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <numeric>

namespace BoltCore
{
  FusionManager::FusionManager(const cn::Currency &currency,
                               OutputSelector &outputSelector,
                               TransactionBuilder &txBuilder,
                               SignatureBuilder &sigBuilder,
                               RelayHandler &relay)
      : m_currency(currency), m_outputSelector(outputSelector),
        m_txBuilder(txBuilder), m_sigBuilder(sigBuilder), m_relay(relay) {}

  FusionEstimate FusionManager::estimate(uint64_t threshold,
                                         const std::vector<OutputInfo> &availableOutputs) const
  {
    FusionEstimate result = {0, 0};
    std::array<size_t, 20> bucketSizes{};

    for (const auto &out : availableOutputs)
    {
      if (out.spent || out.isDeposit)
        continue;

      uint8_t powerOfTen = 0;
      if (m_currency.isAmountApplicableInFusionTransactionInput(out.amount, threshold, powerOfTen, 0))
      {
        if (powerOfTen < bucketSizes.size())
          bucketSizes[powerOfTen]++;
      }
      result.totalOutputCount++;
    }

    for (auto bucketSize : bucketSizes)
    {
      if (bucketSize >= m_currency.fusionTxMinInputCount())
        result.fusionReadyCount += bucketSize;
    }

    return result;
  }

  TransferResult FusionManager::createFusion(uint64_t threshold, uint64_t mixin,
                                             const std::vector<OutputInfo> &availableOutputs,
                                             const cn::AccountPublicAddress &destination)
  {
    TransferResult result;
    result.success = false;

    // Find fusion-ready outputs grouped by digit count
    std::array<std::vector<OutputInfo>, 20> buckets;
    for (const auto &out : availableOutputs)
    {
      if (out.spent || out.isDeposit || out.amount <= m_currency.defaultDustThreshold())
        continue;

      uint8_t powerOfTen = 0;
      if (m_currency.isAmountApplicableInFusionTransactionInput(out.amount, threshold, powerOfTen, 0))
      {
        if (powerOfTen < buckets.size())
          buckets[powerOfTen].push_back(out);
      }
    }

    // Pick the best bucket
    size_t selectedBucket = buckets.size();
    for (size_t i = 0; i < buckets.size(); ++i)
    {
      if (buckets[i].size() >= m_currency.fusionTxMinInputCount())
      {
        selectedBucket = i;
        break;
      }
    }

    if (selectedBucket == buckets.size())
    {
      result.error = "No fusion-ready outputs found";
      return result;
    }

    auto &selected = buckets[selectedBucket];
    size_t maxInputs = m_currency.getApproximateMaximumInputCount(
        m_currency.fusionTxMaxSize(), 8, mixin);
    maxInputs = std::min(maxInputs, selected.size());
    size_t minInputs = m_currency.fusionTxMinInputCount();

    // Shuffle and trim
    std::shuffle(selected.begin(), selected.end(),
                 std::default_random_engine{crypto::rand<std::default_random_engine::result_type>()});

    if (selected.size() > maxInputs)
      selected.resize(maxInputs);

    // Build transaction
    std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
    tx->setUnlockTime(0);

    // Sum inputs
    uint64_t totalAmount = std::accumulate(selected.begin(), selected.end(), uint64_t(0),
                                           [](uint64_t sum, const OutputInfo &o)
                                           { return sum + o.amount; });

    // Add fusion outputs
    std::vector<uint64_t> outputAmounts;
    cn::decompose_amount_into_digits(
        totalAmount - m_currency.minimumFee(), 0,
        [&outputAmounts](uint64_t chunk)
        { outputAmounts.push_back(chunk); },
        [&outputAmounts](uint64_t dust)
        { outputAmounts.push_back(dust); });

    std::sort(outputAmounts.begin(), outputAmounts.end());

    for (auto amount : outputAmounts)
      tx->addOutput(amount, destination);

    result.txHash = common::podToHex(tx->getTransactionHash());
    result.fee = 0; // Fusion has zero fee
    result.success = true;

    return result;
  }
}