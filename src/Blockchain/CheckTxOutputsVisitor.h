// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <string>

#include "CryptoNoteCore/CryptoNoteFormatUtils.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteCore/NewOutputTypes.h"

namespace cn
{

  class check_tx_outputs_visitor : public boost::static_visitor<bool>
  {
  public:
    check_tx_outputs_visitor(const Transaction &tx, uint32_t height, uint64_t amount,
                             const Currency &currency, std::string &error)
        : m_tx(tx), m_height(height), m_amount(amount),
          m_currency(currency), m_error(error) {}

    bool operator()(const KeyOutput &out) const
    {
      if (m_amount == 0)
      {
        m_error = "zero amount output";
        return false;
      }
      if (!check_key(out.key))
      {
        m_error = "output with invalid key";
        return false;
      }
      return true;
    }

    bool operator()(const MultisignatureOutput &out) const
    {
      if (m_tx.version < TRANSACTION_VERSION_2)
      {
        m_error = "contains multisignature output but has version ";
        m_error += std::to_string(m_tx.version);
        return false;
      }
      if (!m_currency.validateOutput(m_amount, out, m_height))
      {
        m_error = "contains invalid multisignature output";
        return false;
      }
      if (out.requiredSignatureCount > out.keys.size())
      {
        m_error = "contains multisignature with invalid required signature count";
        return false;
      }
      if (std::any_of(out.keys.begin(), out.keys.end(),
                      [](const crypto::PublicKey &key)
                      { return !check_key(key); }))
      {
        m_error = "contains multisignature output with invalid public key";
        return false;
      }
      return true;
    }

    bool operator()(const StandardPaymentOutput &out) const
    {
      if (m_amount == 0)
      {
        m_error = "zero amount standard payment output";
        return false;
      }
      if (!check_key(out.key))
      {
        m_error = "standard payment output with invalid key";
        return false;
      }
      return true;
    }

    bool operator()(const MultisigPaymentOutput &out) const
    {
      if (m_tx.version < TRANSACTION_VERSION_3)
      {
        m_error = "contains multisig payment output but has version ";
        m_error += std::to_string(m_tx.version);
        return false;
      }
      if (out.num_keys > out.keys.size())
      {
        m_error = "multisig payment output with invalid key count";
        return false;
      }
      if (std::any_of(out.keys.begin(), out.keys.end(),
                      [](const crypto::PublicKey &key)
                      { return !check_key(key); }))
      {
        m_error = "multisig payment output with invalid public key";
        return false;
      }
      if (out.isTimeLocked() && out.term == 0)
      {
        m_error = "time-locked multisig payment output with zero term";
        return false;
      }
      return true;
    }

    bool operator()(const DomainRegistrationOutput &out) const
    {
      // Domain registrations are non-spendable outputs.
      // Validate basic structure: domain must be non-empty, tier 1-3.
      if (out.domain.empty())
      {
        m_error = "domain registration with empty domain name";
        return false;
      }
      if (out.tier < 1 || out.tier > 3)
      {
        m_error = "domain registration with invalid tier";
        return false;
      }
      return true;
    }

    bool operator()(const DomainDeletionOutput &out) const
    {
      // Domain deletions are non-spendable outputs.
      // Validate basic structure: domain must be non-empty.
      if (out.domain.empty())
      {
        m_error = "domain deletion with empty domain name";
        return false;
      }
      return true;
    }

  private:
    const Transaction &m_tx;
    uint32_t m_height;
    uint64_t m_amount;
    const Currency &m_currency;
    std::string &m_error;
  };

} // namespace cn