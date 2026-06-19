// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 The TurtleCoin developers
// Copyright (c) 2016-2020 The Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#pragma once

#include <functional>
#include <unordered_map>

#include <boost/functional/hash.hpp>
#include <list>
#include <boost/uuid/uuid.hpp>

#include <System/Context.h>
#include <System/ContextGroup.h>
#include <System/Dispatcher.h>
#include <System/Event.h>
#include <System/Timer.h>
#include <System/TcpConnection.h>
#include <System/TcpListener.h>

#include "CryptoNoteCore/OnceInInterval.h"
#include "CryptoNoteProtocol/CryptoNoteProtocolHandler.h"
#include "Logging/LoggerRef.h"

#include "ConnectionContext.h"
#include "LevinProtocol.h"
#include "NetNodeCommon.h"
#include "NetNodeConfig.h"
#include "P2pProtocolDefinitions.h"
#include "P2pNetworks.h"
#include "PeerListManager.h"

namespace platform_system {
class TcpConnection;
}

namespace cn
{
  class LevinProtocol;
  class ISerializer;

  struct P2pMessage {
    enum Type {
      COMMAND,
      REPLY,
      NOTIFY
    };

    P2pMessage(Type type, uint32_t command, const BinaryArray& buffer, int32_t returnCode = 0) :
      type(type), command(command), buffer(buffer), returnCode(returnCode) {
    }

    size_t size() const {
      return buffer.size();
    }

    Type type;
    uint32_t command;
    const BinaryArray buffer;
    int32_t returnCode;
  };

  struct P2pConnectionContext : public CryptoNoteConnectionContext {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    platform_system::Context<void>* context = nullptr;
    PeerIdType peerId = 0;
    platform_system::TcpConnection connection;

    P2pConnectionContext(platform_system::Dispatcher& dispatcher, logging::ILogger& log, platform_system::TcpConnection&& conn) :
      connection(std::move(conn)),
      logger(log, "node_server"),
      queueEvent(dispatcher) {
    }

    bool pushMessage(P2pMessage&& msg);
    std::vector<P2pMessage> popBuffer();
    void interrupt();

    uint64_t writeDuration(TimePoint now) const;

  private:
    logging::LoggerRef logger;
    TimePoint writeOperationStartTime;
    platform_system::Event queueEvent;
    std::vector<P2pMessage> writeQueue;
    size_t writeQueueSize = 0;
    bool stopped = false;
  };

  class NodeServer :  public IP2pEndpoint
  {
  public:

    NodeServer(platform_system::Dispatcher& dispatcher, cn::CryptoNoteProtocolHandler& payload_handler, logging::ILogger& log);

    bool run();
    bool init(const NetNodeConfig& config);
    bool deinit();
    bool sendStopSignal();
    cn::CryptoNoteProtocolHandler& getPayloadObject();

    void serialize(ISerializer& s);

    // debug functions
    bool logPeerlist() const;
    uint64_t getConnectionsCount() override;
    size_t getOutgoingConnectionsCount() const;
    /** Target outgoing sync lanes (P2P auto-scale adjusts this). */
    size_t getTargetOutgoingConnectionsCount() const { return m_config.m_netConfig.connections_count; }

    cn::PeerlistManager& getPeerlistManager() { return m_peerlist; }

  private:
    enum PeerType
    {
      anchor = 0,
      white,
      gray
    };
    int handleCommand(const LevinProtocol::Command& cmd, BinaryArray& buff_out, P2pConnectionContext& context, bool& handled);

    //----------------- commands handlers ----------------------------------------------
    int handleHandshake(int command, const COMMAND_HANDSHAKE::request& arg, COMMAND_HANDSHAKE::response& rsp, P2pConnectionContext& context);
    int handleTimedSync(int command, const COMMAND_TIMED_SYNC::request& arg, COMMAND_TIMED_SYNC::response& rsp, P2pConnectionContext& context);
    int handlePing(int command, const COMMAND_PING::request& arg, COMMAND_PING::response& rsp, const P2pConnectionContext& context) const;

    bool initConfig();
    bool makeDefaultConfig();
    bool storeConfig();

    bool handshake(cn::LevinProtocol& proto, P2pConnectionContext& context, bool just_take_peerlist = false);
    bool timedSync();
    bool handleTimedSyncResponse(const BinaryArray& in, P2pConnectionContext& context);
    void forEachConnection(const std::function<void(P2pConnectionContext &)> &action);

    void onConnectionNew(P2pConnectionContext& context);
    void onConnectionClose(P2pConnectionContext& context);

