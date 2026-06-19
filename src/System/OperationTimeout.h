// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <System/ContextGroup.h>
#include <System/Dispatcher.h>
#include <System/Timer.h>

namespace platform_system {

template<typename T> class OperationTimeout {
public:
  OperationTimeout(Dispatcher& dispatcher, T& object, std::chrono::nanoseconds timeout) :
    m_object(object), m_timerContext(dispatcher), m_timeoutTimer(dispatcher) {
    m_timerContext.spawn([this, timeout]() {
      try {
        m_timeoutTimer.sleep(timeout);
        m_timerContext.interrupt();
      } catch (std::exception&) {
      }
    });
  }

  ~OperationTimeout() {
    m_timerContext.interrupt();
    m_timerContext.wait();
  }

private:
  T& m_object;
  ContextGroup m_timerContext;
  Timer m_timeoutTimer;
};

}
