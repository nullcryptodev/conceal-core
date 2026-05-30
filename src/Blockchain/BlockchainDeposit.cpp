// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"

namespace cn {

  // Returns the total deposit amount currently locked
  uint64_t Blockchain::fullDepositAmount() const
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_depositIndex.fullDepositAmount();
  }

  // Returns the deposit amount at a specific height
  uint64_t Blockchain::depositAmountAtHeight(size_t height) const
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_depositIndex.depositAmountAtHeight(static_cast<DepositIndex::DepositHeight>(height));
  }

  // Returns the deposit interest accrued at a specific height
  uint64_t Blockchain::depositInterestAtHeight(size_t height) const
  {
    std::lock_guard<decltype(m_blockchain_lock)> lk(m_blockchain_lock);
    return m_depositIndex.depositInterestAtHeight(static_cast<DepositIndex::DepositHeight>(height));
  }

  // Adds a block's deposit outputs and interest to the deposit index
  void Blockchain::pushToDepositIndex(const BlockEntry &block, uint64_t interest)
  {
    int64_t deposit = 0;
    for (const auto &tx : block.transactions)
    {
      for (const auto &in : tx.tx.inputs)
        if (in.type() == typeid(MultisignatureInput))
        {
          auto &multisign = boost::get<MultisignatureInput>(in);
          if (multisign.term > 0)
            deposit -= multisign.amount;
        }
      for (const auto &out : tx.tx.outputs)
        if (out.target.type() == typeid(MultisignatureOutput))
        {
          auto &multisign = boost::get<MultisignatureOutput>(out.target);
          if (multisign.term > 0)
            deposit += out.amount;
        }
    }
    m_depositIndex.pushBlock(deposit, interest);
  }
}