    //----------------- i_p2p_endpoint -------------------------------------------------------------
    void relayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) override;
    bool invokeNotifyToPeer(int command, const BinaryArray &req_buff, const CryptoNoteConnectionContext &context) override;
    void dropConnection(CryptoNoteConnectionContext &context, bool add_fail) override;
    void forEachConnection(const std::function<void(cn::CryptoNoteConnectionContext &, PeerIdType)> &f) override;
    void externalRelayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection) override;
    void externalRelayNotifyToList(int command, const BinaryArray &data_buff, const std::list<boost::uuids::uuid> &relayList) override;
    //-----------------------------------------------------------------------------------------------
    bool handleConfig(const NetNodeConfig& config);
    bool append_net_address(std::vector<NetworkAddress>& nodes, const std::string& addr);
    bool idleWorker();
    bool handleRemotePeerlist(const std::list<PeerlistEntry>& peerlist, time_t local_time, const CryptoNoteConnectionContext& context);
    bool getLocalNodeData(basic_node_data& node_data) const;
    bool fixTimeDelta(std::list<PeerlistEntry>& local_peerlist, time_t local_time, int64_t& delta) const;

    bool connectionsMaker();
    bool makeNewConnectionFromPeerlist(bool use_white_list);
    bool makeNewConnectionFromAnchorPeerlist(const std::vector<AnchorPeerlistEntry> &anchor_peerlist);
    bool tryToConnectAndHandshakeWithNewPeer(const NetworkAddress &na, bool just_take_peerlist = false, uint64_t last_seen_stamp = 0, PeerType peer_type = white, uint64_t first_seen_stamp = 0);
    bool isPeerUsed(const PeerlistEntry &peer) const;
    bool isPeerUsed(const AnchorPeerlistEntry &peer) const;
    bool isAddrConnected(const NetworkAddress& peer) const;
    bool tryPing(const basic_node_data& node_data, const P2pConnectionContext& context);
    bool makeExpectedConnectionsCount(PeerType peer_type, size_t expected_connections);
    bool isPriorityNode(const NetworkAddress& na);

    bool connectToPeerlist(const std::vector<NetworkAddress>& peers);

    //debug functions
    std::string printConnectionsContainer() const;

    using ConnectionContainer = std::unordered_map<boost::uuids::uuid, P2pConnectionContext, boost::hash<boost::uuids::uuid>>;
    using ConnectionIterator = ConnectionContainer::iterator;
    ConnectionContainer m_connections;

    void acceptLoop();
    void connectionHandler(const boost::uuids::uuid& connectionId, P2pConnectionContext& connection);
    void writeHandler(P2pConnectionContext& ctx) const;
    void onIdle();
    void timedSyncLoop();
    void timeoutLoop();

    void autoScaleLoop();
    void autoScaleConnections();

    template<typename T>
    void safeInterrupt(T& obj) const;

    struct config
    {
      network_config m_netConfig;
      uint64_t m_peerId;

      void serialize(ISerializer& s) {
        KV_MEMBER(m_netConfig)
        KV_MEMBER(m_peerId)
      }
    };

    config m_config;
    std::string m_configFolder;

    uint32_t m_listeningPort;
    uint32_t m_externalPort;
    bool m_allowLocalIp = false;
    bool m_hideMyPort = false;
    std::string m_p2pStateFilename;

    size_t m_minOutgoingConnections = P2P_DEFAULT_MIN_CONNECTIONS;
    size_t m_maxOutgoingConnections = P2P_DEFAULT_MAX_CONNECTIONS;
    platform_system::Timer m_autoScaleTimer;

    platform_system::Dispatcher& m_dispatcher;
    platform_system::ContextGroup m_workingContextGroup;
    platform_system::Event m_stopEvent;
    platform_system::Timer m_idleTimer;
    platform_system::Timer m_timeoutTimer;
    platform_system::TcpListener m_listener;
    logging::LoggerRef logger;
    std::atomic<bool> m_stop{false};

    CryptoNoteProtocolHandler& m_payload_handler;
    PeerlistManager m_peerlist;

    OnceInInterval m_connectionsMakerInterval = OnceInInterval(1);
    OnceInInterval m_peerlistStoreInterval = OnceInInterval(60 * 30, false);
    platform_system::Timer m_timedSyncTimer;

    std::string m_bindIp;
    std::string m_port;
    std::vector<NetworkAddress> m_priority_peers;
    std::vector<NetworkAddress> m_exclusive_peers;
    std::vector<NetworkAddress> m_seed_nodes;
    std::list<PeerlistEntry> m_command_line_peers;
    boost::uuids::uuid m_network_id = CRYPTONOTE_NETWORK;
  };
}
