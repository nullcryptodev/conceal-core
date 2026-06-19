// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ContextGroup.h"
#include <cassert>

namespace platform_system {

ContextGroup::ContextGroup(Dispatcher& dispatcher) : m_dispatcher(&dispatcher) {
  m_contextGroup.firstContext = nullptr;
}

ContextGroup::ContextGroup(ContextGroup&& other) : m_dispatcher(other.m_dispatcher) {
  if (m_dispatcher != nullptr) {
    assert(other.m_contextGroup.firstContext == nullptr);
    m_contextGroup.firstContext = nullptr;
    other.m_dispatcher = nullptr;
  }
}

ContextGroup::~ContextGroup() {
  if (m_dispatcher != nullptr) {
    interrupt();
    wait();
  }
}

ContextGroup& ContextGroup::operator=(ContextGroup&& other) {
  assert(m_dispatcher == nullptr || m_contextGroup.firstContext == nullptr);
  m_dispatcher = other.m_dispatcher;
  if (m_dispatcher != nullptr) {
    assert(other.m_contextGroup.firstContext == nullptr);
    m_contextGroup.firstContext = nullptr;
    other.m_dispatcher = nullptr;
  }

  return *this;
}

void ContextGroup::interrupt() {
  assert(m_dispatcher != nullptr);
  for (NativeContext* context = m_contextGroup.firstContext; context != nullptr; context = context->groupNext) {
    m_dispatcher->interrupt(context);
  }
}

void ContextGroup::spawn(std::function<void()>&& procedure) {
  assert(m_dispatcher != nullptr);
  NativeContext& context = m_dispatcher->getReusableContext();
  if (m_contextGroup.firstContext != nullptr) {
    context.groupPrev = m_contextGroup.lastContext;
    assert(m_contextGroup.lastContext->groupNext == nullptr);
    m_contextGroup.lastContext->groupNext = &context;
  } else {
    context.groupPrev = nullptr;
    m_contextGroup.firstContext = &context;
    m_contextGroup.firstWaiter = nullptr;
  }

  context.interrupted = false;
  context.group = &m_contextGroup;
  context.groupNext = nullptr;
  context.procedure = std::move(procedure);
  m_contextGroup.lastContext = &context;
  m_dispatcher->pushContext(&context);
}

void ContextGroup::wait() {
  if (m_contextGroup.firstContext != nullptr) {
    NativeContext* context = m_dispatcher->getCurrentContext();
    context->next = nullptr;
    if (m_contextGroup.firstWaiter != nullptr) {
      assert(m_contextGroup.lastWaiter->next == nullptr);
      m_contextGroup.lastWaiter->next = context;
    } else {
      m_contextGroup.firstWaiter = context;
    }

    m_contextGroup.lastWaiter = context;
    m_dispatcher->dispatch();
    assert(context == m_dispatcher->getCurrentContext());
  }
}

}
