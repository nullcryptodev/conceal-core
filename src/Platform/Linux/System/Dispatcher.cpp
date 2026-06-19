// Copyright (c) 2012-2016, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#include "Dispatcher.h"

#include "ErrorMessage.h"
#include <cassert>
#include <fcntl.h>
#include <stdexcept>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <ucontext.h>
#include <unistd.h>
#include <pthread.h>

namespace platform_system {

namespace {

struct ContextMakingData {
  Dispatcher* dispatcher;
  void* ucontext;
};

class MutextGuard {
public:
  MutextGuard(pthread_mutex_t& _mutex) : mutex(_mutex) {
    auto ret = pthread_mutex_lock(&mutex);
    if (ret != 0) {
      throw std::runtime_error("pthread_mutex_lock failed, " + errorMessage(ret));
    }
  }

  ~MutextGuard() {
    pthread_mutex_unlock(&mutex);
  }

private:
  pthread_mutex_t& mutex;
};

static_assert(Dispatcher::SIZEOF_PTHREAD_MUTEX_T == sizeof(pthread_mutex_t), "invalid pthread mutex size");

//const size_t STACK_SIZE = 64 * 1024;
const size_t STACK_SIZE = 512 * 1024;

};

Dispatcher::Dispatcher() {
  std::string message;
  m_epoll = ::epoll_create1(0);
  if (m_epoll == -1) {
    message = "epoll_create1 failed, " + lastErrorMessage();
  } else {
    m_mainContext.ucontext = new ucontext_t;
    if (getcontext(reinterpret_cast<ucontext_t*>(m_mainContext.ucontext)) == -1) {
      message = "getcontext failed, " + lastErrorMessage();
    } else {
      m_remoteSpawnEvent = eventfd(0, O_NONBLOCK);
      if(m_remoteSpawnEvent == -1) {
        message = "eventfd failed, " + lastErrorMessage();
      } else {
        m_remoteSpawnEventContext.writeContext = nullptr;
        m_remoteSpawnEventContext.readContext = nullptr;

        epoll_event remoteSpawnEventEpollEvent;
        remoteSpawnEventEpollEvent.events = EPOLLIN;
        remoteSpawnEventEpollEvent.data.ptr = &m_remoteSpawnEventContext;

        if (epoll_ctl(m_epoll, EPOLL_CTL_ADD, m_remoteSpawnEvent, &remoteSpawnEventEpollEvent) == -1) {
          message = "epoll_ctl failed, " + lastErrorMessage();
        } else {
          *reinterpret_cast<pthread_mutex_t*>(this->m_mutex) = pthread_mutex_t(PTHREAD_MUTEX_INITIALIZER);

          m_mainContext.interrupted = false;
          m_mainContext.group = &m_contextGroup;
          m_mainContext.groupPrev = nullptr;
          m_mainContext.groupNext = nullptr;
          m_contextGroup.firstContext = nullptr;
          m_contextGroup.lastContext = nullptr;
          m_contextGroup.firstWaiter = nullptr;
          m_contextGroup.lastWaiter = nullptr;
          m_currentContext = &m_mainContext;
          m_firstResumingContext = nullptr;
          m_firstReusableContext = nullptr;
          m_runningContextCount = 0;
          return;
        }

        auto result = close(m_remoteSpawnEvent);
        assert(result == 0);
      }
    }

    auto result = close(m_epoll);
    assert(result == 0);
  }

  throw std::runtime_error("Dispatcher::Dispatcher, "+message);
}

Dispatcher::~Dispatcher() {
  for (NativeContext* context = m_contextGroup.firstContext; context != nullptr; context = context->groupNext) {
    interrupt(context);
  }

  yield();
  assert(m_contextGroup.firstContext == nullptr);
  assert(m_contextGroup.firstWaiter == nullptr);
  assert(m_firstResumingContext == nullptr);
  assert(m_runningContextCount == 0);
  while (m_firstReusableContext != nullptr) {
    auto ucontext = static_cast<ucontext_t*>(m_firstReusableContext->ucontext);
    auto stackPtr = static_cast<uint8_t *>(m_firstReusableContext->stackPtr);
    m_firstReusableContext = m_firstReusableContext->next;
    delete[] stackPtr;
    delete ucontext;
  }

  while (!m_timers.empty()) {
    int result = ::close(m_timers.top());
    assert(result == 0);
    m_timers.pop();
  }

  auto result = close(m_epoll);
  assert(result == 0);
  result = close(m_remoteSpawnEvent);
  assert(result == 0);
  result = pthread_mutex_destroy(reinterpret_cast<pthread_mutex_t*>(this->m_mutex));
  assert(result == 0);
}

void Dispatcher::clear() {
  while (m_firstReusableContext != nullptr) {
    auto ucontext = static_cast<ucontext_t*>(m_firstReusableContext->ucontext);
    auto stackPtr = static_cast<uint8_t *>(m_firstReusableContext->stackPtr);
    m_firstReusableContext = m_firstReusableContext->next;
    delete[] stackPtr;
    delete ucontext;
  }

  while (!m_timers.empty()) {
    int result = ::close(m_timers.top());
    if (result == -1) {
      throw std::runtime_error("Dispatcher::clear, close failed, "  + lastErrorMessage());
    }

    m_timers.pop();
  }
}

void Dispatcher::dispatch()
{
  NativeContext *context;
  for (;;)
  {
    if (m_firstResumingContext != nullptr)
    {
      context = m_firstResumingContext;
      m_firstResumingContext = context->next;
      break;
    }

    epoll_event event;
    int count = epoll_wait(m_epoll, &event, 1, 500); // 500ms timeout
    if (count == 1)
    {
      ContextPair *contextPair = static_cast<ContextPair *>(event.data.ptr);
      if (((event.events & (EPOLLIN | EPOLLOUT)) != 0) && contextPair->readContext == nullptr && contextPair->writeContext == nullptr)
      {
        uint64_t buf;
        auto transferred = read(m_remoteSpawnEvent, &buf, sizeof buf);
        if (transferred == -1)
        {
          throw std::runtime_error("Dispatcher::dispatch, read(remoteSpawnEvent) failed, " + lastErrorMessage());
        }

        MutextGuard guard(*reinterpret_cast<pthread_mutex_t *>(this->m_mutex));
        while (!m_remoteSpawningProcedures.empty())
        {
          spawn(std::move(m_remoteSpawningProcedures.front()));
          m_remoteSpawningProcedures.pop();
        }

        continue;
      }

      if ((event.events & EPOLLOUT) != 0)
      {
        context = contextPair->writeContext->context;
        contextPair->writeContext->events = event.events;
      }
      else if ((event.events & EPOLLIN) != 0)
      {
        context = contextPair->readContext->context;
        contextPair->readContext->events = event.events;
      }
      else
      {
        continue;
      }

      assert(context != nullptr);
      break;
    }
    else if (count == 0)
    {
      // timeout - just loop back and check timers / resuming contexts
      continue;
    }
    else
    {
      // count == -1, real error
      if (errno != EINTR)
      {
        throw std::runtime_error("Dispatcher::dispatch, epoll_wait failed, " + lastErrorMessage());
      }
      // EINTR - just retry
    }
  }

  if (context != m_currentContext)
  {
    ucontext_t *oldContext = static_cast<ucontext_t *>(m_currentContext->ucontext);
    m_currentContext = context;
    if (swapcontext(oldContext, static_cast<ucontext_t *>(context->ucontext)) == -1)
    {
      throw std::runtime_error("Dispatcher::dispatch, swapcontext failed, " + lastErrorMessage());
    }
  }
}

NativeContext* Dispatcher::getCurrentContext() const {
  return m_currentContext;
}

void Dispatcher::interrupt() {
  interrupt(m_currentContext);
}

void Dispatcher::interrupt(NativeContext* context) {
  assert(context!=nullptr);
  if (!context->interrupted) {
    if (context->interruptProcedure != nullptr) {
      context->interruptProcedure();
      context->interruptProcedure = nullptr;
    } else {
      context->interrupted = true;
    }
  }
}

bool Dispatcher::interrupted() {
  if (m_currentContext->interrupted) {
    m_currentContext->interrupted = false;
    return true;
  }

  return false;
}

void Dispatcher::pushContext(NativeContext* context) {
  assert(context != nullptr);
  context->next = nullptr;
  if(m_firstResumingContext != nullptr) {
    assert(m_lastResumingContext != nullptr);
    m_lastResumingContext->next = context;
  } else {
    m_firstResumingContext = context;
  }

  m_lastResumingContext = context;
}

void Dispatcher::remoteSpawn(std::function<void()>&& procedure) {
  {
    MutextGuard guard(*reinterpret_cast<pthread_mutex_t*>(this->m_mutex));
    m_remoteSpawningProcedures.push(std::move(procedure));
  }
  uint64_t one = 1;
  auto transferred = write(m_remoteSpawnEvent, &one, sizeof one);
  if(transferred == - 1) {
    throw std::runtime_error("Dispatcher::remoteSpawn, write failed, " + lastErrorMessage());
  }
}

void Dispatcher::spawn(std::function<void()>&& procedure) {
  NativeContext* context = &getReusableContext();
  if(m_contextGroup.firstContext != nullptr) {
    context->groupPrev = m_contextGroup.lastContext;
    assert(m_contextGroup.lastContext->groupNext == nullptr);
    m_contextGroup.lastContext->groupNext = context;
  } else {
    context->groupPrev = nullptr;
    m_contextGroup.firstContext = context;
    m_contextGroup.firstWaiter = nullptr;
  }

  context->interrupted = false;
  context->group = &m_contextGroup;
  context->groupNext = nullptr;
  context->procedure = std::move(procedure);
  m_contextGroup.lastContext = context;
  pushContext(context);
}

void Dispatcher::yield() {
  for(;;){
    epoll_event events[16];
    int count = epoll_wait(m_epoll, events, 16, 0);
    if (count == 0) {
      break;
    }

    if(count > 0) {
      for(int i = 0; i < count; ++i) {
        ContextPair *contextPair = static_cast<ContextPair*>(events[i].data.ptr);
        if(((events[i].events & (EPOLLIN | EPOLLOUT)) != 0) && contextPair->readContext == nullptr && contextPair->writeContext == nullptr) {
          uint64_t buf;
          auto transferred = read(m_remoteSpawnEvent, &buf, sizeof buf);
          if(transferred == -1) {
            throw std::runtime_error("Dispatcher::dispatch, read(remoteSpawnEvent) failed, " + lastErrorMessage());
          }

          MutextGuard guard(*reinterpret_cast<pthread_mutex_t*>(this->m_mutex));
          while (!m_remoteSpawningProcedures.empty()) {
            spawn(std::move(m_remoteSpawningProcedures.front()));
            m_remoteSpawningProcedures.pop();
          }

          continue;
        }

        if ((events[i].events & EPOLLOUT) != 0) {
          if(contextPair->writeContext != nullptr) {
            if(contextPair->writeContext->context != nullptr) {
              contextPair->writeContext->context->interruptProcedure = nullptr;
            }
          }
          pushContext(contextPair->writeContext->context);
          contextPair->writeContext->events = events[i].events;
        } else if ((events[i].events & EPOLLIN) != 0) {
          if(contextPair->readContext != nullptr) {
            if(contextPair->readContext->context != nullptr) {
              contextPair->readContext->context->interruptProcedure = nullptr;
            }
          }
          pushContext(contextPair->readContext->context);
          contextPair->readContext->events = events[i].events;
        } else if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0) {
          throw std::runtime_error("Dispatcher::dispatch, events & (EPOLLERR | EPOLLHUP) != 0");
        } else {
          continue;
        }
      }
    } else {
      if (errno != EINTR) {
        throw std::runtime_error("Dispatcher::dispatch, epoll_wait failed, " + lastErrorMessage());
      }
    }
  }

