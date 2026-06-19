// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Event.h"
#include <cassert>
#include <System/Dispatcher.h>
#include <System/InterruptedException.h>

namespace platform_system {

namespace {

struct EventWaiter {
  bool interrupted;
  EventWaiter* prev;
  EventWaiter* next;
  NativeContext* context;
};

}

Event::Event() : m_dispatcher(nullptr) {
}

Event::Event(Dispatcher& dispatcher) : m_dispatcher(&dispatcher), m_state(false), m_first(nullptr) {
}

Event::Event(Event&& other) : m_dispatcher(other.m_dispatcher) {
  if (m_dispatcher != nullptr) {
    m_state = other.m_state;
    if (!m_state) {
      assert(other.m_first == nullptr);
      m_first = nullptr;
    }

    other.m_dispatcher = nullptr;
  }
}

Event::~Event() {
  assert(m_dispatcher == nullptr || m_state || m_first == nullptr);
}

Event& Event::operator=(Event&& other) {
  assert(m_dispatcher == nullptr || m_state || m_first == nullptr);
  m_dispatcher = other.m_dispatcher;
  if (m_dispatcher != nullptr) {
    m_state = other.m_state;
    if (!m_state) {
      assert(other.m_first == nullptr);
      m_first = nullptr;
    }

    other.m_dispatcher = nullptr;
  }

  return *this;
}

bool Event::get() const {
  assert(m_dispatcher != nullptr);
  return m_state;
}

void Event::clear() {
  assert(m_dispatcher != nullptr);
  if (m_state) {
    m_state = false;
    m_first = nullptr;
  }
}

void Event::set() {
  assert(m_dispatcher != nullptr);
  if (!m_state) {
    m_state = true;
    for (EventWaiter* waiter = static_cast<EventWaiter*>(m_first); waiter != nullptr; waiter = waiter->next) {
      waiter->context->interruptProcedure = nullptr;
      m_dispatcher->pushContext(waiter->context);
    }
  }
}

void Event::wait() {
  assert(m_dispatcher != nullptr);
  if (m_dispatcher->interrupted()) {
    throw InterruptedException();
  }

  if (!m_state) {
    EventWaiter waiter = { false, nullptr, nullptr, m_dispatcher->getCurrentContext() };
    waiter.context->interruptProcedure = [&] {
      if (waiter.next != nullptr) {
        assert(waiter.next->prev == &waiter);
        waiter.next->prev = waiter.prev;
      } else {
        assert(m_last == &waiter);
        m_last = waiter.prev;
      }

      if (waiter.prev != nullptr) { 
        assert(waiter.prev->next == &waiter);
        waiter.prev->next = waiter.next;
      } else {
        assert(m_first == &waiter);
        m_first = waiter.next;
      }

      assert(!waiter.interrupted);
      waiter.interrupted = true;
      m_dispatcher->pushContext(waiter.context);
    };

    if (m_first != nullptr) {
      static_cast<EventWaiter*>(m_last)->next = &waiter;
      waiter.prev = static_cast<EventWaiter*>(m_last);
    } else {
      m_first = &waiter;
    }

    m_last = &waiter;
    m_dispatcher->dispatch();
    assert(waiter.context == m_dispatcher->getCurrentContext());
    assert( waiter.context->interruptProcedure == nullptr);
    assert(m_dispatcher != nullptr);
    if (waiter.interrupted) {
      throw InterruptedException();
    } 
  }
}

}
