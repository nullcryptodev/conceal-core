// IExecutor.h — Fiber/Thread executor interface
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <functional>
#include <future>
#include <memory>

namespace BoltHttp
{
  // A work item that can be dispatched to either a fiber or a thread
  using WorkItem = std::function<void()>;

  // Result of dispatching work — the caller can wait on it
  class IWorkToken
  {
  public:
    virtual ~IWorkToken() = default;
    virtual void wait() = 0;
    virtual bool ready() const = 0;
  };

  // Executor abstraction: fibers for I/O, threads for CPU
  class IExecutor
  {
  public:
    virtual ~IExecutor() = default;

    // Dispatch work to a thread pool. Returns a token the fiber can wait on.
    virtual std::unique_ptr<IWorkToken> dispatch(WorkItem work) = 0;

    // Run work inline on the current fiber/thread (fast path)
    virtual void runInline(WorkItem work) = 0;

    // Are we currently running on a fiber?
    virtual bool isFiberContext() const = 0;

    // Total number of worker threads available
    virtual size_t threadCount() const = 0;

    // Shutdown
    virtual void stop() = 0;
  };
}