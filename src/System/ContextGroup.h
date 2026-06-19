// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#if __has_include(<System/Dispatcher.h>)
#include <System/Dispatcher.h>
#else
#if defined(__linux__)
#include "../Platform/Linux/System/Dispatcher.h"
#elif defined(__APPLE__)
#include "../Platform/OSX/System/Dispatcher.h"
#elif defined(_WIN32)
#include "../Platform/Windows/System/Dispatcher.h"
#endif
#endif

namespace platform_system {

class ContextGroup {
public:
  explicit ContextGroup(Dispatcher& dispatcher);
  ContextGroup(const ContextGroup&) = delete;
  ContextGroup(ContextGroup&& other);
  ~ContextGroup();
  ContextGroup& operator=(const ContextGroup&) = delete;
  ContextGroup& operator=(ContextGroup&& other);
  void interrupt();
  void spawn(std::function<void()>&& procedure);
  void wait();

private:
  Dispatcher* m_dispatcher;
  NativeContextGroup m_contextGroup;
};

}
