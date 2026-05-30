// SubAddressManager - generate and track sub-addresses
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "crypto/crypto.h"
#include <string>
#include <vector>

namespace cn
{
  class Currency;
}

namespace BoltCore
{
  class SubAddressManager
  {
  public:
    SubAddressManager(const cn::Currency &currency,
                      const crypto::PublicKey &viewPublicKey,
                      const crypto::SecretKey &viewSecretKey,
                      const crypto::SecretKey &mainSpendSecretKey);

    // Generate a new sub-address
    SubAddress generate();

    // Generate sub-address from known spend key
    SubAddress import(const crypto::SecretKey &spendSecretKey);

    // Get all sub-addresses
    std::vector<SubAddress> getAll() const;

    // Get main address
    std::string getMainAddress() const;

    // Check if an output belongs to any of our addresses
    bool isOurOutput(const crypto::PublicKey &txPublicKey,
                     size_t outputIndex,
                     const crypto::PublicKey &outputKey) const;

    // Find which sub-address an output belongs to
    std::string findSubAddress(const crypto::PublicKey &txPublicKey,
                               size_t outputIndex,
                               const crypto::PublicKey &outputKey) const;

  private:
    const cn::Currency &m_currency;
    crypto::PublicKey m_viewPublicKey;
    crypto::SecretKey m_viewSecretKey;
    crypto::SecretKey m_mainSpendSecretKey;
    crypto::PublicKey m_mainSpendPublicKey;
    std::vector<SubAddress> m_subAddresses;
  };
}