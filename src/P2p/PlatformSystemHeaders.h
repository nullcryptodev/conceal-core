// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

// CMake adds src/Platform/<OS> to the include path, so <System/...> resolves at
// build time. When the IDE only indexes src/, use platform-relative fallbacks.

#if __has_include(<System/Dispatcher.h>)
#include <System/Dispatcher.h>
#include <System/TcpConnection.h>
#include <System/Timer.h>
#include <System/ContextGroup.h>
#include <System/Event.h>
#else
#if defined(__linux__)
#include "../Platform/Linux/System/Dispatcher.h"
#include "../Platform/Linux/System/TcpConnection.h"
#include "../Platform/Linux/System/Timer.h"
#elif defined(__APPLE__)
#include "../Platform/OSX/System/Dispatcher.h"
#include "../Platform/OSX/System/TcpConnection.h"
#include "../Platform/OSX/System/Timer.h"
#elif defined(_WIN32)
#include "../Platform/Windows/System/Dispatcher.h"
#include "../Platform/Windows/System/TcpConnection.h"
#include "../Platform/Windows/System/Timer.h"
#endif
#include "../System/ContextGroup.h"
#include "../System/Event.h"
#endif