  if (m_firstResumingContext != nullptr) {
    pushContext(m_currentContext);
    dispatch();
  }
}

int Dispatcher::getEpoll() const {
  return m_epoll;
}

NativeContext& Dispatcher::getReusableContext() {
  if(m_firstReusableContext == nullptr) {
    ucontext_t* newlyCreatedContext = new ucontext_t;
    if (getcontext(newlyCreatedContext) == -1) { //makecontext precondition
      throw std::runtime_error("Dispatcher::getReusableContext, getcontext failed, " + lastErrorMessage());
    }

    auto stackPointer = new uint8_t[STACK_SIZE];
    newlyCreatedContext->uc_stack.ss_sp = stackPointer;
    newlyCreatedContext->uc_stack.ss_size = STACK_SIZE;

    ContextMakingData makingContextData {this, newlyCreatedContext};
    makecontext(newlyCreatedContext, (void(*)())contextProcedureStatic, 1, reinterpret_cast<int*>(&makingContextData));

    ucontext_t* oldContext = static_cast<ucontext_t*>(m_currentContext->ucontext);
    if (swapcontext(oldContext, newlyCreatedContext) == -1) {
      throw std::runtime_error("Dispatcher::getReusableContext, swapcontext failed, " + lastErrorMessage());
    }

    assert(m_firstReusableContext != nullptr);
    assert(m_firstReusableContext->ucontext == newlyCreatedContext);
    m_firstReusableContext->stackPtr = stackPointer;
  };

  NativeContext* context = m_firstReusableContext;
  m_firstReusableContext = m_firstReusableContext->next;
  return *context;
}

