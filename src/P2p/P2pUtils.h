// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <chrono>
#include <functional>

namespace platform_system {
class Dispatcher;
}

namespace cn {

/**
 * Runs f() inside a ContextGroup with a timeout.
 * Throws std::runtime_error if f() throws, is interrupted, or times out.
 * Used by both Layer 1 (NodeServer) and Layer 2 (P2pNode) for timed operations.
 */
void doWithTimeoutAndThrow(platform_system::Dispatcher& dispatcher,
                           std::chrono::nanoseconds timeout,
                           std::function<void()> f);

}
