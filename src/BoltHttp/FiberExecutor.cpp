// FiberExecutor.cpp — Fiber-based executor implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "FiberExecutor.h"

namespace BoltHttp
{
  FiberExecutor::FiberExecutor(platform_system::Dispatcher &dispatcher,
                               std::shared_ptr<ThreadPool> threadPool)
      : m_dispatcher(dispatcher), m_threadPool(std::move(threadPool))
  {
  }

  std::unique_ptr<IWorkToken> FiberExecutor::dispatch(WorkItem work)
  {
    return m_threadPool->dispatch(std::move(work));
  }

  void FiberExecutor::runInline(WorkItem work)
  {
    work();
  }
}