void Dispatcher::pushReusableContext(NativeContext& context) {
  context.next = m_firstReusableContext;
  m_firstReusableContext = &context;
  --m_runningContextCount;
}

int Dispatcher::getTimer() {
  int timer;
  if (m_timers.empty()) {
    timer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    epoll_event timerEvent;
    timerEvent.events = 0;
    timerEvent.data.ptr = nullptr;

    if (epoll_ctl(getEpoll(), EPOLL_CTL_ADD, timer, &timerEvent) == -1) {
      throw std::runtime_error("Dispatcher::getTimer, epoll_ctl failed, "  + lastErrorMessage());
    }
  } else {
    timer = m_timers.top();
    m_timers.pop();
  }

  return timer;
}

void Dispatcher::pushTimer(int timer) {
  m_timers.push(timer);
}

void Dispatcher::contextProcedure(void* ucontext) {
  assert(m_firstReusableContext == nullptr);
  NativeContext context;
  context.ucontext = ucontext;
  context.interrupted = false;
  context.next = nullptr;
  m_firstReusableContext = &context;
  ucontext_t* oldContext = static_cast<ucontext_t*>(context.ucontext);
  if (swapcontext(oldContext, static_cast<ucontext_t*>(m_currentContext->ucontext)) == -1) {
    throw std::runtime_error("Dispatcher::contextProcedure, swapcontext failed, " + lastErrorMessage());
  }

  for (;;) {
    ++m_runningContextCount;
    try {
      context.procedure();
    } catch(std::exception&) {
    }

    if (context.group != nullptr) {
      if (context.groupPrev != nullptr) {
        assert(context.groupPrev->groupNext == &context);
        context.groupPrev->groupNext = context.groupNext;
        if (context.groupNext != nullptr) {
          assert(context.groupNext->groupPrev == &context);
          context.groupNext->groupPrev = context.groupPrev;
        } else {
          assert(context.group->lastContext == &context);
          context.group->lastContext = context.groupPrev;
        }
      } else {
        assert(context.group->firstContext == &context);
        context.group->firstContext = context.groupNext;
        if (context.groupNext != nullptr) {
          assert(context.groupNext->groupPrev == &context);
          context.groupNext->groupPrev = nullptr;
        } else {
          assert(context.group->lastContext == &context);
          if (context.group->firstWaiter != nullptr) {
            if (m_firstResumingContext != nullptr) {
              assert(m_lastResumingContext->next == nullptr);
              m_lastResumingContext->next = context.group->firstWaiter;
            } else {
              m_firstResumingContext = context.group->firstWaiter;
            }

            m_lastResumingContext = context.group->lastWaiter;
            context.group->firstWaiter = nullptr;
          }
        }
      }

      pushReusableContext(context);
    }

    dispatch();
  }
};

void Dispatcher::contextProcedureStatic(void *context) {
  ContextMakingData* makingContextData = reinterpret_cast<ContextMakingData*>(context);
  makingContextData->dispatcher->contextProcedure(makingContextData->ucontext);
}

}
