// SubAddressManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SubAddressManager.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/Account.h"

namespace BoltCore
{
  SubAddressManager::SubAddressManager(const cn::Currency &currency,
                                       const crypto::PublicKey &viewPublicKey,
                                       const crypto::SecretKey &viewSecretKey,
                                       const crypto::SecretKey &mainSpendSecretKey)
      : m_currency(currency),
        m_viewPublicKey(viewPublicKey),
        m_viewSecretKey(viewSecretKey),
        m_mainSpendSecretKey(mainSpendSecretKey)
  {
    crypto::secret_key_to_public_key(m_mainSpendSecretKey, m_mainSpendPublicKey);

    // Main address is always first
    SubAddress mainAddr;
    mainAddr.spendPublicKey = m_mainSpendPublicKey;
    mainAddr.spendSecretKey = m_mainSpendSecretKey;
    mainAddr.address = m_currency.accountAddressAsString(
        {m_mainSpendPublicKey, m_viewPublicKey});
    m_subAddresses.push_back(mainAddr);
  }

  SubAddress SubAddressManager::generate()
  {
    crypto::PublicKey spendPub;
    crypto::SecretKey spendSec;
    crypto::generate_keys(spendPub, spendSec);

    SubAddress addr;
    addr.spendPublicKey = spendPub;
    addr.spendSecretKey = spendSec;
    addr.address = m_currency.accountAddressAsString({spendPub, m_viewPublicKey});
    m_subAddresses.push_back(addr);

    return addr;
  }

  SubAddress SubAddressManager::import(const crypto::SecretKey &spendSecretKey)
  {
    crypto::PublicKey spendPub;
    crypto::secret_key_to_public_key(spendSecretKey, spendPub);

    SubAddress addr;
    addr.spendPublicKey = spendPub;
    addr.spendSecretKey = spendSecretKey;
    addr.address = m_currency.accountAddressAsString({spendPub, m_viewPublicKey});
    m_subAddresses.push_back(addr);

    return addr;
  }

  std::vector<SubAddress> SubAddressManager::getAll() const
  {
    return m_subAddresses;
  }

  std::string SubAddressManager::getMainAddress() const
  {
    return m_subAddresses.empty() ? "" : m_subAddresses[0].address;
  }

  bool SubAddressManager::isOurOutput(const crypto::PublicKey &txPublicKey,
                                      size_t outputIndex,
                                      const crypto::PublicKey &outputKey) const
  {
    return !findSubAddress(txPublicKey, outputIndex, outputKey).empty();
  }

  std::string SubAddressManager::findSubAddress(const crypto::PublicKey &txPublicKey,
                                                size_t outputIndex,
                                                const crypto::PublicKey &outputKey) const
  {
    crypto::KeyDerivation derivation;
    if (!crypto::generate_key_derivation(txPublicKey, m_viewSecretKey, derivation))
      return "";

    for (const auto &sub : m_subAddresses)
    {
      crypto::PublicKey derivedKey;
      if (!crypto::derive_public_key(derivation, outputIndex, sub.spendPublicKey, derivedKey))
        continue;

      if (derivedKey == outputKey)
        return sub.address;
    }

    return "";
  }
}