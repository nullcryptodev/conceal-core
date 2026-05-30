// ThreadPool.cpp — Work-stealing thread pool implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "ThreadPool.h"
#include <random>
#include <iostream>

namespace BoltHttp
{
  namespace
  {
    class WorkToken : public IWorkToken
    {
    public:
      explicit WorkToken(std::shared_future<void> future)
          : m_future(std::move(future)) {}

      void wait() override { m_future.wait(); }
      bool ready() const override
      {
        return m_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
      }

    private:
      std::shared_future<void> m_future;
    };
  }

  ThreadPool::ThreadPool(size_t threadCount)
      : m_queues(threadCount)
  {
    m_running = true;
    for (size_t i = 0; i < threadCount; ++i)
      m_threads.emplace_back(&ThreadPool::workerLoop, this, i);
  }

  ThreadPool::~ThreadPool()
  {
    stop();
  }

  void ThreadPool::stop()
  {
    m_running = false;
    for (auto &t : m_threads)
    {
      if (t.joinable())
        t.join();
    }
    m_threads.clear();
  }

  std::unique_ptr<IWorkToken> ThreadPool::dispatch(WorkItem work)
  {
    auto pending = std::make_shared<PendingWork>();
    pending->work = std::move(work);
    auto future = pending->promise.get_future().share();

    static thread_local std::mt19937 rng(std::random_device{}());
    size_t target = std::uniform_int_distribution<size_t>(0, m_queues.size() - 1)(rng);

    {
      std::lock_guard<std::mutex> lock(m_queues[target].mutex);
      m_queues[target].queue.push(pending);
    }

    return std::unique_ptr<WorkToken>(new WorkToken(future));
  }

  void ThreadPool::runInline(WorkItem work)
  {
    work();
  }

  std::shared_ptr<ThreadPool::PendingWork> ThreadPool::stealFrom(size_t thiefId)
  {
    static thread_local std::mt19937 rng(std::random_device{}());
    size_t victim = thiefId;

    for (size_t attempts = 0; attempts < m_queues.size(); ++attempts)
    {
      victim = (victim + 1) % m_queues.size();
      if (victim == thiefId)
        continue;

      std::lock_guard<std::mutex> lock(m_queues[victim].mutex);
      if (!m_queues[victim].queue.empty())
      {
        auto work = m_queues[victim].queue.front();
        m_queues[victim].queue.pop();
        return work;
      }
    }
    return nullptr;
  }

  void ThreadPool::workerLoop(size_t workerId)
  {
    while (m_running)
    {
      std::shared_ptr<PendingWork> work;

      {
        std::lock_guard<std::mutex> lock(m_queues[workerId].mutex);
        if (!m_queues[workerId].queue.empty())
        {
          work = m_queues[workerId].queue.front();
          m_queues[workerId].queue.pop();
        }
      }

      if (!work)
        work = stealFrom(workerId);

      if (work)
      {
        work->work();
        work->promise.set_value();
      }
      else
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }
}