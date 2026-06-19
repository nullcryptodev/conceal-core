// Copyright (c) 2012-2016, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "Timer.h"
#include <cassert>
#include <stdexcept>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "Dispatcher.h"
#include <System/ErrorMessage.h>
#include <System/InterruptedException.h>

namespace platform_system {

Timer::Timer() : m_dispatcher(nullptr) {
}

Timer::Timer(Dispatcher& dispatcher) : m_dispatcher(&dispatcher), m_context(nullptr), m_timer(-1) {
}

Timer::Timer(Timer&& other) : m_dispatcher(other.m_dispatcher) {
  if (other.m_dispatcher != nullptr) {
    assert(other.m_context == nullptr);
    m_timer = other.m_timer;
    m_context = nullptr;
    other.m_dispatcher = nullptr;
  }
}

Timer::~Timer() {
  assert(m_dispatcher == nullptr || m_context == nullptr);
}

Timer& Timer::operator=(Timer&& other) {
  assert(m_dispatcher == nullptr || m_context == nullptr);
  m_dispatcher = other.m_dispatcher;
  if (other.m_dispatcher != nullptr) {
    assert(other.m_context == nullptr);
    m_timer = other.m_timer;
    m_context = nullptr;
    other.m_dispatcher = nullptr;
    other.m_timer = -1;
  }

  return *this;
}

void Timer::sleep(std::chrono::nanoseconds duration) {
  assert(m_dispatcher != nullptr);
  assert(m_context == nullptr);
  if (m_dispatcher->interrupted()) {
    throw InterruptedException();
  }

  if(duration.count() == 0 ) {
    m_dispatcher->yield();
  } else {
    m_timer = m_dispatcher->getTimer();

    auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
    itimerspec expires;
    expires.it_interval.tv_nsec = expires.it_interval.tv_sec = 0;
    expires.it_value.tv_sec = seconds.count();
    expires.it_value.tv_nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds).count();
    timerfd_settime(m_timer, 0, &expires, NULL);

    ContextPair contextPair;
    OperationContext timerContext;
    timerContext.interrupted = false;
    timerContext.context = m_dispatcher->getCurrentContext();
    contextPair.writeContext = nullptr;
    contextPair.readContext = &timerContext;

    epoll_event timerEvent;
    timerEvent.events = EPOLLIN | EPOLLONESHOT;
    timerEvent.data.ptr = &contextPair;

    if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_timer, &timerEvent) == -1) {
      throw std::runtime_error("Timer::sleep, epoll_ctl failed, " + lastErrorMessage());
    }
    m_dispatcher->getCurrentContext()->interruptProcedure = [&]() {
        assert(m_dispatcher != nullptr);
        assert(m_context != nullptr);
        OperationContext* timerContext = static_cast<OperationContext*>(m_context);
        if (!timerContext->interrupted) {
          uint64_t value = 0;
          if(::read(m_timer, &value, sizeof value) == -1 ){
            if(errno == EAGAIN or EWOULDBLOCK) {
              timerContext->interrupted = true;
              m_dispatcher->pushContext(timerContext->context);
            } else {
              throw std::runtime_error("Timer::interrupt, read failed, "  + lastErrorMessage());
            }
          } else {
            assert(value>0);
            m_dispatcher->pushContext(timerContext->context);
          }

          epoll_event timerEvent;
          timerEvent.events = 0;
          timerEvent.data.ptr = nullptr;

          if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_timer, &timerEvent) == -1) {
            throw std::runtime_error("Timer::interrupt, epoll_ctl failed, " + lastErrorMessage());
          }
        }
    };

    m_context = &timerContext;
    m_dispatcher->dispatch();
    m_dispatcher->getCurrentContext()->interruptProcedure = nullptr;
    assert(m_dispatcher != nullptr);
    assert(timerContext.context == m_dispatcher->getCurrentContext());
    assert(contextPair.writeContext == nullptr);
    assert(m_context == &timerContext);
    m_context = nullptr;
    timerContext.context = nullptr;
    m_dispatcher->pushTimer(m_timer);
    if (timerContext.interrupted) {
      throw InterruptedException();
    }
  }
}

}
