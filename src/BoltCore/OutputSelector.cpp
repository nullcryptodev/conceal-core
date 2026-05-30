// OutputSelector implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "OutputSelector.h"
#include "CryptoNoteCore/Currency.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace BoltCore
{
  OutputSelector::OutputSelector(const cn::Currency &currency)
      : m_currency(currency) {}

  OutputSelector::Selection OutputSelector::select(
      uint64_t neededAmount,
      const std::vector<OutputInfo> &available) const
  {
    Selection result;
    result.totalFound = 0;
    result.enough = false;

    if (available.empty())
      return result;

    // Filter unspent only
    std::vector<const OutputInfo *> unspent;
    for (const auto &out : available)
    {
      if (!out.spent)
        unspent.push_back(&out);
    }

    // Group by digit count (same as WalletGreen)
    std::unordered_map<int, std::vector<const OutputInfo *>> buckets;
    for (const auto *out : unspent)
    {
      uint64_t dust = m_currency.defaultDustThreshold();
      if (out->amount > dust)
      {
        int digits = static_cast<int>(floor(log10(out->amount)) + 1);
        buckets[digits].push_back(out);
      }
    }

    // Pick one from each bucket, smallest first
    while (result.totalFound < neededAmount && !buckets.empty())
    {
      for (auto it = buckets.begin(); it != buckets.end();)
      {
        if (it->second.empty())
        {
          it = buckets.erase(it);
          continue;
        }

        if (result.totalFound < neededAmount)
        {
          const auto *out = it->second.back();
          result.outputs.push_back(*out);
          result.totalFound += out->amount;
          it->second.pop_back();
        }
        ++it;
      }
    }

    result.enough = (result.totalFound >= neededAmount);
    return result;
  }
}