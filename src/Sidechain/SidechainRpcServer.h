// SidechainRpcServer.h — JSON-RPC server for sidechain user interactions
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <mutex>

#include "BoltDex.h"
#include "SidechainTypes.h"
#include "SidechainStorage.h"
#include "SidechainValidator.h"
#include "Common/JsonValue.h"
#include "Logging/LoggerRef.h"
#include "BoltHttp/BoltHttpServer.h"
#include "BoltHttp/BoltWebSocket.h"
#include "BoltHttp/BoltSse.h"

namespace Sidechain
{
  namespace BoltAMM
  {
    class Engine;
  }

  class SidechainRpcServer
  {
  public:
    SidechainRpcServer(logging::ILogger &logger,
                       SidechainStorage &storage,
                       SidechainValidator &validator);

    void start(const std::string &bindIp, uint16_t bindPort, size_t threadCount = 1);
    void stop();

    void setSidechainEndpoint(const std::string &host, uint16_t port)
    {
      m_sidechainHost = host;
      m_sidechainPort = port;
    }

    void setTestnet(bool testnet) { m_testnet = testnet; }

    void setDexEngine(Sidechain::BoltDex::Engine *engine) { m_dexEngine = engine; }

    // Access the underlying HTTP server for WebSocket/SSE setup
    BoltHttp::Server *server() { return m_httpServer.get(); }

    // Push a sidechain block event to all connected clients
    void pushBlockEvent(uint64_t height, uint64_t txCount, size_t votes);

    // Push a DEX trade event to all WebSocket clients
    void pushDexTrade(const Sidechain::BoltDex::Trade &trade);

    // Push a bridge deposit event
    void pushBridgeDeposit(uint64_t amount, const std::string &destHex, const std::string &txHash);

    void setAmmEngine(Sidechain::BoltAMM::Engine *engine) { m_ammEngine = engine; }

  private:
    std::string handleJsonRpc(const std::string &body);

    // WebSocket handler: manages real-time DEX order book subscriptions
    void handleWebSocketConnection(BoltHttp::WebSocket &ws);

    // Balance check helper for transfer and burn validation
    bool checkBalance(const crypto::PublicKey &sender, uint64_t tokenId,
                      uint64_t amount, uint64_t fee);

    // Account methods
    std::string methodGetBalance(const common::JsonValue &params);
    std::string methodGetTokenBalance(const common::JsonValue &params);
    std::string methodGetTokens(const common::JsonValue &params);
    std::string methodGetTokenByFingerprint(const common::JsonValue &params);
    std::string methodTransfer(const common::JsonValue &params);
    std::string methodCreateToken(const common::JsonValue &params);
    std::string methodMintToken(const common::JsonValue &params);
    std::string methodBurnToken(const common::JsonValue &params);
    std::string methodGetStatus(const common::JsonValue &params);
    std::string methodGetPendingTransactions(const common::JsonValue &params);
    std::string methodGetValidators(const common::JsonValue &params);
    std::string methodGetTransactions(const common::JsonValue &params);
    std::string methodFaucet(const common::JsonValue &params);

    // Asset registry, bridge, and equivalence
    std::string methodGetAssetRegistry(const common::JsonValue &params);
    std::string methodGetEquivalenceGroup(const common::JsonValue &params);
    std::string methodGetBridgeStatus(const common::JsonValue &params);
    std::string methodBridgeUnlock(const common::JsonValue &params);

    // Legacy sidechain aliases
    std::string methodGetSidechainStatus(const common::JsonValue &params);
    std::string methodGetSidechainTokens(const common::JsonValue &params);
    std::string methodSidechainTransfer(const common::JsonValue &params);
    std::string methodSidechainCreateToken(const common::JsonValue &params);

    // DEX methods
    std::string methodDexGetOrders(const common::JsonValue &params);
    std::string methodDexGetTrades(const common::JsonValue &params);
    std::string methodDexGetAllTrades(const common::JsonValue &params);
    std::string methodDexSubmitOrder(const common::JsonValue &params);
    std::string methodDexCancelOrder(const common::JsonValue &params);
    std::string methodDexDeposit(const common::JsonValue &params);
    std::string methodDexWithdraw(const common::JsonValue &params);
    std::string methodDexGetEscrowBalance(const common::JsonValue &params);

    // AMM methods
    std::string methodAmmGetPools(const common::JsonValue &params);
    std::string methodAmmGetQuote(const common::JsonValue &params);
    std::string methodAmmCreatePool(const common::JsonValue &params);
    std::string methodAmmAddLiquidity(const common::JsonValue &params);
    std::string methodAmmRemoveLiquidity(const common::JsonValue &params);
    std::string methodAmmSwap(const common::JsonValue &params);
    std::string methodAmmGetPositions(const common::JsonValue &params);

    // Active WebSocket connections for real-time push
    std::vector<BoltHttp::WebSocket *> m_wsClients;
    std::mutex m_wsMutex;

    logging::LoggerRef m_logger;
    SidechainStorage &m_storage;
    SidechainValidator &m_validator;
    std::unique_ptr<BoltHttp::Server> m_httpServer;

    Sidechain::BoltDex::Engine *m_dexEngine = nullptr;
    Sidechain::BoltAMM::Engine *m_ammEngine = nullptr;

    std::string m_sidechainHost = "127.0.0.1";
    uint16_t m_sidechainPort = 8080;
    bool m_testnet = false;
  };
}