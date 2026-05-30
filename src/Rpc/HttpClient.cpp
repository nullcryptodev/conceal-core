// Copyright (c) 2012-2016, The CryptoNote developers, The Bytecoin developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// This file is part of Karbo.
//
// Karbo is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Karbo is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Karbo.  If not, see <http://www.gnu.org/licenses/>.

#include "HttpClient.h"

#include <chrono>
#include <thread>

#include <HTTP/HttpParser.h>
#include <System/Ipv4Resolver.h>
#include <System/TcpConnector.h>
#include <System/Ipv4Address.h>

namespace cn
{

  HttpClient::HttpClient(platform_system::Dispatcher &dispatcher, const std::string &address,
                         uint16_t port)
      : m_dispatcher(dispatcher), m_address(address), m_port(port) {}

  HttpClient::~HttpClient()
  {
    if (m_connected)
      disconnect();
  }

  void HttpClient::request(const HttpRequest &req, HttpResponse &res)
  {
    if (!m_connected)
      connect();

    try
    {
      std::iostream stream(m_streamBuf.get());
      HttpParser parser;
      stream << req;
      stream.flush();
      parser.receiveResponse(stream, res);
    }
    catch (const std::exception &)
    {
      disconnect();
      throw;
    }
  }

  bool HttpClient::connect()
  {
    try
    {
      platform_system::Ipv4Resolver resolver(m_dispatcher);
      auto addr = resolver.resolve(m_address);

      // Connect with a 3-second timeout to avoid hanging if the daemon
      // is unreachable. The platform's TcpConnector blocks, so we run it
      // in a detached thread and poll for completion.
      bool connected = false;
      std::exception_ptr connectException;

      std::thread connectThread([&]()
                                {
      try
      {
        m_connection = platform_system::TcpConnector(m_dispatcher).connect(addr, m_port);
        connected = true;
      }
      catch (...)
      {
        connectException = std::current_exception();
      } });

      connectThread.detach();

      auto start = std::chrono::steady_clock::now();
      while (!connected && !connectException)
      {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(3))
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      if (!connected)
      {
        m_connection = platform_system::TcpConnection();
        return false;
      }

      if (connectException)
        std::rethrow_exception(connectException);

      m_streamBuf.reset(new platform_system::TcpStreambuf(m_connection));
      m_connected = true;
      return true;
    }
    catch (const std::exception &)
    {
      m_connection = platform_system::TcpConnection();
      return false;
    }
  }

  bool HttpClient::isConnected() const
  {
    return m_connected;
  }

  void HttpClient::disconnect()
  {
    m_streamBuf.reset();

    try
    {
      m_connection.write(nullptr, 0);
    }
    catch (const std::exception &)
    {
    }

    m_connection = platform_system::TcpConnection();
    m_connected = false;
  }

  ConnectException::ConnectException(const std::string &whatArg)
      : std::runtime_error(whatArg.c_str()) {}

} // namespace cn