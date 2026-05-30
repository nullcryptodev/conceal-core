// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "SignalHandler.h"

#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <signal.h>
#include <cstring>
#include <unistd.h>
#include <thread>
#endif

namespace {

  std::function<void(void)> m_handler;

#if defined(WIN32)

  void handleSignal() {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0 && m_handler) {
      m_handler();
    }
  }

  BOOL WINAPI winHandler(DWORD type) {
    if (CTRL_C_EVENT == type || CTRL_BREAK_EVENT == type) {
      handleSignal();
      return TRUE;
    }
    std::cerr << "Got control signal " << type << ". Exiting without saving...";
    return FALSE;
  }

#else

  // Self-pipe: only write() is async-signal-safe.
  // Signal handler writes one byte; a watcher thread reads it and invokes the callback.
  int g_pipeFd[2] = {-1, -1};

  void posixHandler(int /*type*/) {
    char c = 0;
    // write() is async-signal-safe; errors intentionally ignored here.
    (void)write(g_pipeFd[1], &c, 1);
  }

#endif

} // namespace


namespace tools {

  bool SignalHandler::install(std::function<void(void)> t)
  {
#if defined(WIN32)
    bool r = TRUE == ::SetConsoleCtrlHandler(&winHandler, TRUE);
    if (r) {
      m_handler = t;
    }
    return r;
#else
    if (pipe(g_pipeFd) != 0) {
      return false;
    }

    m_handler = std::move(t);

    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = posixHandler;
    // SA_RESTART deliberately omitted so nanosleep/read return EINTR on signal.
    if (sigaction(SIGINT, &sa, nullptr) != 0) {
      return false;
    }
    if (sigaction(SIGTERM, &sa, nullptr) != 0) {
      return false;
    }

    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, nullptr) != 0) {
      return false;
    }

    // Watcher thread: blocks on the read end of the pipe.
    // Invoked at most once; detaches so it outlives install() scope.
    std::thread([] {
      char c;
      if (read(g_pipeFd[0], &c, 1) == 1 && m_handler) {
        m_handler();
      }
      close(g_pipeFd[0]);
      close(g_pipeFd[1]);
      g_pipeFd[0] = g_pipeFd[1] = -1;
    }).detach();

    return true;
#endif
  }

}
