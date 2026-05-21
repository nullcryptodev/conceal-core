// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include<atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "WalletManager.h"
#include "Common/JsonValue.h"

namespace cn
{
  class INode;
  class Currency;
}

namespace BoltRPC
{

  class WalletManager;

  // ─── JSON-RPC Request / Response ───────────────────────────────────────────

  struct JsonRpcRequest
  {
    std::string jsonrpc;
    std::string method;
    common::JsonValue params;
    common::JsonValue id;
  };

  struct JsonRpcResponse
  {
    common::JsonValue result;
    common::JsonValue error;
    common::JsonValue id;

    std::string toJson() const;
  };

  // ─── Method handler type ───────────────────────────────────────────────────

  using RpcMethod = std::function<common::JsonValue(const common::JsonValue &params)>;

  // ─── BoltRpcServer ─────────────────────────────────────────────────────────

  class BoltRpcServer
  {
  public:
    BoltRpcServer(uint16_t port,
                  cn::INode &node,
                  const cn::Currency &currency,
                  const std::string &dataDir,
                  const std::string &daemonHost,
                  uint16_t daemonPort);
    ~BoltRpcServer();

    // ── Lifecycle ──────────────────────────────────────────────────────────
    bool start();
    void stop();
    bool isRunning() const;

    // ── Wallet access (for CLI / GUI to call directly) ─────────────────────
    WalletManager &wallet() { return *m_walletManager; }
    const WalletManager &wallet() const { return *m_walletManager; }

    // ── CORS ───────────────────────────────────────────────────────────────
    void setCorsDomain(const std::string &domain);

  private:
    // ── HTTP server ────────────────────────────────────────────────────────
    void httpListenLoop();
    void handleRequest(const std::string &requestBody, std::string &responseBody);
    void handleJsonRpc(const std::string &requestBody, std::string &responseBody);

    // ── Method registration ────────────────────────────────────────────────
    void registerMethods();
    RpcMethod findMethod(const std::string &name) const;

    // ── RPC methods ────────────────────────────────────────────────────────
    common::JsonValue rpc_getStatus(const common::JsonValue &params);
    common::JsonValue rpc_getBalance(const common::JsonValue &params);
    common::JsonValue rpc_getAddress(const common::JsonValue &params);
    common::JsonValue rpc_getOutputs(const common::JsonValue &params);
    common::JsonValue rpc_getTransactions(const common::JsonValue &params);
    common::JsonValue rpc_createWallet(const common::JsonValue &params);
    common::JsonValue rpc_importWallet(const common::JsonValue &params);
    common::JsonValue rpc_unlockWallet(const common::JsonValue &params);
    common::JsonValue rpc_lockWallet(const common::JsonValue &params);
    common::JsonValue rpc_sendTransaction(const common::JsonValue &params);
    common::JsonValue rpc_sendDeposit(const common::JsonValue &params);
    common::JsonValue rpc_sendWithdrawal(const common::JsonValue &params);
    common::JsonValue rpc_syncNow(const common::JsonValue &params);
    common::JsonValue rpc_exportKeys(const common::JsonValue &params);
    common::JsonValue rpc_exportState(const common::JsonValue &params);

    // ── Helpers ────────────────────────────────────────────────────────────
    common::JsonValue makeResult(const common::JsonValue &data) const;
    common::JsonValue makeError(int code, const std::string &message) const;

    // ── Members ────────────────────────────────────────────────────────────
    uint16_t m_port;
    cn::INode &m_node;
    const cn::Currency &m_currency;
    std::string m_dataDir;
    std::string m_corsDomain;
    std::string m_daemonHost;
    uint16_t m_daemonPort;

    std::unique_ptr<WalletManager> m_walletManager;
    std::unordered_map<std::string, RpcMethod> m_methods;
    std::atomic<bool> m_running{false};
    int m_serverSocket = -1;
    std::thread m_httpThread;

    static constexpr int JSON_RPC_PARSE_ERROR = -32700;
    static constexpr int JSON_RPC_INVALID_REQUEST = -32600;
    static constexpr int JSON_RPC_METHOD_NOT_FOUND = -32601;
    static constexpr int JSON_RPC_INVALID_PARAMS = -32602;
    static constexpr int JSON_RPC_INTERNAL_ERROR = -32603;
    static constexpr int WALLET_LOCKED = -1;
    static constexpr int WALLET_NOT_FOUND = -2;
    static constexpr int WALLET_ALREADY_EXISTS = -3;
    static constexpr int INVALID_PASSWORD = -4;
    static constexpr int SYNC_IN_PROGRESS = -5;
  };

} // namespace BoltRPC