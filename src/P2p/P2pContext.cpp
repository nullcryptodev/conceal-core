// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "P2pContext.h"

#include <System/EventLock.h>
#include <System/InterruptedException.h>
#include <System/Ipv4Address.h>
#include <System/OperationTimeout.h>

#include "LevinProtocol.h"

using namespace platform_system;

namespace cn {

P2pContext::Message::Message(P2pMessage&& msg, Type messageType, uint32_t returnCode) :
  messageType(messageType), returnCode(returnCode) {
  type = msg.type;
  data = std::move(msg.data);
}

size_t P2pContext::Message::size() const {
  return data.size();
}

P2pContext::P2pContext(
  Dispatcher& dispatcher,
  TcpConnection&& conn,
  bool isIncoming,
  const NetworkAddress& remoteAddress,
  std::chrono::nanoseconds timedSyncInterval,
  const CORE_SYNC_DATA& timedSyncData)
  :
  m_incoming(isIncoming),
  m_remoteAddress(remoteAddress),
  m_dispatcher(dispatcher),
  m_contextGroup(dispatcher),
  m_timeStarted(Clock::now()),
  m_timedSyncInterval(timedSyncInterval),
  m_timedSyncData(timedSyncData),
  m_timedSyncTimer(dispatcher),
  m_timedSyncFinished(dispatcher),
  m_connection(std::move(conn)),
  m_writeEvent(dispatcher),
  m_readEvent(dispatcher) {
  m_writeEvent.set();
  m_readEvent.set();
  m_lastReadTime = m_timeStarted; // use current time
  m_contextGroup.spawn(std::bind(&P2pContext::timedSyncLoop, this));
}

P2pContext::~P2pContext() {
  stop();
  // wait for timedSyncLoop finish
  m_timedSyncFinished.wait();
  // ensure that all read/write operations completed
  m_readEvent.wait();
  m_writeEvent.wait();
}

PeerIdType P2pContext::getPeerId() const {
  return m_peerId;
}

uint16_t P2pContext::getPeerPort() const {
  return m_peerPort;
}

const NetworkAddress& P2pContext::getRemoteAddress() const {
  return m_remoteAddress;
}

bool P2pContext::isIncoming() const {
  return m_incoming;
}

void P2pContext::setPeerInfo(uint8_t protocolVersion, PeerIdType id, uint16_t port) {
  m_version = protocolVersion;
  m_peerId = id;
  if (isIncoming()) {
    m_peerPort = port;
  }
}

bool P2pContext::readCommand(LevinProtocol::Command& cmd) {
  if (m_stopped) {
    throw InterruptedException();
  }

  EventLock lk(m_readEvent);
  bool result = LevinProtocol(m_connection).readCommand(cmd);
  m_lastReadTime = Clock::now();
  return result;
}

void P2pContext::writeMessage(const Message& msg) {
  if (m_stopped) {
    throw InterruptedException();
  }

  EventLock lk(m_writeEvent);
  LevinProtocol proto(m_connection);

  switch (msg.messageType) {
  case P2pContext::Message::NOTIFY:
    proto.sendMessage(msg.type, msg.data, false);
    break;
  case P2pContext::Message::REQUEST:
    proto.sendMessage(msg.type, msg.data, true);
    break;
  case P2pContext::Message::REPLY:
    proto.sendReply(msg.type, msg.data, msg.returnCode);
    break;
  }
}

void P2pContext::start() {
  // stub for OperationTimeout class
} 

void P2pContext::stop() {
  if (!m_stopped) {
    m_stopped = true;
    m_contextGroup.interrupt();
  }
}

void P2pContext::timedSyncLoop() {
  // construct message
  P2pContext::Message timedSyncMessage{ 
    P2pMessage{ 
      COMMAND_TIMED_SYNC::ID, 
      LevinProtocol::encode(COMMAND_TIMED_SYNC::request{ m_timedSyncData })
    }, 
    P2pContext::Message::REQUEST 
  };

  while (!m_stopped) {
    try {
      m_timedSyncTimer.sleep(m_timedSyncInterval);

      OperationTimeout<P2pContext> timeout(m_dispatcher, *this, m_timedSyncInterval);
      writeMessage(timedSyncMessage);

      // check if we had read operation in given time interval
      if ((m_lastReadTime + m_timedSyncInterval * 2) < Clock::now()) {
        stop();
        break;
      }
    } catch (InterruptedException&) {
      // someone stopped us
    } catch (std::exception&) {
      stop(); // stop connection on write error
      break;
    }
  }

  m_timedSyncFinished.set();
}

P2pContext::Message makeReply(uint32_t command, const BinaryArray& data, uint32_t returnCode) {
  return P2pContext::Message(
    P2pMessage{ command, data },
    P2pContext::Message::REPLY,
    returnCode);
}

P2pContext::Message makeRequest(uint32_t command, const BinaryArray& data) {
  return P2pContext::Message(
    P2pMessage{ command, data },
    P2pContext::Message::REQUEST);
}

std::ostream& operator <<(std::ostream& s, const P2pContext& conn) {
  return s << "[" << conn.getRemoteAddress() << "]";
}

}
