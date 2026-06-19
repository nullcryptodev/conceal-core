// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "P2pUtils.h"

#include <System/ContextGroup.h>
#include <System/ContextGroupTimeout.h>
#include <System/Dispatcher.h>
#include <System/InterruptedException.h>

#include <stdexcept>
#include <string>

namespace cn {

void doWithTimeoutAndThrow(platform_system::Dispatcher& dispatcher,
                           std::chrono::nanoseconds timeout,
                           std::function<void()> f) {
  std::string result;
  platform_system::ContextGroup cg(dispatcher);
  platform_system::ContextGroupTimeout cgTimeout(dispatcher, cg, timeout);

  cg.spawn([&] {
    try {
      f();
    } catch (platform_system::InterruptedException&) {
      result = "Operation timeout";
    } catch (std::exception& e) {
      result = e.what();
    }
  });

  cg.wait();

  if (!result.empty()) {
    throw std::runtime_error(result);
  }
}

}
