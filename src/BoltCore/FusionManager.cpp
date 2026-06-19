// FusionManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "FusionManager.h"
#include "TransactionBuilder.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteConfig.h"
#include "CryptoNoteCore/TransactionApi.h"
#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/CryptoNoteTools.h"
#include <CryptoNoteCore/TransactionExtra.h>
#include <algorithm>
#include <array>
#include <numeric>
#include <random>

namespace BoltCore
{
  namespace
  {
    bool isDustFusionInput(uint64_t amount, uint64_t dustThreshold)
    {
      return amount > 0 && amount < dustThreshold;
    }

    bool isFusionInputCandidate(const cn::Currency &currency,
                                uint64_t amount,
                                uint64_t threshold,
                                uint64_t mixin,
                                uint8_t &powerOfTen,
                                uint32_t height,
                                size_t dustBucketIndex)
    {
      const uint64_t dustThreshold = currency.defaultDustThreshold();
      if (mixin == 0 && isDustFusionInput(amount, dustThreshold))
      {
        powerOfTen = static_cast<uint8_t>(dustBucketIndex);
        return true;
      }

      return currency.isAmountApplicableInFusionTransactionInput(
          amount, threshold, powerOfTen, height);
    }

    std::unique_ptr<cn::ITransaction> buildFusionOutputShell(
        const cn::Currency &currency,
        const std::vector<OutputInfo> &selected,
        const cn::AccountPublicAddress &destination)
    {
      uint64_t totalAmount = std::accumulate(
          selected.begin(), selected.end(), uint64_t(0),
          [](uint64_t sum, const OutputInfo &o) { return sum + o.amount; });

      std::vector<uint64_t> outputAmounts;
      cn::decompose_amount_into_digits(
          totalAmount - currency.minimumFeeV2(), 0,
          [&outputAmounts](uint64_t chunk) { outputAmounts.push_back(chunk); },
          [&outputAmounts](uint64_t dust) { outputAmounts.push_back(dust); });

      std::sort(outputAmounts.begin(), outputAmounts.end());

      std::unique_ptr<cn::ITransaction> tx = cn::createTransaction();
      tx->setUnlockTime(0);
      for (auto amount : outputAmounts)
        tx->addOutput(amount, destination);
      return tx;
    }

    std::vector<OutputInfo> pickFusionInputs(
        const std::array<std::vector<OutputInfo>, cn::parameters::FUSION_BUCKET_COUNT> &buckets,
        const std::vector<OutputInfo> &dustOutputs,
        uint64_t mixin,
        size_t minInputCount,
        size_t maxInputCount)
    {
      std::array<size_t, cn::parameters::FUSION_BUCKET_COUNT> bucketSizes{};
      for (size_t i = 0; i < buckets.size(); ++i)
        bucketSizes[i] = buckets[i].size();

      const size_t dustReadyCount = dustOutputs.size();
      const bool dustBucketEligible = mixin == 0 && dustReadyCount >= minInputCount;

      std::vector<uint8_t> bucketNumbers(bucketSizes.size());
      std::iota(bucketNumbers.begin(), bucketNumbers.end(), 0);
      std::shuffle(bucketNumbers.begin(), bucketNumbers.end(),
                   std::default_random_engine{
                       crypto::rand<std::default_random_engine::result_type>()});

      size_t bucketNumberIndex = 0;
      for (; bucketNumberIndex < bucketNumbers.size(); ++bucketNumberIndex)
      {
        if (bucketSizes[bucketNumbers[bucketNumberIndex]] >= minInputCount)
          break;
      }

      const bool prettyBucketEligible = bucketNumberIndex != bucketNumbers.size();
      if (!prettyBucketEligible && !dustBucketEligible)
        return {};

      const bool useDustBucket = dustBucketEligible &&
                                 (!prettyBucketEligible ||
                                  (crypto::rand<uint8_t>() & 1) == 0);

      auto sortByAmount = [](const OutputInfo &a, const OutputInfo &b)
      { return a.amount < b.amount; };

      if (useDustBucket)
      {
        std::vector<OutputInfo> selected = dustOutputs;
        if (selected.size() <= maxInputCount)
        {
          std::sort(selected.begin(), selected.end(), sortByAmount);
          return selected;
        }

        std::shuffle(selected.begin(), selected.end(),
                     std::default_random_engine{
                         crypto::rand<std::default_random_engine::result_type>()});
        selected.resize(maxInputCount);
        std::sort(selected.begin(), selected.end(), sortByAmount);
        return selected;
      }

      const size_t selectedBucket = bucketNumbers[bucketNumberIndex];
      std::vector<OutputInfo> selected = buckets[selectedBucket];
      if (selected.size() <= maxInputCount)
      {
        std::sort(selected.begin(), selected.end(), sortByAmount);
        return selected;
      }

      std::shuffle(selected.begin(), selected.end(),
                   std::default_random_engine{
                       crypto::rand<std::default_random_engine::result_type>()});
      selected.resize(maxInputCount);
      std::sort(selected.begin(), selected.end(), sortByAmount);
      return selected;
    }
  }

