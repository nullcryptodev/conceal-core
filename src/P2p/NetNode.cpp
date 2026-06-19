// Copyright (c) 2011-2017 The Cryptonote developers
// Copyright (c) 2017-2018 The Circle Foundation & Conceal Devs
// Copyright (c) 2018-2019 The TurtleCoin developers
// Copyright (c) 2016-2020 The Karbo developers
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "NetNode.h"

#include "../pow/pow_service.hpp"

#include <algorithm>
#include <fstream>

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <miniupnpc/include/miniupnpc.h>
#include <miniupnpc/include/upnpcommands.h>

#include <System/Context.h>
#include <System/ContextGroupTimeout.h>
#include <System/EventLock.h>
#include <System/InterruptedException.h>
#include <System/Ipv4Address.h>
#include <System/Ipv4Resolver.h>
#include <System/TcpListener.h>
#include <System/TcpConnector.h>

#include "version.h"
#include "Common/StdInputStream.h"
#include "Common/StdOutputStream.h"
#include "Common/Util.h"
#include "crypto/crypto.h"

#include <CryptoNoteConfig.h>
#include "ConnectionContext.h"
#include "LevinProtocol.h"
#include "P2pProtocolDefinitions.h"
#include "P2pUtils.h"

#include "Serialization/BinaryInputStreamSerializer.h"
#include "Serialization/BinaryOutputStreamSerializer.h"
#include "Serialization/SerializationOverloads.h"
#include "Common/StringTools.h"

using namespace common;
using namespace logging;
using namespace cn;

namespace {

boost::uuids::random_generator uuidGen;

size_t get_random_index_with_fixed_probability(size_t max_index) {
  //divide by zero workaround
  if (!max_index) {
    return 0;
  }

  size_t x = crypto::rand<size_t>() % (max_index + 1);
  return (x * x * x ) / (max_index * max_index); //parabola \/
}

void addPortMapping(const logging::LoggerRef& logger, uint32_t port) {
  // Add UPnP port mapping
  logger(INFO) <<  "Attempting to add IGD port mapping.";
  int result;
  UPNPDev *deviceList = upnpDiscover(1000, nullptr, nullptr, 0, 0, 2, &result);
  UPNPUrls urls;
  IGDdatas igdData;
  char lanAddress[64];
  result = UPNP_GetValidIGD(deviceList, &urls, &igdData, lanAddress, sizeof lanAddress);
  freeUPNPDevlist(deviceList);
  if (result != 0) {
    if (result == 1) {
      std::ostringstream portString;
      portString << port;
      if (UPNP_AddPortMapping(urls.controlURL, igdData.first.servicetype, portString.str().c_str(),
        portString.str().c_str(), lanAddress, "conceal", "TCP", nullptr, "0") != 0) {
        logger(ERROR) << "UPNP port mapping failed.";
      } else {
        logger(INFO, BRIGHT_GREEN) << "Added IGD port mapping.";
      }
    } else if (result == 2) {
      logger(INFO) <<  "IGD was found but reported as not connected.";
    } else if (result == 3) {
      logger(INFO) <<  "UPnP device was found but not recognized as IGD.";
    } else {
      logger(ERROR) << "UPNP_GetValidIGD returned an unknown result code.";
    }

    FreeUPNPUrls(&urls);
  } else {
    logger(INFO) <<  "No IGD was found.";
  }
}

}


namespace cn
{
  namespace
  {
    std::string print_peerlist_to_string(const std::list<PeerlistEntry> &pl)
    {
      time_t now_time = 0;
      time(&now_time);
      std::stringstream ss;
      ss << std::left
         << std::setw(20) << "Peer id"
         << std::setw(25) << "Remote host"
         << std::setw(16) << "Last seen" << std::endl;
      for (const auto &pe : pl)
      {
        ss << std::setw(20) << std::hex << pe.id
           << std::setw(25) << common::ipAddressToString(pe.adr.ip) + ":" + std::to_string(pe.adr.port)
           << std::setw(16) << common::timeIntervalToString(now_time - pe.last_seen) << std::endl;
      }
      return ss.str();
    }
  }

  //-----------------------------------------------------------------------------------
  // P2pConnectionContext implementation
  //-----------------------------------------------------------------------------------

  bool P2pConnectionContext::pushMessage(P2pMessage&& msg) {
    writeQueueSize += msg.size();

    if (writeQueueSize > P2P_CONNECTION_MAX_WRITE_BUFFER_SIZE) {
      logger(DEBUGGING) << *this << "P2pConnectionContext::pushMessage() write queue overflows. Interrupt connection";
      interrupt();
      return false;
    }

    writeQueue.push_back(std::move(msg));
    queueEvent.set();
    return true;
  }

  std::vector<P2pMessage> P2pConnectionContext::popBuffer() {
    writeOperationStartTime = TimePoint();

    while (writeQueue.empty() && !stopped) {
      queueEvent.wait();
    }

    std::vector<P2pMessage> msgs(std::move(writeQueue));
    writeQueue.clear();
    writeQueueSize = 0;
    writeOperationStartTime = Clock::now();
    queueEvent.clear();
    return msgs;
  }

  uint64_t P2pConnectionContext::writeDuration(TimePoint now) const { // in milliseconds
    return writeOperationStartTime == TimePoint() ? 0 : std::chrono::duration_cast<std::chrono::milliseconds>(now - writeOperationStartTime).count();
  }

  void P2pConnectionContext::interrupt() {
    logger(DEBUGGING) << *this << "P2pConnectionContext::interrupt()";
    assert(context != nullptr);
    stopped = true;
    queueEvent.set();
    context->interrupt();
  }

  template <typename Command, typename Handler>
  int invokeAdaptor(const BinaryArray& reqBuf, BinaryArray& resBuf, P2pConnectionContext& ctx, Handler handler) {
    using Request = typename Command::request;
    using Response = typename Command::response;
    int command = Command::ID;

    Request req{};

    if (!LevinProtocol::decode(reqBuf, req)) {
      throw std::runtime_error("Failed to load_from_binary in command " + std::to_string(command));
    }

    Response res{};
    int ret = handler(command, req, res, ctx);
    resBuf = LevinProtocol::encode(res);
    return ret;
  }

  NodeServer::NodeServer(platform_system::Dispatcher& dispatcher, cn::CryptoNoteProtocolHandler& payload_handler, logging::ILogger& log) :
    m_dispatcher(dispatcher),
    m_workingContextGroup(dispatcher),
    m_stopEvent(m_dispatcher),
    m_idleTimer(m_dispatcher),
    m_timeoutTimer(m_dispatcher),
    logger(log, "node_server"),
    m_payload_handler(payload_handler),
    m_timedSyncTimer(m_dispatcher)    
  {
  }

