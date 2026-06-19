// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include "CryptoNote.h"
#include "P2pProtocolTypes.h"
#include <list>
#include <boost/uuid/uuid.hpp>

namespace cn
{

  struct CryptoNoteConnectionContext;

  struct IP2pEndpoint
  {
    virtual ~IP2pEndpoint() = default;
    virtual void relayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) = 0;
    virtual bool invokeNotifyToPeer(int command, const BinaryArray &req_buff, const cn::CryptoNoteConnectionContext &context) = 0;
    virtual uint64_t getConnectionsCount() = 0;
    virtual void forEachConnection(const std::function<void(cn::CryptoNoteConnectionContext &, PeerIdType)> &f) = 0;
    virtual void dropConnection(CryptoNoteConnectionContext &context, bool add_fail) = 0;

    // can be called from external threads
    virtual void externalRelayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) = 0;
    virtual void externalRelayNotifyToList(int command, const BinaryArray &data_buff, const std::list<boost::uuids::uuid> &relayList) = 0;
  };

  struct p2p_endpoint_stub : public IP2pEndpoint
  {
    void relayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) override {}
    bool invokeNotifyToPeer(int command, const BinaryArray &req_buff, const cn::CryptoNoteConnectionContext &context) override { return true; }
    void dropConnection(CryptoNoteConnectionContext &context, bool add_fail) override {}
    void forEachConnection(const std::function<void(cn::CryptoNoteConnectionContext &, PeerIdType)> &f) override {}
    uint64_t getConnectionsCount() override { return 0; }
    void externalRelayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) override {}
    void externalRelayNotifyToList(int command, const BinaryArray &data_buff, const std::list<boost::uuids::uuid> &relayList) override {}
  };
}
