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

#include "TcpConnection.h"

#include <System/ErrorMessage.h>
#include <System/InterruptedException.h>
#include <System/Ipv4Address.h>
#include <arpa/inet.h>
#include <cassert>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>

namespace platform_system {

TcpConnection::TcpConnection() : m_dispatcher(nullptr) {
}

TcpConnection::TcpConnection(TcpConnection&& other) : m_dispatcher(other.m_dispatcher) {
  if (other.m_dispatcher != nullptr) {
    assert(other.m_contextPair.writeContext == nullptr);
    assert(other.m_contextPair.readContext == nullptr);
    m_connection = other.m_connection;
    m_contextPair = other.m_contextPair;
    other.m_dispatcher = nullptr;
  }
}

TcpConnection::~TcpConnection() {
  if (m_dispatcher != nullptr) {
    assert(m_contextPair.readContext == nullptr);
    assert(m_contextPair.writeContext == nullptr);
    int result = close(m_connection);
    assert(result != -1);
  }
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) {
  if (m_dispatcher != nullptr) {
    assert(m_contextPair.readContext == nullptr);
    assert(m_contextPair.writeContext == nullptr);
    if (close(m_connection) == -1) {
      throw std::runtime_error("TcpConnection::operator=, close failed, " + lastErrorMessage());
    }
  }

  m_dispatcher = other.m_dispatcher;
  if (other.m_dispatcher != nullptr) {
    assert(other.m_contextPair.readContext == nullptr);
    assert(other.m_contextPair.writeContext == nullptr);
    m_connection = other.m_connection;
    m_contextPair = other.m_contextPair;
    other.m_dispatcher = nullptr;
  }

  return *this;
}

size_t TcpConnection::read(uint8_t* data, size_t size) {
  assert(m_dispatcher != nullptr);
  assert(m_contextPair.readContext == nullptr);
  if (m_dispatcher->interrupted()) {
    throw InterruptedException();
  }

  std::string message;
  ssize_t transferred = ::recv(m_connection, (void *)data, size, 0);
  if (transferred == -1) {
    if (errno != EAGAIN) {
      message = "recv failed, " + lastErrorMessage();
    } else {
      epoll_event connectionEvent;
      OperationContext operationContext;
      operationContext.interrupted = false;
      operationContext.context = m_dispatcher->getCurrentContext();
      m_contextPair.readContext = &operationContext;
      connectionEvent.data.ptr = &m_contextPair;

      if(m_contextPair.writeContext != nullptr) {
        connectionEvent.events = EPOLLIN | EPOLLOUT | EPOLLONESHOT;
      } else {
        connectionEvent.events = EPOLLIN | EPOLLONESHOT;
      }

      if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_connection, &connectionEvent) == -1) {
        message = "epoll_ctl failed, " + lastErrorMessage();
      } else {
        m_dispatcher->getCurrentContext()->interruptProcedure = [&]() {
            assert(m_dispatcher != nullptr);
            assert(m_contextPair.readContext != nullptr);
            epoll_event connectionEvent;
            connectionEvent.events = 0;
            connectionEvent.data.ptr = nullptr;

            if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_connection, &connectionEvent) == -1) {
              throw std::runtime_error("TcpConnection::stop, epoll_ctl failed, " + lastErrorMessage());
            }

            m_contextPair.readContext->interrupted = true;
            m_dispatcher->pushContext(m_contextPair.readContext->context);
        };

        m_dispatcher->dispatch();
        m_dispatcher->getCurrentContext()->interruptProcedure = nullptr;
        assert(m_dispatcher != nullptr);
        assert(operationContext.context == m_dispatcher->getCurrentContext());
        assert(m_contextPair.readContext == &operationContext);

        if (operationContext.interrupted) {
          m_contextPair.readContext = nullptr;
          throw InterruptedException();
        }

        m_contextPair.readContext = nullptr;
        if(m_contextPair.writeContext != nullptr) { //write is presented, rearm
          epoll_event connectionEvent;
          connectionEvent.events = EPOLLOUT | EPOLLONESHOT;
          connectionEvent.data.ptr = &m_contextPair;

          if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_connection, &connectionEvent) == -1) {
            message = "epoll_ctl failed, " + lastErrorMessage();
            throw std::runtime_error("TcpConnection::read");
          }
        }

        if((operationContext.events & (EPOLLERR | EPOLLHUP)) != 0) {
          throw std::runtime_error("TcpConnection::read");
        }

        ssize_t transferred = ::recv(m_connection, (void *)data, size, 0);
        if (transferred == -1) {
          message = "recv failed, " + lastErrorMessage();
        } else {
          assert(transferred <= static_cast<ssize_t>(size));
          return transferred;
        }
      }
    }

    throw std::runtime_error("TcpConnection::read, "+ message);
  }

  assert(transferred <= static_cast<ssize_t>(size));
  return transferred;
}