  void NodeServer::serialize(ISerializer& s) {
    uint8_t version = 1;
    s(version, "version");

    if (version != 1) {
      return;
    }

    s(m_peerlist, "peerlist");
    s(m_config.m_peerId, "peer_id");
  }

  int NodeServer::handleCommand(const LevinProtocol::Command& cmd, BinaryArray& out, P2pConnectionContext& ctx, bool& handled)
  {
    int ret = 0;
    handled = true;

    if (cmd.isResponse && cmd.command == COMMAND_TIMED_SYNC::ID) {
      if (!handleTimedSyncResponse(cmd.buf, ctx)) {
        ctx.m_state = CryptoNoteConnectionContext::stateShutdown;
      }
      return 0;
    }

    switch (cmd.command) {
      case COMMAND_HANDSHAKE::ID: {
        ret = invokeAdaptor<COMMAND_HANDSHAKE>(cmd.buf, out, ctx,
          [this](int command, const COMMAND_HANDSHAKE::request& req, COMMAND_HANDSHAKE::response& res, P2pConnectionContext& c) {
            return handleHandshake(command, req, res, c);
          });
        break;
      }
      case COMMAND_TIMED_SYNC::ID: {
        ret = invokeAdaptor<COMMAND_TIMED_SYNC>(cmd.buf, out, ctx,
          [this](int command, const COMMAND_TIMED_SYNC::request& req, COMMAND_TIMED_SYNC::response& res, P2pConnectionContext& c) {
            return handleTimedSync(command, req, res, c);
          });
        break;
      }
      case COMMAND_PING::ID: {
        ret = invokeAdaptor<COMMAND_PING>(cmd.buf, out, ctx,
          [this](int command, const COMMAND_PING::request& req, COMMAND_PING::response& res, const P2pConnectionContext& c) {
            return handlePing(command, req, res, c);
          });
        break;
      }
      default: {
        handled = false;
        ret = m_payload_handler.handleCommand(cmd.isNotify, cmd.command, cmd.buf, out, ctx, handled);
      }
    }

    return ret;
  }

  //-----------------------------------------------------------------------------------

  bool NodeServer::initConfig() {
    try {
      std::string state_file_path = m_configFolder + "/" + m_p2pStateFilename;
      bool loaded = false;

      try {
        std::ifstream p2p_data;
        p2p_data.open(state_file_path, std::ios_base::binary | std::ios_base::in);

        if (!p2p_data.fail()) {
          StdInputStream inputStream(p2p_data);
          BinaryInputStreamSerializer a(inputStream);
          cn::serialize(*this, a);
          loaded = true;
        }
      } catch (const std::exception&) {
      }

      if (!loaded) {
        makeDefaultConfig();
      }

      //at this moment we have hardcoded config
      m_config.m_netConfig.handshake_interval = cn::P2P_DEFAULT_HANDSHAKE_INTERVAL;
      m_config.m_netConfig.connections_count = cn::P2P_DEFAULT_CONNECTIONS_COUNT;
      m_config.m_netConfig.packet_max_size = cn::P2P_DEFAULT_PACKET_MAX_SIZE; //20 MB limit
      m_config.m_netConfig.config_id = 0; // initial config
      m_config.m_netConfig.connection_timeout = cn::P2P_DEFAULT_CONNECTION_TIMEOUT;
      m_config.m_netConfig.ping_connection_timeout = cn::P2P_DEFAULT_PING_CONNECTION_TIMEOUT;
      m_config.m_netConfig.send_peerlist_sz = cn::P2P_DEFAULT_PEERS_IN_HANDSHAKE;

    } catch (const std::exception& e) {
      logger(ERROR) << "initConfig failed: " << e.what();
      return false;
    }
    return true;
  }

  //-----------------------------------------------------------------------------------
  void NodeServer::forEachConnection(const std::function<void(CryptoNoteConnectionContext &, PeerIdType)> &f)
  {
    for (auto& ctx : m_connections) {
      f(ctx.second, ctx.second.peerId);
    }
  }

  //-----------------------------------------------------------------------------------
  void NodeServer::externalRelayNotifyToAll(int command, const BinaryArray &data_buff, const net_connection_id *excludeConnection)
  {
    m_dispatcher.remoteSpawn([this, command, data_buff, excludeConnection] {
      relayNotifyToAll(command, data_buff, excludeConnection);
    });
  }

  //-----------------------------------------------------------------------------------
  void NodeServer::externalRelayNotifyToList(int command, const BinaryArray &data_buff, const std::list<boost::uuids::uuid> &relayList)
  {
    m_dispatcher.remoteSpawn([this, command, data_buff, relayList] {
      forEachConnection([&relayList, &command, &data_buff](P2pConnectionContext &conn) {
        if (std::find(relayList.begin(), relayList.end(), conn.m_connectionId) != relayList.end())
        {
          if (conn.peerId && (conn.m_state == CryptoNoteConnectionContext::stateNormal || conn.m_state == CryptoNoteConnectionContext::stateSynchronizing))
          {
            conn.pushMessage(P2pMessage(P2pMessage::NOTIFY, command, data_buff));
          }
        }
      });
    });
  }