  FusionManager::FusionManager(const cn::Currency &currency, TransactionBuilder &txBuilder)
      : m_currency(currency), m_txBuilder(txBuilder) {}

  FusionEstimate FusionManager::estimate(uint64_t threshold, uint64_t mixin,
                                         const std::vector<OutputInfo> &availableOutputs,
                                         uint32_t height) const
  {
    FusionEstimate result{};
    const size_t bucketCount = m_currency.fusionBucketCount();
    const size_t dustBucketIndex = bucketCount - 1;
    std::vector<size_t> bucketSizes(bucketCount, 0);

    for (const auto &out : availableOutputs)
    {
      if (out.spent || out.isDeposit)
        continue;

      uint8_t powerOfTen = 0;
      if (isFusionInputCandidate(m_currency, out.amount, threshold, mixin, powerOfTen, height,
                                 dustBucketIndex))
      {
        if (powerOfTen < bucketSizes.size())
          bucketSizes[powerOfTen]++;
      }
      result.totalOutputCount++;
    }

    for (auto bucketSize : bucketSizes)
    {
      if (bucketSize >= m_currency.fusionTxMinInputCount())
      {
        result.fusionReadyCount += bucketSize;
        ++result.readyBucketCount;
        if (bucketSize > result.largestReadyBucket)
          result.largestReadyBucket = bucketSize;
      }
    }

    return result;
  }

  TransferResult FusionManager::createFusion(uint64_t threshold, uint64_t mixin,
                                             const std::vector<OutputInfo> &availableOutputs,
                                             const cn::AccountPublicAddress &destination,
                                             uint32_t height)
  {
    TransferResult result;
    result.success = false;

    if (threshold <= m_currency.defaultDustThreshold())
    {
      result.error = "Threshold too low";
      return result;
    }

    const size_t maxOutputCount = m_currency.fusionTxMaxOutputCount();
    const size_t minInputCount = m_currency.fusionTxMinInputCount();
    const size_t maxInputs = m_currency.getApproximateMaximumInputCount(
        m_currency.fusionTxMaxSize(), maxOutputCount, mixin);

    if (maxInputs < minInputCount)
    {
      result.error = "Mixin count too big";
      return result;
    }

    const size_t bucketCount = m_currency.fusionBucketCount();
    const size_t dustBucketIndex = bucketCount - 1;
    std::array<std::vector<OutputInfo>, cn::parameters::FUSION_BUCKET_COUNT> buckets;
    std::vector<OutputInfo> dustOutputs;

    for (const auto &out : availableOutputs)
    {
      if (out.spent || out.isDeposit)
        continue;

      uint8_t powerOfTen = 0;
      if (!isFusionInputCandidate(m_currency, out.amount, threshold, mixin, powerOfTen, height,
                                  dustBucketIndex))
        continue;

      if (powerOfTen == dustBucketIndex)
        dustOutputs.push_back(out);
      else if (powerOfTen < buckets.size())
        buckets[powerOfTen].push_back(out);
    }

    std::vector<OutputInfo> selected = pickFusionInputs(
        buckets, dustOutputs, mixin, minInputCount, maxInputs);

    if (selected.size() < minInputCount)
    {
      result.error = "Nothing to optimize";
      return result;
    }

    std::string signError;
    if (!m_txBuilder.ensureGlobalOutputIndices(selected, signError))
    {
      result.error = signError;
      return result;
    }

    const uint64_t fusionFee = m_currency.minimumFeeV2();
    std::unique_ptr<cn::ITransaction> tx;
    int round = 0;
    do
    {
      if (round != 0)
        selected.pop_back();

      if (selected.size() < minInputCount)
      {
        result.error = "Minimum input count not reached after size trim";
        return result;
      }

      tx = buildFusionOutputShell(m_currency, selected, destination);
      if (tx->getOutputCount() == 0)
      {
        result.error = "Wrong amount";
        return result;
      }
      if (tx->getOutputCount() > maxOutputCount)
      {
        result.error = "Too many fusion outputs";
        return result;
      }

      if (!m_txBuilder.signFusionKeyInputs(*tx, selected, mixin, signError))
      {
        result.error = signError;
        return result;
      }

      ++round;
    } while (tx->getTransactionData().size() > m_currency.fusionTxMaxSize() &&
             selected.size() >= minInputCount);

    if (selected.size() < minInputCount)
    {
      result.error = "Minimum input count not reached after size trim";
      return result;
    }

    tx = buildFusionOutputShell(m_currency, selected, destination);
    if (tx->getOutputCount() == 0)
    {
      result.error = "Wrong amount";
      return result;
    }
    if (tx->getOutputCount() > maxOutputCount)
    {
      result.error = "Too many fusion outputs";
      return result;
    }

    result = m_txBuilder.fundFusionKeyInputs(tx, selected, mixin);
    if (!result.success)
      return result;

    result.fee = fusionFee;
    return result;
  }
}
