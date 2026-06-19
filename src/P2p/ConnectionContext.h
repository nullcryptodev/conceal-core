// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 The TurtleCoin developers
// Copyright (c) 2016-2020 The Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <list>
#include <ostream>
#include <unordered_set>
#include <boost/optional.hpp>
#include <boost/uuid/uuid.hpp>
#include "Common/StringTools.h"
#include "P2p/PendingLiteBlock.h"
#include "crypto/hash.h"

namespace cn {

struct CryptoNoteConnectionContext {
  uint8_t version;
  boost::uuids::uuid m_connectionId;
  uint32_t m_remoteIp = 0;
  uint32_t m_remotePort = 0;
  bool m_isIncoming = false;
  time_t m_started = 0;

  enum state {
    stateBeforeHandshake = 0, //default state
    stateSynchronizing,
    stateIdle,
    stateNormal,
    stateSyncRequired,
    statePoolSyncRequired,
    stateShutdown
  };

  state m_state = stateBeforeHandshake;
  boost::optional<PendingLiteBlock> m_pendingLiteBlock;
  std::list<crypto::Hash> m_neededObjects;
  std::unordered_set<crypto::Hash> m_requestedObjects;
  uint32_t m_remoteBlockchainHeight = 0;
  uint32_t m_lastResponseHeight = 0;
};

inline std::string get_protocol_state_string(CryptoNoteConnectionContext::state s) {
  switch (s)  {
  case CryptoNoteConnectionContext::stateBeforeHandshake:
    return "stateBeforeHandshake";
  case CryptoNoteConnectionContext::stateSynchronizing:
    return "stateSynchronizing";
  case CryptoNoteConnectionContext::stateIdle:
    return "stateIdle";
  case CryptoNoteConnectionContext::stateNormal:
    return "stateNormal";
  case CryptoNoteConnectionContext::stateSyncRequired:
    return "stateSyncRequired";
  case CryptoNoteConnectionContext::statePoolSyncRequired:
    return "statePoolSyncRequired";
  case CryptoNoteConnectionContext::stateShutdown:
    return "stateShutdown";
  default:
    return "unknown";
  }
}

}

namespace std {
inline std::ostream& operator << (std::ostream& s, const cn::CryptoNoteConnectionContext& context) {
  return s << "[" << common::ipAddressToString(context.m_remoteIp) << ":" << 
    context.m_remotePort << (context.m_isIncoming ? " INC" : " OUT") << "] ";
}
}
