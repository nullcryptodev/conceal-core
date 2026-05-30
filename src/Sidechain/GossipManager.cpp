// GossipManager implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "GossipManager.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <algorithm>

namespace Sidechain
{
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

    // Create server socket
    m_serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverSocket < 0)
    {
      std::cerr << "GossipManager: Failed to create socket" << std::endl;
      return;
    }

    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_port);

    if (bind(m_serverSocket, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
      std::cerr << "GossipManager: Failed to bind to port " << m_port << std::endl;
      return;
    }

    listen(m_serverSocket, 10);

    // Start accept thread
    m_threads.emplace_back(&GossipManager::acceptLoop, this);

    // Connect to seed nodes
    if (!m_seedNodes.empty())
    {
      m_threads.emplace_back(&GossipManager::connectToSeeds, this);
    }

    std::cout << "GossipManager: Listening on port " << m_port << std::endl;
  }

  void GossipManager::stop()
  {
    m_running = false;

    // Close server socket immediately so acceptLoop exits
    if (m_serverSocket >= 0)
    {
      shutdown(m_serverSocket, SHUT_RDWR);
      close(m_serverSocket);
      m_serverSocket = -1;
    }

    // Give acceptLoop a moment to see m_running is false and exit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Close all peer sockets so readLoop threads wake up
    {
      std::lock_guard<std::mutex> lock(m_peersMutex);
      for (int sock : m_peerSockets)
      {
        shutdown(sock, SHUT_RDWR);
        close(sock);
      }
      m_peerSockets.clear();
    }

    // Now join all threads
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
      uint32_t len = htonl(message.size());
      ssize_t sent1 = send(sock, &len, sizeof(len), MSG_NOSIGNAL);
      ssize_t sent2 = send(sock, message.data(), message.size(), MSG_NOSIGNAL);
      std::cout << "GossipManager::broadcast: sent " << sent1 << "+" << sent2 << " bytes to socket " << sock << std::endl;

      if (sent1 <= 0 || sent2 <= 0)
      {
        std::cout << "GossipManager::broadcast: marking socket " << sock << " as dead" << std::endl;
        deadSockets.push_back(sock);
        close(sock);
      }
    }

    // Remove dead sockets from the peer list
    for (int dead : deadSockets)
    {
      m_peerSockets.erase(std::remove(m_peerSockets.begin(), m_peerSockets.end(), dead), m_peerSockets.end());
    }

    if (!deadSockets.empty())
      std::cout << "GossipManager::broadcast: cleaned up " << deadSockets.size() << " dead sockets" << std::endl;
  }

  void GossipManager::sendTo(const std::string &host, uint16_t port, const std::vector<uint8_t> &message)
  {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
      return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) == 0)
    {
      uint32_t len = htonl(message.size());
      send(sock, &len, sizeof(len), 0);
      send(sock, message.data(), message.size(), 0);
    }

    // Shutdown before close to prevent epoll errors
    shutdown(sock, SHUT_RDWR);
    close(sock);
  }

  void GossipManager::onMessage(MessageHandler handler)
  {
    m_messageHandler = handler;
  }

  std::vector<std::string> GossipManager::getPeers() const
  {
    return m_seedNodes;
  }

  // Private methods
  void GossipManager::acceptLoop()
  {
    while (m_running)
    {
      sockaddr_in clientAddr{};
      socklen_t clientLen = sizeof(clientAddr);
      int clientSock = accept(m_serverSocket, (sockaddr *)&clientAddr, &clientLen);

      if (clientSock < 0)
        continue;

      char clientIp[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, INET_ADDRSTRLEN);
      std::string peerAddr = std::string(clientIp) + ":" + std::to_string(ntohs(clientAddr.sin_port));

      std::lock_guard<std::mutex> lock(m_peersMutex);
      m_peerSockets.push_back(clientSock);

      m_threads.emplace_back(&GossipManager::handleConnection, this, clientSock, peerAddr);
    }
  }

  void GossipManager::connectToSeeds()
  {
    for (const auto &seed : m_seedNodes)
    {
      size_t colon = seed.find(':');
      std::string host = seed.substr(0, colon);
      uint16_t port = std::stoi(seed.substr(colon + 1));

      while (m_running)
      {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (connect(sock, (sockaddr *)&addr, sizeof(addr)) == 0)
        {
          std::lock_guard<std::mutex> lock(m_peersMutex);
          m_peerSockets.push_back(sock);
          m_firstSeedAddress = seed;
          m_threads.emplace_back(&GossipManager::handleConnection, this, sock, seed);
          std::cout << "GossipManager: Connected to seed " << seed << std::endl;
          break;
        }
        else
        {
          close(sock);
          std::cerr << "GossipManager: Failed to connect to seed " << seed << ", retrying in 5s..." << std::endl;
          std::this_thread::sleep_for(std::chrono::seconds(5));
        }
      }
    }
  }

  void GossipManager::handleConnection(int socket, const std::string &peerAddress)
  {
    readLoop(socket, peerAddress);
    close(socket);
  }

  void GossipManager::readLoop(int socket, const std::string &peerAddress)
  {
    while (m_running)
    {
      uint32_t len;
      int bytesRead = recv(socket, &len, sizeof(len), 0);
      if (bytesRead <= 0)
        break;

      len = ntohl(len);
      std::vector<uint8_t> message(len);
      bytesRead = recv(socket, message.data(), len, MSG_WAITALL);
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
}