// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "P2pContextOwner.h"
#include <cassert>
#include "P2pContext.h"

namespace cn {

P2pContextOwner::P2pContextOwner(P2pContext* ctx, ContextList& contextList) : m_contextList(contextList) {
  m_contextIterator = m_contextList.insert(m_contextList.end(), ContextList::value_type(ctx));
}

P2pContextOwner::P2pContextOwner(P2pContextOwner&& other) : m_contextList(other.m_contextList), m_contextIterator(other.m_contextIterator) {
  other.m_contextIterator = m_contextList.end();
}

P2pContextOwner::~P2pContextOwner() {
  if (m_contextIterator != m_contextList.end()) {
    m_contextList.erase(m_contextIterator);
  }
}

P2pContext& P2pContextOwner::get() {
  assert(m_contextIterator != m_contextList.end());
  return *m_contextIterator->get();
}

P2pContext* P2pContextOwner::operator -> () {
  return &get();
}

}