  void NodeServer::dropConnection(CryptoNoteConnectionContext &context, bool add_fail)
  {
    context.m_state = CryptoNoteConnectionContext::stateShutdown;
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::makeDefaultConfig()
  {
    m_config.m_peerId  = crypto::rand<uint64_t>();
    return true;
  }

  bool NodeServer::isPeerUsed(const AnchorPeerlistEntry &peer) const
  {
    if (m_config.m_peerId == peer.id)
      return true; //dont make connections to ourself

    for (const auto &kv : m_connections)
    {
      const auto &cntxt = kv.second;
      if (cntxt.peerId == peer.id || (!cntxt.m_isIncoming && peer.adr.ip == cntxt.m_remoteIp && peer.adr.port == cntxt.m_remotePort))
      {
        return true;
      }
    }
    return false;
  }

  bool NodeServer::handleConfig(const NetNodeConfig& config) {
    m_bindIp = config.getBindIp();
    m_port = std::to_string(config.getBindPort());
    m_externalPort = config.getExternalPort();
    m_allowLocalIp = config.getAllowLocalIp();

    auto peers = config.getPeers();
    std::copy(peers.begin(), peers.end(), std::back_inserter(m_command_line_peers));

    auto exclusiveNodes = config.getExclusiveNodes();
    std::copy(exclusiveNodes.begin(), exclusiveNodes.end(), std::back_inserter(m_exclusive_peers));

    auto priorityNodes = config.getPriorityNodes();
    std::copy(priorityNodes.begin(), priorityNodes.end(), std::back_inserter(m_priority_peers));

    auto seedNodes = config.getSeedNodes();
    std::copy(seedNodes.begin(), seedNodes.end(), std::back_inserter(m_seed_nodes));

    m_hideMyPort = config.getHideMyPort();
    return true;
  }

  bool NodeServer::append_net_address(std::vector<NetworkAddress>& nodes, const std::string& addr) {
    size_t pos = addr.find_last_of(':');
    if (!(std::string::npos != pos && addr.length() - 1 != pos && 0 != pos)) {
      logger(ERROR, BRIGHT_RED) << "Failed to parse seed address from string: '" << addr << '\'';
      return false;
    }

    std::string host = addr.substr(0, pos);

    try {
      uint32_t port = common::fromString<uint32_t>(addr.substr(pos + 1));

      platform_system::Ipv4Resolver resolver(m_dispatcher);
      auto address = resolver.resolve(host);
      nodes.push_back(NetworkAddress{hostToNetwork(address.getValue()), port});

      logger(TRACE) << "Added seed node: " << nodes.back() << " (" << host << ")";

    } catch (const std::exception& e) {
      logger(ERROR, BRIGHT_YELLOW) << "Failed to resolve host name '" << host << "': " << e.what();
      return false;
    }

    return true;
  }


  //-----------------------------------------------------------------------------------

  bool NodeServer::init(const NetNodeConfig& config) {
    if (!config.getTestnet())
    {
      for (auto seed : cn::SEED_NODES)
      {
        append_net_address(m_seed_nodes, seed);
      }
    }
    else
    {
      for (auto seed : cn::TESTNET_SEED_NODES)
      {
        append_net_address(m_seed_nodes, seed);
      }
      m_network_id.data[0] += 1;
    }

    if (!handleConfig(config)) {
      logger(ERROR, BRIGHT_RED) << "Failed to handle command line";
      return false;
    }

    m_configFolder = config.getConfigFolder();
    m_p2pStateFilename = config.getP2pStateFilename();

    if (!initConfig()) {
      logger(ERROR, BRIGHT_RED) << "Failed to init config.";
      return false;
    }

    if (!m_peerlist.init(m_allowLocalIp)) {
      logger(ERROR, BRIGHT_RED) << "Failed to init peerlist.";
      return false;
    }

    for (const auto &p : m_command_line_peers)
    {
      m_peerlist.append_with_peer_white(p);
    }

    //try to bind
    logger(INFO) <<  "Binding on " << m_bindIp << ":" << m_port;
    m_listeningPort = common::fromString<uint16_t>(m_port);

    m_listener = platform_system::TcpListener(m_dispatcher, platform_system::Ipv4Address(m_bindIp), static_cast<uint16_t>(m_listeningPort));

    logger(INFO, BRIGHT_GREEN) << "Net service bound on " << m_bindIp << ":" << m_listeningPort;

    if(m_externalPort) {
      logger(INFO) <<  "External port defined as " << m_externalPort;
    }

    addPortMapping(logger, m_listeningPort);

    return true;
  }
  //-----------------------------------------------------------------------------------

  cn::CryptoNoteProtocolHandler& NodeServer::getPayloadObject()
  {
    return m_payload_handler;
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::run() {
    logger(INFO) <<  "Starting node server";

    m_autoScaleTimer = platform_system::Timer(m_dispatcher);
    m_workingContextGroup.spawn(std::bind(&NodeServer::autoScaleLoop, this));

    m_workingContextGroup.spawn(std::bind(&NodeServer::acceptLoop, this));
    m_workingContextGroup.spawn(std::bind(&NodeServer::onIdle, this));
    m_workingContextGroup.spawn(std::bind(&NodeServer::timedSyncLoop, this));
    m_workingContextGroup.spawn(std::bind(&NodeServer::timeoutLoop, this));

    m_stopEvent.wait();

    logger(INFO) <<  "Stopping node server and it's, " << m_connections.size() << " connections...";
    m_workingContextGroup.interrupt();
    m_workingContextGroup.wait();

    logger(INFO) <<  "node server loop stopped successfully";
    return true;
  }

  void NodeServer::autoScaleLoop()
  {
    try
    {
      while (true)
      {
        m_autoScaleTimer.sleep(std::chrono::seconds(P2P_AUTO_SCALE_INTERVAL_SECONDS));
        autoScaleConnections();
      }
    }
    catch (const platform_system::InterruptedException &)
    {
      logger(DEBUGGING) << "NodeServer::autoScaleLoop() interrupted";
    }
  }

  void NodeServer::autoScaleConnections()
  {
    size_t currentTarget = m_config.m_netConfig.connections_count;
    size_t currentOutgoing = getOutgoingConnectionsCount();

    // Calculate average write duration across outgoing connections
    uint64_t totalWriteDuration = 0;
    size_t outgoingCount = 0;
    auto now = P2pConnectionContext::Clock::now();

    for (const auto &kv : m_connections)
    {
      const auto &ctx = kv.second;
      if (!ctx.m_isIncoming)
      {
        totalWriteDuration += ctx.writeDuration(now);
        ++outgoingCount;
      }
    }

    // If we have no outgoing connections yet, don't adjust
    if (outgoingCount == 0)
      return;

    uint64_t avgWriteDuration = totalWriteDuration / outgoingCount;

    // Scale up: writes are fast and we're below max
    if (avgWriteDuration < 100 && currentTarget < m_maxOutgoingConnections && currentOutgoing >= currentTarget)
    {
      m_config.m_netConfig.connections_count = std::min(currentTarget + 1, m_maxOutgoingConnections);
      logger(INFO) << "Auto-scale: increasing target connections from "
                   << currentTarget << " to " << m_config.m_netConfig.connections_count;
    }
    // Scale down: writes are slow and we're above min
    else if (avgWriteDuration > 500 && currentTarget > m_minOutgoingConnections)
    {
      m_config.m_netConfig.connections_count = std::max(currentTarget - 1, m_minOutgoingConnections);
      logger(INFO) << "Auto-scale: decreasing target connections from "
                   << currentTarget << " to " << m_config.m_netConfig.connections_count;
    }

    cn::PowService::instance().updatePrefetchForConnections(m_config.m_netConfig.connections_count);
  }

  //-----------------------------------------------------------------------------------

  uint64_t NodeServer::getConnectionsCount() {
    return m_connections.size();
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::deinit()  {
    return storeConfig();
  }

  //-----------------------------------------------------------------------------------

  bool NodeServer::storeConfig()
  {
    try {
      if (!tools::create_directories_if_necessary(m_configFolder)) {
        logger(INFO) <<  "Failed to create data directory: " << m_configFolder;
        return false;
      }

      std::string state_file_path = m_configFolder + "/" + m_p2pStateFilename;
      std::ofstream p2p_data;
      p2p_data.open(state_file_path, std::ios_base::binary | std::ios_base::out | std::ios::trunc);
      if (p2p_data.fail())  {
        logger(INFO) <<  "Failed to save config to file " << state_file_path;
        return false;
      };

      StdOutputStream stream(p2p_data);
      BinaryOutputStreamSerializer a(stream);
      cn::serialize(*this, a);
      return true;
    } catch (const std::exception& e) {
      logger(WARNING) << "storeConfig failed: " << e.what();
    }

    return false;
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::sendStopSignal()  {
    m_stop = true;

    m_dispatcher.remoteSpawn([this] {
      m_stopEvent.set();
      m_payload_handler.stop();
    });

    logger(INFO, BRIGHT_YELLOW) << "Stop signal sent";
    return true;
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::handshake(cn::LevinProtocol& proto, P2pConnectionContext& context, bool just_take_peerlist) {
    COMMAND_HANDSHAKE::request arg;
    COMMAND_HANDSHAKE::response rsp;
    getLocalNodeData(arg.node_data);
    m_payload_handler.get_payload_sync_data(arg.payload_data);

    if (!proto.invoke(COMMAND_HANDSHAKE::ID, arg, rsp)) {
      logger(logging::DEBUGGING) << context << "Failed to invoke COMMAND_HANDSHAKE, closing connection.";
      return false;
    }

    context.version = rsp.node_data.version;

    if (rsp.node_data.network_id != m_network_id) {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE Failed, wrong network!  (" << rsp.node_data.network_id << "), closing connection.";
      return false;
    }

    if (rsp.node_data.version < cn::P2P_MINIMUM_VERSION)
    {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE Failed, peer is wrong version! ("
                                 << std::to_string(rsp.node_data.version) << "), closing connection.";
      return false;
    }
    else if ((rsp.node_data.version - cn::P2P_CURRENT_VERSION) >= cn::P2P_UPGRADE_WINDOW)
    {
      logger(logging::WARNING) << context
                               << "COMMAND_HANDSHAKE Warning, your software may be out of date. Please visit: "
                               << "https://github.com/concealnetwork/conceal-core/releases for the latest version.";
    }

    if (!handleRemotePeerlist(rsp.local_peerlist, rsp.node_data.local_time, context)) {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE: failed to handleRemotePeerlist(...), closing connection.";
      return false;
    }

    if (just_take_peerlist) {
      return true;
    }

    if (!m_payload_handler.process_payload_sync_data(rsp.payload_data, context, true)) {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE invoked, but process_payload_sync_data returned false, dropping connection.";
      return false;
    }

    context.peerId = rsp.node_data.peer_id;
    m_peerlist.set_peer_just_seen(rsp.node_data.peer_id, context.m_remoteIp, context.m_remotePort);

    if (rsp.node_data.peer_id == m_config.m_peerId)  {
      logger(logging::TRACE) << context << "Connection to self detected, dropping connection";
      return false;
    }

    logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE INVOKED OK";
    return true;
  }


  bool NodeServer::timedSync() {
    COMMAND_TIMED_SYNC::request arg{};
    m_payload_handler.get_payload_sync_data(arg.payload_data);
    auto cmdBuf = LevinProtocol::encode<COMMAND_TIMED_SYNC::request>(arg);

    forEachConnection([&cmdBuf](P2pConnectionContext& conn) {
      if (conn.peerId &&
          (conn.m_state == CryptoNoteConnectionContext::stateNormal ||
           conn.m_state == CryptoNoteConnectionContext::stateIdle)) {
        conn.pushMessage(P2pMessage(P2pMessage::COMMAND, COMMAND_TIMED_SYNC::ID, cmdBuf));
      }
    });

    return true;
  }

  bool NodeServer::handleTimedSyncResponse(const BinaryArray& in, P2pConnectionContext& context) {
    COMMAND_TIMED_SYNC::response rsp;
    if (!LevinProtocol::decode<COMMAND_TIMED_SYNC::response>(in, rsp)) {
      return false;
    }

    if (!handleRemotePeerlist(rsp.local_peerlist, rsp.local_time, context)) {
      logger(logging::DEBUGGING) << context << "COMMAND_TIMED_SYNC: failed to handleRemotePeerlist(...), closing connection.";
      return false;
    }

    if (!context.m_isIncoming) {
      m_peerlist.set_peer_just_seen(context.peerId, context.m_remoteIp, context.m_remotePort);
    }

    if (!m_payload_handler.process_payload_sync_data(rsp.payload_data, context, false)) {
      return false;
    }

    return true;
  }

  void NodeServer::forEachConnection(const std::function<void(P2pConnectionContext &)> &action)
  {

    // create copy of connection ids because the list can be changed during action
    std::vector<boost::uuids::uuid> connectionIds;
    connectionIds.reserve(m_connections.size());
    for (const auto& c : m_connections) {
      connectionIds.push_back(c.first);
    }

    for (const auto& connId : connectionIds) {
      auto it = m_connections.find(connId);
      if (it != m_connections.end()) {
        action(it->second);
      }
    }
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::isPeerUsed(const PeerlistEntry& peer) const {
    if(m_config.m_peerId == peer.id)
      return true; //dont make connections to ourself

    for (const auto& kv : m_connections) {
      const auto& cntxt = kv.second;
      if(cntxt.peerId == peer.id || (!cntxt.m_isIncoming && peer.adr.ip == cntxt.m_remoteIp && peer.adr.port == cntxt.m_remotePort)) {
        return true;
      }
    }
    return false;
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::isAddrConnected(const NetworkAddress& peer) const {
    for (const auto& conn : m_connections) {
      if (!conn.second.m_isIncoming && peer.ip == conn.second.m_remoteIp && peer.port == conn.second.m_remotePort) {
        return true;
      }
    }
    return false;
  }

  bool NodeServer::tryToConnectAndHandshakeWithNewPeer(const NetworkAddress &na, bool just_take_peerlist, uint64_t last_seen_stamp, PeerType peer_type, uint64_t first_seen_stamp)
  {
    logger(DEBUGGING) << "Connecting to " << na << " (peer_type=" << peer_type << ", last_seen: "
            << (last_seen_stamp ? common::timeIntervalToString(time(nullptr) - last_seen_stamp) : "never") << ")...";

    try {
      platform_system::TcpConnection connection;

      try {
        doWithTimeoutAndThrow(m_dispatcher, std::chrono::milliseconds(m_config.m_netConfig.connection_timeout), [&] {
          platform_system::TcpConnector connector(m_dispatcher);
          connection = connector.connect(platform_system::Ipv4Address(common::ipAddressToString(na.ip)), static_cast<uint16_t>(na.port));
        });
      } catch (const std::exception&) {
        logger(DEBUGGING) << "Connection timed out";
        return false;
      }

      P2pConnectionContext ctx(m_dispatcher, logger.getLogger(), std::move(connection));

      ctx.m_connectionId = uuidGen();
      ctx.m_remoteIp = na.ip;
      ctx.m_remotePort = na.port;
      ctx.m_isIncoming = false;
      ctx.m_started = time(nullptr);

      bool handshakeResult = false;
      try {
        doWithTimeoutAndThrow(m_dispatcher, std::chrono::milliseconds(m_config.m_netConfig.connection_timeout * 3), [&] {
          cn::LevinProtocol proto(ctx.connection);
          handshakeResult = handshake(proto, ctx, just_take_peerlist);
        });
      } catch (const std::exception&) {
        logger(DEBUGGING) << "Handshake timed out";
        return false;
      }

      if (!handshakeResult) {
        logger(DEBUGGING) << "Failed to HANDSHAKE with peer " << na;
        return false;
      }

      if (just_take_peerlist) {
        logger(logging::DEBUGGING, logging::BRIGHT_GREEN) << ctx << "CONNECTION HANDSHAKED OK AND CLOSED.";
        return true;
      }

      PeerlistEntry pe_local{};
      pe_local.adr = na;
      pe_local.id = ctx.peerId;
      pe_local.last_seen = time(nullptr);
      m_peerlist.append_with_peer_white(pe_local);

      AnchorPeerlistEntry ape{};
      ape.adr = na;
      ape.id = ctx.peerId;
      ape.first_seen = first_seen_stamp ? first_seen_stamp : time(nullptr);
      m_peerlist.append_with_peer_anchor(ape);

      if (m_stop) {
        throw platform_system::InterruptedException();
      }

      auto iter = m_connections.emplace(ctx.m_connectionId, std::move(ctx)).first;
      const boost::uuids::uuid& connectionId = iter->first;
      P2pConnectionContext& connectionContext = iter->second;

      m_workingContextGroup.spawn(std::bind(&NodeServer::connectionHandler, this, std::cref(connectionId), std::ref(connectionContext)));

      return true;
    } catch (const platform_system::InterruptedException&) {
      logger(DEBUGGING) << "Connection to new peer interrupted";
      throw;
    } catch (const std::exception& e) {
      logger(DEBUGGING) << "Connection to " << na << " failed: " << e.what();
    }

    return false;
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::makeNewConnectionFromPeerlist(bool use_white_list)
  {
    size_t local_peers_count = use_white_list ? m_peerlist.get_white_peers_count():m_peerlist.get_gray_peers_count();
    if(!local_peers_count)
      return false;//no peers

    size_t max_random_index = std::min<uint64_t>(local_peers_count -1, 20);

    std::set<size_t> tried_peers;

    size_t try_count = 0;
    size_t rand_count = 0;
    while(rand_count < (max_random_index+1)*3 &&  try_count < 10 && !m_stop) {
      ++rand_count;
      size_t random_index = get_random_index_with_fixed_probability(max_random_index);
      if (!(random_index < local_peers_count)) { logger(ERROR, BRIGHT_RED) << "random_starter_index < peers_local.size() failed!!"; return false; }

      if(tried_peers.count(random_index))
        continue;

      tried_peers.insert(random_index);
      PeerlistEntry pe{};
      bool r = use_white_list ? m_peerlist.get_white_peer_by_index(pe, random_index):m_peerlist.get_gray_peer_by_index(pe, random_index);
      if (!r) { logger(ERROR, BRIGHT_RED) << "Failed to get random peer from peerlist(white:" << use_white_list << ")"; return false; }

      ++try_count;

      if(isPeerUsed(pe))
        continue;

      logger(DEBUGGING) << "Selected peer: " << pe.id << " " << pe.adr << " [peer_list=" << (use_white_list ? white : gray)
                        << "] last_seen: " << (pe.last_seen ? common::timeIntervalToString(time(nullptr) - pe.last_seen) : "never");

      if (!tryToConnectAndHandshakeWithNewPeer(pe.adr, false, pe.last_seen, use_white_list ? white : gray))
        continue;

      return true;
    }
    return false;
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::makeNewConnectionFromAnchorPeerlist(const std::vector<AnchorPeerlistEntry> &anchor_peerlist)
  {
    for (const auto &pe : anchor_peerlist)
    {
      logger(DEBUGGING) << "Considering connecting (out) to peer: " << pe.id << " " << common::ipAddressToString(pe.adr.ip) << ":" << std::to_string(pe.adr.port);

      if (isPeerUsed(pe))
      {
        logger(DEBUGGING) << "Peer is used";
        continue;
      }

      logger(DEBUGGING) << "Selected anchor peer: " << pe.id << " " << common::ipAddressToString(pe.adr.ip)
                        << ":" << std::to_string(pe.adr.port)
                        << "[peer_type=" << anchor
                        << "] first_seen: " << common::timeIntervalToString(time(nullptr) - pe.first_seen);

      if (!tryToConnectAndHandshakeWithNewPeer(pe.adr, false, 0, anchor, pe.first_seen))
      {
        logger(DEBUGGING) << "Handshake failed";
        continue;
      }

      return true;
    }
    return false;
  }


  bool NodeServer::connectionsMaker()
  {
    if (!connectToPeerlist(m_exclusive_peers)) {
      return false;
    }

    if (!m_exclusive_peers.empty()) {
      return true;
    }

    if(!m_peerlist.get_white_peers_count() && m_seed_nodes.size()) {
      size_t try_count = 0;
      size_t current_index = crypto::rand<size_t>() % m_seed_nodes.size();

      while(true) {
        if(tryToConnectAndHandshakeWithNewPeer(m_seed_nodes[current_index], true))
          break;

        if(++try_count > m_seed_nodes.size()) {
          logger(ERROR) << "Failed to connect to any of seed peers, continuing without seeds";
          break;
        }
        if(++current_index >= m_seed_nodes.size())
          current_index = 0;
      }
    }

    if (!connectToPeerlist(m_priority_peers)) return false;

    size_t expected_white_connections = (m_config.m_netConfig.connections_count * cn::P2P_DEFAULT_WHITELIST_CONNECTIONS_PERCENT) / 100;

    size_t conn_count = getOutgoingConnectionsCount();
    if(conn_count < m_config.m_netConfig.connections_count)
    {
      if(conn_count < expected_white_connections)
      {

        if (!makeExpectedConnectionsCount(anchor, P2P_DEFAULT_ANCHOR_CONNECTIONS_COUNT))
          return false;

        //start from white list
        if (!makeExpectedConnectionsCount(white, expected_white_connections))
          return false;
        //and then do grey list
        if (!makeExpectedConnectionsCount(gray, m_config.m_netConfig.connections_count))
          return false;
      }else
      {
        //start from grey list
        if (!makeExpectedConnectionsCount(gray, m_config.m_netConfig.connections_count))
          return false;
        //and then do white list
        if (!makeExpectedConnectionsCount(white, m_config.m_netConfig.connections_count))
          return false;
      }
    }

    return true;
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::makeExpectedConnectionsCount(PeerType peer_type, size_t expected_connections)
  {
    std::vector<AnchorPeerlistEntry> apl;

    if (peer_type == anchor)
    {
      m_peerlist.get_and_empty_anchor_peerlist(apl);
    }
  
      size_t conn_count = getOutgoingConnectionsCount();
      //add new connections from white peers
      while (conn_count < expected_connections)
      {
        if (m_stopEvent.get())
          return false;

        if (peer_type == anchor && !makeNewConnectionFromAnchorPeerlist(apl))
        {
          break;
        }

        if (peer_type == white && !makeNewConnectionFromPeerlist(true))
        {
          break;
        }

        if (peer_type == gray && !makeNewConnectionFromPeerlist(false))
        {
          break;
        }
        conn_count = getOutgoingConnectionsCount();
      }
      return true;
    }

    //-----------------------------------------------------------------------------------
    size_t NodeServer::getOutgoingConnectionsCount() const
    {
      size_t count = 0;
      for (const auto &cntxt : m_connections)
      {
        if (!cntxt.second.m_isIncoming)
          ++count;
      }
      return count;
    }

    //-----------------------------------------------------------------------------------
    bool NodeServer::idleWorker()
    {
      try
      {
        m_connectionsMakerInterval.call(std::bind(&NodeServer::connectionsMaker, this));
        m_peerlistStoreInterval.call(std::bind(&NodeServer::storeConfig, this));
      } catch (const std::exception& e) {
      logger(DEBUGGING) << "exception in idleWorker: " << e.what();
    }
    return true;
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::fixTimeDelta(std::list<PeerlistEntry>& local_peerlist, time_t local_time, int64_t& delta) const
  {
    //fix time delta
    time_t now = 0;
    time(&now);
    delta = now - local_time;

    for (PeerlistEntry& be : local_peerlist)
    {
      if(be.last_seen > uint64_t(local_time))
      {
        logger(DEBUGGING) << "FOUND FUTURE peerlist for entry " << be.adr << " last_seen: " << be.last_seen << ", local_time(on remote node):" << local_time;
        return false;
      }
      be.last_seen += delta;
    }
    return true;
  }

  //-----------------------------------------------------------------------------------

  bool NodeServer::handleRemotePeerlist(const std::list<PeerlistEntry>& peerlist, time_t local_time, const CryptoNoteConnectionContext& context)
  {
    int64_t delta = 0;
    std::list<PeerlistEntry> peerlist_ = peerlist;
    if(!fixTimeDelta(peerlist_, local_time, delta))
      return false;
    logger(logging::TRACE) << context << "REMOTE PEERLIST: TIME_DELTA: " << delta << ", remote peerlist size=" << peerlist_.size();
    logger(logging::TRACE) << context << "REMOTE PEERLIST: " <<  print_peerlist_to_string(peerlist_);
    return m_peerlist.merge_peerlist(peerlist_);
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::getLocalNodeData(basic_node_data& node_data) const
  {
    node_data.version = cn::P2P_CURRENT_VERSION;
    time_t local_time;
    time(&local_time);
    node_data.local_time = local_time;
    node_data.peer_id = m_config.m_peerId;
    if(!m_hideMyPort)
      node_data.my_port = m_externalPort ? m_externalPort : m_listeningPort;
    else
      node_data.my_port = 0;
    node_data.network_id = m_network_id;
    return true;
  }
  //-----------------------------------------------------------------------------------

  void NodeServer::relayNotifyToAll(int command, const BinaryArray& data_buff, const net_connection_id* excludeConnection) {
    net_connection_id excludeId = excludeConnection ? *excludeConnection : net_connection_id{};

    forEachConnection([&excludeId, &command, &data_buff](P2pConnectionContext& conn) {
      if (conn.peerId && conn.m_connectionId != excludeId &&
          (conn.m_state == CryptoNoteConnectionContext::stateNormal ||
           conn.m_state == CryptoNoteConnectionContext::stateSynchronizing)) {
        conn.pushMessage(P2pMessage(P2pMessage::NOTIFY, command, data_buff));
      }
    });
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::invokeNotifyToPeer(int command, const BinaryArray& buffer, const CryptoNoteConnectionContext& context) {
    auto it = m_connections.find(context.m_connectionId);
    if (it == m_connections.end()) {
      return false;
    }

    it->second.pushMessage(P2pMessage(P2pMessage::NOTIFY, command, buffer));

    return true;
  }

  //-----------------------------------------------------------------------------------
  bool NodeServer::tryPing(const basic_node_data& node_data, const P2pConnectionContext& context) {
    if(!node_data.my_port) {
      return false;
    }

    uint32_t actual_ip =  context.m_remoteIp;
    if(!m_peerlist.is_ip_allowed(actual_ip)) {
      return false;
    }

    auto ip = common::ipAddressToString(actual_ip);
    auto port = node_data.my_port;
    auto peerId = node_data.peer_id;

    COMMAND_PING::request req;
    COMMAND_PING::response rsp;

    try {
      doWithTimeoutAndThrow(m_dispatcher, std::chrono::milliseconds(m_config.m_netConfig.connection_timeout * 2), [&] {
        platform_system::TcpConnector connector(m_dispatcher);
        auto connection = connector.connect(platform_system::Ipv4Address(ip), static_cast<uint16_t>(port));
        LevinProtocol(connection).invoke(COMMAND_PING::ID, req, rsp);
      });
    } catch (const std::exception& e) {
      logger(DEBUGGING) << context << "Back ping connection to " << ip << ":" << port << " failed: " << e.what();
      return false;
    }

    if (rsp.status != PING_OK_RESPONSE_STATUS_TEXT || peerId != rsp.peer_id) {
      logger(DEBUGGING) << context << "Back ping invoke wrong response \"" << rsp.status << "\" from" << ip
                                 << ":" << port << ", hsh_peer_id=" << peerId << ", rsp.peer_id=" << rsp.peer_id;
      return false;
    }

    return true;
  }

  //-----------------------------------------------------------------------------------
  int NodeServer::handleTimedSync(int command, const COMMAND_TIMED_SYNC::request& arg, COMMAND_TIMED_SYNC::response& rsp, P2pConnectionContext& context)
  {
    if(!m_payload_handler.process_payload_sync_data(arg.payload_data, context, false)) {
      logger(logging::DEBUGGING) << context << "Failed to process_payload_sync_data(), dropping connection";
      context.m_state = CryptoNoteConnectionContext::stateShutdown;
      return 1;
    }

    //fill response
    rsp.local_time = time(nullptr);
    m_peerlist.get_peerlist_head(rsp.local_peerlist);
    m_payload_handler.get_payload_sync_data(rsp.payload_data);
    logger(logging::TRACE) << context << "COMMAND_TIMED_SYNC";
    return 1;
  }
  //-----------------------------------------------------------------------------------

  int NodeServer::handleHandshake(int command, const COMMAND_HANDSHAKE::request& arg, COMMAND_HANDSHAKE::response& rsp, P2pConnectionContext& context)
  {
    context.version = arg.node_data.version;

    if (arg.node_data.network_id != m_network_id) {
      logger(logging::INFO) << context << "WRONG NETWORK AGENT CONNECTED! id=" << arg.node_data.network_id;
      context.m_state = CryptoNoteConnectionContext::stateShutdown;
      return 1;
    }

    if (arg.node_data.version < cn::P2P_MINIMUM_VERSION) {
      logger(logging::DEBUGGING) << context << "UNSUPPORTED NETWORK AGENT VERSION CONNECTED! version=" << std::to_string(arg.node_data.version);
      context.m_state = CryptoNoteConnectionContext::stateShutdown;
      return 1;
    } else if (arg.node_data.version > cn::P2P_CURRENT_VERSION) {
      logger(logging::WARNING) << context << "Warning, your software may be out of date. Please upgrare to the latest version.";
    }

    if(!context.m_isIncoming) {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE came not from incoming connection";
      context.m_state = CryptoNoteConnectionContext::stateShutdown;
      return 1;
    }

    if(context.peerId) {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE came, but seems that connection already have associated peer_id (double COMMAND_HANDSHAKE?)";
      context.m_state = CryptoNoteConnectionContext::stateShutdown;
      return 1;
    }

    if(!m_payload_handler.process_payload_sync_data(arg.payload_data, context, true))  {
      logger(logging::DEBUGGING) << context << "COMMAND_HANDSHAKE came, but process_payload_sync_data returned false, dropping connection.";
      context.m_state = CryptoNoteConnectionContext::stateShutdown;
      return 1;
    }
    //associate peer_id with this connection
    context.peerId = arg.node_data.peer_id;

    if(arg.node_data.peer_id != m_config.m_peerId && arg.node_data.my_port) {
      PeerIdType peer_id_l = arg.node_data.peer_id;
      uint32_t port_l = arg.node_data.my_port;

      if (tryPing(arg.node_data, context)) {
          //called only(!) if success pinged, update local peerlist
          PeerlistEntry pe;
          pe.adr.ip = context.m_remoteIp;
          pe.adr.port = port_l;
          pe.last_seen = time(nullptr);
          pe.id = peer_id_l;
          m_peerlist.append_with_peer_white(pe);

          logger(logging::TRACE) << context << "BACK PING SUCCESS, " << common::ipAddressToString(context.m_remoteIp) << ":" << port_l << " added to whitelist";
      }
    }

    //fill response
    m_peerlist.get_peerlist_head(rsp.local_peerlist);
    getLocalNodeData(rsp.node_data);
    m_payload_handler.get_payload_sync_data(rsp.payload_data);

    logger(logging::DEBUGGING, logging::BRIGHT_GREEN) << "COMMAND_HANDSHAKE";
    return 1;
  }
  //-----------------------------------------------------------------------------------

  int NodeServer::handlePing(int command, const COMMAND_PING::request& arg, COMMAND_PING::response& rsp, const P2pConnectionContext& context) const
  {
    logger(logging::TRACE) << context << "COMMAND_PING";
    rsp.status = PING_OK_RESPONSE_STATUS_TEXT;
    rsp.peer_id = m_config.m_peerId;
    return 1;
  }
  //-----------------------------------------------------------------------------------

  bool NodeServer::logPeerlist() const
  {
    std::list<PeerlistEntry> pl_wite;
    std::list<PeerlistEntry> pl_gray;
    m_peerlist.get_peerlist_full(pl_gray, pl_wite);
    logger(INFO) << std::endl
                 << "Peerlist white:\n"
                 << print_peerlist_to_string(pl_wite) << std::endl
                 << "Peerlist gray:\n"
                 << print_peerlist_to_string(pl_gray);
    return true;
  }
  //-----------------------------------------------------------------------------------

  std::string NodeServer::printConnectionsContainer() const {

    std::stringstream ss;

    for (const auto& cntxt : m_connections) {
      ss << common::ipAddressToString(cntxt.second.m_remoteIp) << ":" << cntxt.second.m_remotePort
        << " \t\tpeer_id " << cntxt.second.peerId
        << " \t\tconn_id " << cntxt.second.m_connectionId << (cntxt.second.m_isIncoming ? " INC" : " OUT")
        << std::endl;
    }

    return ss.str();
  }
  //-----------------------------------------------------------------------------------

  void NodeServer::onConnectionNew(P2pConnectionContext& context)
  {
    logger(TRACE) << context << "NEW CONNECTION";
    m_payload_handler.onConnectionOpened(context);
  }
  //-----------------------------------------------------------------------------------

  void NodeServer::onConnectionClose(P2pConnectionContext& context)
  {

    if (!m_stopEvent.get() && !context.m_isIncoming)
    {
      NetworkAddress na;
      na.ip = context.m_remoteIp;
      na.port = context.m_remotePort;

      m_peerlist.remove_from_peer_anchor(na);
    }

    logger(TRACE) << context << "CLOSE CONNECTION";
    m_payload_handler.onConnectionClosed(context);
  }

  bool NodeServer::isPriorityNode(const NetworkAddress& na)
  {
    return
      (std::find(m_priority_peers.begin(), m_priority_peers.end(), na) != m_priority_peers.end()) ||
      (std::find(m_exclusive_peers.begin(), m_exclusive_peers.end(), na) != m_exclusive_peers.end());
  }

  bool NodeServer::connectToPeerlist(const std::vector<NetworkAddress>& peers)
  {
    for(const auto& na: peers) {
      if (!isAddrConnected(na)) {
        tryToConnectAndHandshakeWithNewPeer(na);
      }
    }

    return true;
  }

  void NodeServer::acceptLoop() {
    for (;;) {
      try {
        P2pConnectionContext ctx(m_dispatcher, logger.getLogger(), m_listener.accept());
        ctx.m_connectionId = uuidGen();
        ctx.m_isIncoming = true;
        ctx.m_started = time(nullptr);

        auto addressAndPort = ctx.connection.getPeerAddressAndPort();
        ctx.m_remoteIp = hostToNetwork(addressAndPort.first.getValue());
        ctx.m_remotePort = addressAndPort.second;

        auto iter = m_connections.emplace(ctx.m_connectionId, std::move(ctx)).first;
        const boost::uuids::uuid& connectionId = iter->first;
        P2pConnectionContext& connection = iter->second;

        m_workingContextGroup.spawn(std::bind(&NodeServer::connectionHandler, this, std::cref(connectionId), std::ref(connection)));
      } catch (const platform_system::InterruptedException&) {
        logger(DEBUGGING) << "NodeServer::acceptLoop() is interrupted";
        break;
      } catch (const std::exception& e) {
        logger(DEBUGGING) << "Exception in NodeServer::acceptLoop(): " << e.what();
      }
    }

    logger(DEBUGGING) << "NodeServer::acceptLoop() finished";
  }

  void NodeServer::onIdle() {
    logger(DEBUGGING) << "NodeServer::onIdle() started";

    try {
      while (!m_stop) {
        idleWorker();
        m_payload_handler.on_idle();
        m_idleTimer.sleep(std::chrono::seconds(1));
      }
    } catch (const platform_system::InterruptedException&) {
      logger(DEBUGGING) << "NodeServer::onIdle() is interrupted";
    } catch (const std::exception& e) {
      logger(DEBUGGING) << "Exception in NodeServer::onIdle(): " << e.what();
    }

    logger(DEBUGGING) << "NodeServer::onIdle() finished";
  }

  void NodeServer::timeoutLoop() {
    try {
      while (!m_stop) {
        m_timeoutTimer.sleep(std::chrono::seconds(10));
        auto now = P2pConnectionContext::Clock::now();

        for (auto& kv : m_connections) {
          auto& ctx = kv.second;
          if (ctx.writeDuration(now) > P2P_DEFAULT_INVOKE_TIMEOUT) {
            logger(DEBUGGING) << ctx << "write operation timed out, stopping connection";
            safeInterrupt(ctx);
          }
        }
      }
    } catch (const platform_system::InterruptedException&) {
      logger(DEBUGGING) << "NodeServer::timeoutLoop() is interrupted";
    } catch (const std::exception& e) {
      logger(DEBUGGING) << "Exception in NodeServer::timeoutLoop(): " << e.what();
    }
  }

  void NodeServer::timedSyncLoop() {
    try {
      for (;;) {
        m_timedSyncTimer.sleep(std::chrono::seconds(P2P_DEFAULT_HANDSHAKE_INTERVAL));
        timedSync();
      }
    } catch (const platform_system::InterruptedException&) {
      logger(DEBUGGING) << "NodeServer::timedSyncLoop() is interrupted";
    } catch (const std::exception& e) {
      logger(DEBUGGING) << "Exception in NodeServer::timedSyncLoop(): " << e.what();
    }

    logger(DEBUGGING) << "NodeServer::timedSyncLoop() finished";
  }

  void NodeServer::connectionHandler(const boost::uuids::uuid& connectionId, P2pConnectionContext& ctx) {
    // This inner context is necessary in order to stop connection handler at any moment
    platform_system::Context<> context(m_dispatcher, [this, &connectionId, &ctx] {
      platform_system::Context<> writeContext(m_dispatcher, std::bind(&NodeServer::writeHandler, this, std::ref(ctx)));

      try {
        onConnectionNew(ctx);

        LevinProtocol proto(ctx.connection);
        LevinProtocol::Command cmd;

        for (;;) {
          if (ctx.m_state == CryptoNoteConnectionContext::stateSyncRequired) {
            ctx.m_state = CryptoNoteConnectionContext::stateSynchronizing;
            m_payload_handler.start_sync(ctx);
          } else if (ctx.m_state == CryptoNoteConnectionContext::statePoolSyncRequired) {
            ctx.m_state = CryptoNoteConnectionContext::stateNormal;
            m_payload_handler.requestMissingPoolTransactions(ctx);
          }

          if (!proto.readCommand(cmd)) {
            break;
          }

          BinaryArray response;
          bool handled = false;
          auto retcode = handleCommand(cmd, response, ctx, handled);

          // send response
          if (cmd.needReply()) {
            if (!handled) {
              retcode = static_cast<int32_t>(LevinError::ERROR_CONNECTION_HANDLER_NOT_DEFINED);
              response.clear();
            }

            ctx.pushMessage(P2pMessage(P2pMessage::REPLY, cmd.command, response, retcode));
          }

          if (ctx.m_state == CryptoNoteConnectionContext::stateShutdown) {
            break;
          }
        }
      } catch (const platform_system::InterruptedException&) {
        logger(DEBUGGING) << ctx << "NodeServer::connectionHandler() inner context is interrupted";
      } catch (const std::exception& e) {
        logger(DEBUGGING) << ctx << "Exception in NodeServer::connectionHandler(): " << e.what();
      }

      safeInterrupt(ctx);
      safeInterrupt(writeContext);
      writeContext.wait();

      onConnectionClose(ctx);
      m_connections.erase(connectionId);
    });

    ctx.context = &context;

    try {
      context.get();
    } catch (const platform_system::InterruptedException&) {
      logger(DEBUGGING) << "NodeServer::connectionHandler() is interrupted";
    }
  }

  void NodeServer::writeHandler(P2pConnectionContext& ctx) const {
    logger(DEBUGGING) << ctx << "NodeServer::writeHandler() started";

    try {
      LevinProtocol proto(ctx.connection);

      for (;;) {
        auto msgs = ctx.popBuffer();
        if (msgs.empty()) {
          break;
        }

        for (const auto& msg : msgs) {
          logger(DEBUGGING) << ctx << "msg " << msg.type << ':' << msg.command;
          switch (msg.type) {
          case P2pMessage::COMMAND:
            proto.sendMessage(msg.command, msg.buffer, true);
            break;
          case P2pMessage::NOTIFY:
            proto.sendMessage(msg.command, msg.buffer, false);
            break;
          case P2pMessage::REPLY:
            proto.sendReply(msg.command, msg.buffer, msg.returnCode);
            break;
          default:
            assert(false);
          }
        }
      }
    } catch (const platform_system::InterruptedException&) {
      // connection stopped
      logger(DEBUGGING) << ctx << "NodeServer::writeHandler() is interrupted";
    } catch (const std::exception& e) {
      logger(DEBUGGING) << ctx << "NodeServer::writeHandler() error during write: " << e.what();
      safeInterrupt(ctx); // stop connection on write error
    }

    logger(DEBUGGING) << ctx << "NodeServer::writeHandler() finished";
  }

  template<typename T>
  void NodeServer::safeInterrupt(T& obj) const {
    try {
      obj.interrupt();
    } catch (const std::exception& e) {
      logger(DEBUGGING) << "NodeServer::safeInterrupt() throws exception: " << e.what();
    } catch (...) {
      logger(DEBUGGING) << "NodeServer::safeInterrupt() throws unknown exception";
    }
  }
  }
