// GossipManager.h — TCP mesh network between validators
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "SidechainTypes.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace Sidechain
{
  class GossipManager
  {
  public:
    using MessageHandler = std::function<void(const std::vector<uint8_t> &, const std::string &fromIp)>;

    GossipManager(uint16_t gossipPort, const std::vector<std::string> &seedNodes);
    ~GossipManager();

    void start();
    void stop();

    // Send a message to all connected peers
    void broadcast(const std::vector<uint8_t> &message);

    // Send to a specific peer
    void sendTo(const std::string &host, uint16_t port, const std::vector<uint8_t> &message);

    // Register handler for incoming messages
    void onMessage(MessageHandler handler);

    // Get list of connected peers
    std::vector<std::string> getPeers() const;

    // Get the address of the first connected seed (for sync requests)
    std::string getFirstSeedAddress() const;

    size_t getPeerCount() const
    {
      std::lock_guard<std::mutex> lock(m_peersMutex);
      return m_peerSockets.size();
    }

  private:
    void acceptLoop();
    void connectToSeeds();
    void handleConnection(int socket, const std::string &peerAddress);
    void readLoop(int socket, const std::string &peerAddress);

    uint16_t m_port;
    std::vector<std::string> m_seedNodes;
    std::atomic<bool> m_running{false};

    int m_serverSocket;
    std::vector<std::thread> m_threads;
    std::vector<int> m_peerSockets;
    mutable std::mutex m_peersMutex;

    MessageHandler m_messageHandler;
    std::string m_firstSeedAddress;
  };
}