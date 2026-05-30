// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <chrono>
#include "NetNodeConfig.h"

namespace cn {

class P2pNodeConfig : public NetNodeConfig {
public:
  P2pNodeConfig();

  // getters
  std::chrono::nanoseconds getTimedSyncInterval() const;
  std::chrono::nanoseconds getHandshakeTimeout() const;
  std::chrono::nanoseconds getConnectInterval() const;
  std::chrono::nanoseconds getConnectTimeout() const;
  size_t getExpectedOutgoingConnectionsCount() const;
  size_t getWhiteListConnectionsPercent() const;
  boost::uuids::uuid getNetworkId() const;
  size_t getPeerListConnectRange() const;
  size_t getPeerListGetTryCount() const;

  // setters
  void setTimedSyncInterval(std::chrono::nanoseconds interval);
  void setHandshakeTimeout(std::chrono::nanoseconds timeout);
  void setConnectInterval(std::chrono::nanoseconds interval);
  void setConnectTimeout(std::chrono::nanoseconds timeout);
  void setExpectedOutgoingConnectionsCount(size_t count);
  void setWhiteListConnectionsPercent(size_t percent);
  void setNetworkId(const boost::uuids::uuid& id);
  void setPeerListConnectRange(size_t range);
  void setPeerListGetTryCount(size_t count);

  size_t getMinOutgoingConnections() const;
  size_t getMaxOutgoingConnections() const;
  void setMinOutgoingConnections(size_t count);
  void setMaxOutgoingConnections(size_t count);

private:
  std::chrono::nanoseconds timedSyncInterval;
  std::chrono::nanoseconds handshakeTimeout;
  std::chrono::nanoseconds connectInterval;
  std::chrono::nanoseconds connectTimeout;
  boost::uuids::uuid networkId;
  size_t expectedOutgoingConnectionsCount;
  size_t whiteListConnectionsPercent;
  size_t peerListConnectRange;
  size_t peerListGetTryCount;

  size_t m_minOutgoingConnections = cn::P2P_DEFAULT_MIN_CONNECTIONS;
  size_t m_maxOutgoingConnections = cn::P2P_DEFAULT_MAX_CONNECTIONS;
};

}
