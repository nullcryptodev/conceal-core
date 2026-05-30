// FiberExecutor.h — Fiber-based executor using Dispatcher
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "IExecutor.h"
#include "ThreadPool.h"
#include <System/Dispatcher.h>
#include <memory>

namespace BoltHttp
{
  // Runs work on fibers, dispatches heavy work to a thread pool
  class FiberExecutor : public IExecutor
  {
  public:
    FiberExecutor(platform_system::Dispatcher &dispatcher,
                  std::shared_ptr<ThreadPool> threadPool);

    std::unique_ptr<IWorkToken> dispatch(WorkItem work) override;
    void runInline(WorkItem work) override;
    bool isFiberContext() const override { return true; }
    size_t threadCount() const override { return m_threadPool->threadCount(); }
    void stop() override {}

  private:
    platform_system::Dispatcher &m_dispatcher;
    std::shared_ptr<ThreadPool> m_threadPool;
  };
}