// GossipManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "GossipManager.h"
#include "BoltHttp/BoltSocket.hpp"

#include <cstring>
#include <iostream>
#include <algorithm>

namespace Sidechain
{
  namespace
  {
    int sendBytes(int fd, const void *data, size_t len)
    {
      return BoltHttp::boltSend(fd, static_cast<const char *>(data), len, BoltHttp::BOLT_MSG_NOSIGNAL);
    }

    int recvBytes(int fd, void *data, size_t len, int flags)
    {
      return BoltHttp::boltRecv(fd, static_cast<char *>(data), len, flags);
    }
  } // namespace

  GossipManager::GossipManager(uint16_t gossipPort, const std::vector<std::string> &seedNodes)
      : m_port(gossipPort), m_seedNodes(seedNodes), m_serverSocket(-1)
  {
  }

  GossipManager::~GossipManager()
  {
    stop();
  }

  void GossipManager::start()
  {
    if (m_running)
      return;
    m_running = true;

    m_serverSocket = BoltHttp::boltSocket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
      std::cerr << "GossipManager: Failed to create socket" << std::endl;
      return;
    }

    if (BoltHttp::boltSetReuseAddr(m_serverSocket) != 0)
    {
      std::cerr << "GossipManager: Failed to set SO_REUSEADDR" << std::endl;
      BoltHttp::boltClose(m_serverSocket);
      m_serverSocket = -1;
      return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (BoltHttp::boltBind(m_serverSocket, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
      std::cerr << "GossipManager: Failed to bind to port " << m_port << std::endl;
      return;
    }

    if (BoltHttp::boltListen(m_serverSocket, 10) < 0)
    {
      std::cerr << "GossipManager: Failed to listen on port " << m_port << std::endl;
      return;
    }

    m_threads.emplace_back(&GossipManager::acceptLoop, this);

    if (!m_seedNodes.empty())
      m_threads.emplace_back(&GossipManager::connectToSeeds, this);

    std::cout << "GossipManager: Listening on port " << m_port << std::endl;
  }

  void GossipManager::stop()
  {
    m_running = false;

    if (m_serverSocket >= 0)
    {
      BoltHttp::boltShutdown(m_serverSocket, BoltHttp::BOLT_SHUT_RDWR);
      BoltHttp::boltClose(m_serverSocket);
      m_serverSocket = -1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    {
      std::lock_guard<std::mutex> lock(m_peersMutex);
      for (int sock : m_peerSockets)
      {
        BoltHttp::boltShutdown(sock, BoltHttp::BOLT_SHUT_RDWR);
        BoltHttp::boltClose(sock);
      }
      m_peerSockets.clear();
    }

    for (auto &t : m_threads)
    {
      if (t.joinable())
        t.join();
    }
    m_threads.clear();
  }

  void GossipManager::broadcast(const std::vector<uint8_t> &message)
  {
    std::lock_guard<std::mutex> lock(m_peersMutex);
    std::vector<int> deadSockets;

    std::cout << "GossipManager::broadcast: sending to " << m_peerSockets.size() << " peers" << std::endl;
    for (int sock : m_peerSockets)
    {
      uint32_t len = htonl(static_cast<uint32_t>(message.size()));
      const int sent1 = sendBytes(sock, &len, sizeof(len));
      const int sent2 = sendBytes(sock, message.data(), message.size());
      std::cout << "GossipManager::broadcast: sent " << sent1 << "+" << sent2 << " bytes to socket " << sock << std::endl;

      if (sent1 <= 0 || sent2 <= 0)
      {
        std::cout << "GossipManager::broadcast: marking socket " << sock << " as dead" << std::endl;
        deadSockets.push_back(sock);
        BoltHttp::boltClose(sock);
      }
    }

    for (int dead : deadSockets)
      m_peerSockets.erase(std::remove(m_peerSockets.begin(), m_peerSockets.end(), dead), m_peerSockets.end());

    if (!deadSockets.empty())
      std::cout << "GossipManager::broadcast: cleaned up " << deadSockets.size() << " dead sockets" << std::endl;
  }

  void GossipManager::sendTo(const std::string &host, uint16_t port, const std::vector<uint8_t> &message)
  {
    int sock = BoltHttp::boltSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
      return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    BoltHttp::boltInetPton(AF_INET, host.c_str(), &addr.sin_addr);

    if (BoltHttp::boltConnect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
    {
      uint32_t len = htonl(static_cast<uint32_t>(message.size()));
      sendBytes(sock, &len, sizeof(len));
      sendBytes(sock, message.data(), message.size());
    }

    BoltHttp::boltShutdown(sock, BoltHttp::BOLT_SHUT_RDWR);
    BoltHttp::boltClose(sock);
  }

  void GossipManager::onMessage(MessageHandler handler)
  {
    m_messageHandler = handler;
  }

  std::vector<std::string> GossipManager::getPeers() const
  {
    return m_seedNodes;
  }

  void GossipManager::acceptLoop()
  {
    while (m_running)
    {
      sockaddr_in clientAddr{};
#ifdef _WIN32
      int clientLen = sizeof(clientAddr);
#else
      socklen_t clientLen = sizeof(clientAddr);
#endif
      const int clientSock = BoltHttp::boltAccept(
          m_serverSocket, reinterpret_cast<sockaddr *>(&clientAddr), &clientLen);

      if (clientSock < 0)
        continue;

      char clientIp[INET_ADDRSTRLEN];
      BoltHttp::boltInetNtop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
      const std::string peerAddr = std::string(clientIp) + ":" + std::to_string(ntohs(clientAddr.sin_port));

      std::lock_guard<std::mutex> lock(m_peersMutex);
      m_peerSockets.push_back(clientSock);
      m_threads.emplace_back(&GossipManager::handleConnection, this, clientSock, peerAddr);
    }
  }

  void GossipManager::connectToSeeds()
  {
    for (const auto &seed : m_seedNodes)
    {
      const size_t colon = seed.find(':');
      const std::string host = seed.substr(0, colon);
      const uint16_t port = static_cast<uint16_t>(std::stoi(seed.substr(colon + 1)));

      while (m_running)
      {
        int sock = BoltHttp::boltSocket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        BoltHttp::boltInetPton(AF_INET, host.c_str(), &addr.sin_addr);

        if (BoltHttp::boltConnect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0)
        {
          std::lock_guard<std::mutex> lock(m_peersMutex);
          m_peerSockets.push_back(sock);
          m_firstSeedAddress = seed;
          m_threads.emplace_back(&GossipManager::handleConnection, this, sock, seed);
          std::cout << "GossipManager: Connected to seed " << seed << std::endl;
          break;
        }

        BoltHttp::boltClose(sock);
        std::cerr << "GossipManager: Failed to connect to seed " << seed << ", retrying in 5s..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    }
  }

  void GossipManager::handleConnection(int socket, const std::string &peerAddress)
  {
    readLoop(socket, peerAddress);
    BoltHttp::boltClose(socket);
  }

  void GossipManager::readLoop(int socket, const std::string &peerAddress)
  {
    while (m_running)
    {
      uint32_t len = 0;
      int bytesRead = recvBytes(socket, &len, sizeof(len), 0);
      if (bytesRead <= 0)
        break;

      len = ntohl(len);
      std::vector<uint8_t> message(len);
      bytesRead = recvBytes(socket, message.data(), len, MSG_WAITALL);
      if (bytesRead <= 0)
        break;

      if (m_messageHandler)
        m_messageHandler(message, peerAddress);
    }
  }

  std::string GossipManager::getFirstSeedAddress() const
  {
    return m_firstSeedAddress;
  }
} // namespace Sidechain