std::size_t TcpConnection::write(const uint8_t* data, size_t size) {
  assert(m_dispatcher != nullptr);
  assert(m_contextPair.writeContext == nullptr);
  if (m_dispatcher->interrupted()) {
    throw InterruptedException();
  }

  std::string message;
  if(size == 0) {
    if(shutdown(m_connection, SHUT_WR) == -1) {
      throw std::runtime_error("TcpConnection::write, shutdown failed, " + lastErrorMessage());
    }

    return 0;
  }

  ssize_t transferred = ::send(m_connection, (void *)data, size, MSG_NOSIGNAL);
  if (transferred == -1) {
    if (errno != EAGAIN) {
      message = "send failed, " + lastErrorMessage();
    } else {
      epoll_event connectionEvent;
      OperationContext operationContext;
      operationContext.interrupted = false;
      operationContext.context = m_dispatcher->getCurrentContext();
      m_contextPair.writeContext = &operationContext;
      connectionEvent.data.ptr = &m_contextPair;

      if(m_contextPair.readContext != nullptr) {
        connectionEvent.events = EPOLLIN | EPOLLOUT | EPOLLONESHOT;
      } else {
        connectionEvent.events = EPOLLOUT | EPOLLONESHOT;
      }

      if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_connection, &connectionEvent) == -1) {
        message = "epoll_ctl failed, " + lastErrorMessage();
      } else {
        m_dispatcher->getCurrentContext()->interruptProcedure = [&]() {
            assert(m_dispatcher != nullptr);
            assert(m_contextPair.writeContext != nullptr);
            epoll_event connectionEvent;
            connectionEvent.events = 0;
            connectionEvent.data.ptr = nullptr;

            if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_connection, &connectionEvent) == -1) {
              throw std::runtime_error("TcpConnection::stop, epoll_ctl failed, " + lastErrorMessage());
            }

            m_contextPair.writeContext->interrupted = true;
            m_dispatcher->pushContext(m_contextPair.writeContext->context);
        };

        m_dispatcher->dispatch();
        m_dispatcher->getCurrentContext()->interruptProcedure = nullptr;
        assert(m_dispatcher != nullptr);
        assert(operationContext.context == m_dispatcher->getCurrentContext());
        assert(m_contextPair.writeContext == &operationContext);

        if (operationContext.interrupted) {
          m_contextPair.writeContext = nullptr;
          throw InterruptedException();
        }

        m_contextPair.writeContext = nullptr;
        if(m_contextPair.readContext != nullptr) { //read is presented, rearm
          epoll_event connectionEvent;
          connectionEvent.events = EPOLLIN | EPOLLONESHOT;
          connectionEvent.data.ptr = &m_contextPair;

          if (epoll_ctl(m_dispatcher->getEpoll(), EPOLL_CTL_MOD, m_connection, &connectionEvent) == -1) {
            message = "epoll_ctl failed, " + lastErrorMessage();
            throw std::runtime_error("TcpConnection::write, " + message);
          }
        }

        if((operationContext.events & (EPOLLERR | EPOLLHUP)) != 0) {
          throw std::runtime_error("TcpConnection::write, events & (EPOLLERR | EPOLLHUP) != 0");
        }

        ssize_t transferred = ::send(m_connection, (void *)data, size, 0);
        if (transferred == -1) {
          message = "send failed, "  + lastErrorMessage();
        } else {
          assert(transferred <= static_cast<ssize_t>(size));
          return transferred;
        }
      }
    }

    throw std::runtime_error("TcpConnection::write, " + message);
  }

  assert(transferred <= static_cast<ssize_t>(size));
  return transferred;
}

std::pair<Ipv4Address, uint16_t> TcpConnection::getPeerAddressAndPort() const {
  sockaddr_in addr;
  socklen_t size = sizeof(addr);
  if (getpeername(m_connection, reinterpret_cast<sockaddr*>(&addr), &size) != 0) {
    throw std::runtime_error("TcpConnection::getPeerAddress, getpeername failed, " + lastErrorMessage());
  }

  assert(size == sizeof(sockaddr_in));
  return std::make_pair(Ipv4Address(htonl(addr.sin_addr.s_addr)), htons(addr.sin_port));
}

TcpConnection::TcpConnection(Dispatcher& dispatcher, int socket) : m_dispatcher(&dispatcher), m_connection(socket) {
  m_contextPair.readContext = nullptr;
  m_contextPair.writeContext = nullptr;
  epoll_event connectionEvent;
  connectionEvent.events = EPOLLONESHOT;
  connectionEvent.data.ptr = nullptr;

  if (epoll_ctl(dispatcher.getEpoll(), EPOLL_CTL_ADD, socket, &connectionEvent) == -1) {
    throw std::runtime_error("TcpConnection::TcpConnection, epoll_ctl failed, " + lastErrorMessage());
  }
}

}
