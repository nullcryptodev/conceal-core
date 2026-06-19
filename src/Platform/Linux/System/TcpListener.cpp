// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "TcpListener.h"
#include <cassert>
#include <stdexcept>

#include <fcntl.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <string.h>

#include "Dispatcher.h"
#include "TcpConnection.h"
#include <System/ErrorMessage.h>
#include <System/InterruptedException.h>
#include <System/Ipv4Address.h>

namespace platform_system {

TcpListener::TcpListener() : m_dispatcher(nullptr) {
}

TcpListener::TcpListener(Dispatcher& dispatcher, const Ipv4Address& addr, uint16_t port) : m_dispatcher(&dispatcher) {
  std::string message;
  m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (m_listener == -1) {
    message = "socket failed, " + lastErrorMessage();
  } else {
    int flags = fcntl(m_listener, F_GETFL, 0);
    if (flags == -1 || fcntl(m_listener, F_SETFL, flags | O_NONBLOCK) == -1) {
      message = "fcntl failed, " + lastErrorMessage();
    } else {
      int on = 1;
      if (setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == -1) {
        message = "setsockopt failed, " + lastErrorMessage();
      } else {
        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl( addr.getValue());
        if (bind(m_listener, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) {
          message = "bind failed, " + lastErrorMessage();
        } else if (listen(m_listener, SOMAXCONN) != 0) {
          message = "listen failed, " + lastErrorMessage();
        } else {
          epoll_event listenEvent;
          listenEvent.events = 0;
          listenEvent.data.ptr = nullptr;

          if (epoll_ctl(dispatcher.getEpoll(), EPOLL_CTL_ADD, m_listener, &listenEvent) == -1) {
            message = "epoll_ctl failed, " + lastErrorMessage();
          } else {
            m_context = nullptr;
            return;
          }
        }
      }
    }

    int result = close(m_listener);
    assert(result != -1);
  }

  throw std::runtime_error("TcpListener::TcpListener, " + message);
}

TcpListener::TcpListener(TcpListener&& other) : m_dispatcher(other.m_dispatcher) {
  if (other.m_dispatcher != nullptr) {
    assert(other.m_context == nullptr);
    m_listener = other.m_listener;
    m_context = nullptr;
    other.m_dispatcher = nullptr;
  }
}

TcpListener::~TcpListener() {
  if (m_dispatcher != nullptr) {
    assert(m_context == nullptr);
    int result = close(m_listener);
    assert(result != -1);
  }
}

TcpListener& TcpListener::operator=(TcpListener&& other) {
  if (m_dispatcher != nullptr) {
    assert(m_context == nullptr);
    if (close(m_listener) == -1) {
      throw std::runtime_error("TcpListener::operator=, close failed, " + lastErrorMessage());
    }
  }

  m_dispatcher = other.m_dispatcher;
  if (other.m_dispatcher != nullptr) {
    assert(other.m_context == nullptr);
    m_listener = other.m_listener;
    m_context = nullptr;
    other.m_dispatcher = nullptr;
  }

  return *this;
}

TcpConnection TcpListener::accept() {
  assert(m_dispatcher != nullptr);
  assert(m_context == nullptr);
  if (m_dispatcher->interrupted()) {
    throw InterruptedException();
  }

  ContextPair contextPair;
  OperationContext listenerContext;
  listenerContext.interrupted = false;
  listenerContext.context = m_dispatcher->getCurrentContext();

  contextPair.writeContext = nullptr;
  contextPair.readContext = &listenerContext;

  epoll_event listenEvent;
  listenEvent.events = EPOLLIN | EPOLLONESHOT;
  listenEvent.data.ptr = &contextPair;
  std::string message;
  if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_listener, &listenEvent) == -1) {
    message = "epoll_ctl failed, " + lastErrorMessage();
  } else {
    m_context = &listenerContext;
    m_dispatcher->getCurrentContext()->interruptProcedure = [&]() {
        assert(m_dispatcher != nullptr);
        assert(m_context != nullptr);
        OperationContext* listenerContext = static_cast<OperationContext*>(m_context);
        if (!listenerContext->interrupted) {
          epoll_event listenEvent;
          listenEvent.events = 0;
          listenEvent.data.ptr = nullptr;

          if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_listener, &listenEvent) == -1) {
            throw std::runtime_error("TcpListener::stop, epoll_ctl failed, " + lastErrorMessage() );
          }

          listenerContext->interrupted = true;
          m_dispatcher->pushContext(listenerContext->context);
        }
    };

    m_dispatcher->dispatch();
    m_dispatcher->getCurrentContext()->interruptProcedure = nullptr;
    assert(m_dispatcher != nullptr);
    assert(listenerContext.context == m_dispatcher->getCurrentContext());
    assert(contextPair.writeContext == nullptr);
    assert(m_context == &listenerContext);
    m_context = nullptr;
    listenerContext.context = nullptr;
    if (listenerContext.interrupted) {
      throw InterruptedException();
    }

    if((listenerContext.events & (EPOLLERR | EPOLLHUP)) != 0) {
      throw std::runtime_error("TcpListener::accept, accepting failed");
    }

    sockaddr inAddr;
    socklen_t inLen = sizeof(inAddr);
    int connection = ::accept(m_listener, &inAddr, &inLen);
    if (connection == -1) {
      message = "accept failed, " + lastErrorMessage();
    } else {
      int flags = fcntl(connection, F_GETFL, 0);
      if (flags == -1 || fcntl(connection, F_SETFL, flags | O_NONBLOCK) == -1) {
        message = "fcntl failed, " + lastErrorMessage();
      } else {
        return TcpConnection(*m_dispatcher, connection);
      }

      int result = close(connection);
      assert(result != -1);
    }
  }

  throw std::runtime_error("TcpListener::accept, " + message);
}

}
