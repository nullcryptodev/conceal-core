// ThreadPool.h — Work-stealing thread pool for CPU-bound work
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "IExecutor.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <random>

namespace BoltHttp
{
  class ThreadPool : public IExecutor
  {
  public:
    explicit ThreadPool(size_t threadCount);
    ~ThreadPool();

    std::unique_ptr<IWorkToken> dispatch(WorkItem work) override;
    void runInline(WorkItem work) override;
    bool isFiberContext() const override { return false; }
    size_t threadCount() const override { return m_threads.size(); }
    void stop() override;

  private:
    struct PendingWork
    {
      WorkItem work;
      std::promise<void> promise;
    };

    void workerLoop(size_t workerId);
    std::shared_ptr<PendingWork> stealFrom(size_t thiefId);

    std::vector<std::thread> m_threads;
    std::atomic<bool> m_running{false};

    struct WorkerQueue
    {
      std::mutex mutex;
      std::queue<std::shared_ptr<PendingWork>> queue;
    };
    std::vector<WorkerQueue> m_queues;
  };
}