// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
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
  class MultiWalletManager;

  // JSON-RPC Request / Response
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

  // Method handler type
  using RpcMethod = std::function<common::JsonValue(const common::JsonValue &params)>;

  class BoltRpcServer
  {
  public:
    // Single-wallet constructor
    BoltRpcServer(uint16_t port,
                  cn::INode &node,
                  const cn::Currency &currency,
                  const std::string &dataDir,
                  const std::string &daemonHost,
                  uint16_t daemonPort);

    // Multi-wallet constructor
    BoltRpcServer(uint16_t port,
                  MultiWalletManager &multiWallet,
                  const cn::Currency &currency,
                  const std::string &dataDir,
                  const std::string &daemonHost,
                  uint16_t daemonPort);

    ~BoltRpcServer();

    // Lifecycle
    bool start();
    void stop();
    bool isRunning() const;

    // Wallet access (for CLI / GUI to call directly)
    WalletManager &wallet() { return *m_walletManager; }
    const WalletManager &wallet() const { return *m_walletManager; }

    // CORS
    void setCorsDomain(const std::string &domain);

  private:
    // HTTP server
    void httpListenLoop();
    void handleRequest(const std::string &requestBody, std::string &responseBody);
    void handleJsonRpc(const std::string &requestBody, std::string &responseBody);

    // Method registration
    void registerMethods();
    void registerMultiMethods();
    RpcMethod findMethod(const std::string &name) const;

    // Single-wallet RPC methods
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
    common::JsonValue rpc_listWallets(const common::JsonValue &params);
    common::JsonValue rpc_setActiveWallet(const common::JsonValue &params);

    // Multi-wallet RPC methods
    common::JsonValue rpc_multi_getStatus(const common::JsonValue &params);
    common::JsonValue rpc_multi_getBalance(const common::JsonValue &params);
    common::JsonValue rpc_multi_getAddress(const common::JsonValue &params);
    common::JsonValue rpc_multi_getOutputs(const common::JsonValue &params);
    common::JsonValue rpc_multi_getTransactions(const common::JsonValue &params);
    common::JsonValue rpc_multi_createWallet(const common::JsonValue &params);
    common::JsonValue rpc_multi_importWallet(const common::JsonValue &params);
    common::JsonValue rpc_multi_loadWallet(const common::JsonValue &params);
    common::JsonValue rpc_multi_unloadWallet(const common::JsonValue &params);
    common::JsonValue rpc_multi_deleteWallet(const common::JsonValue &params);
    common::JsonValue rpc_multi_sendTransaction(const common::JsonValue &params);
    common::JsonValue rpc_multi_sendDeposit(const common::JsonValue &params);
    common::JsonValue rpc_multi_sendWithdrawal(const common::JsonValue &params);
    common::JsonValue rpc_multi_syncNow(const common::JsonValue &params);
    common::JsonValue rpc_multi_exportKeys(const common::JsonValue &params);
    common::JsonValue rpc_multi_exportState(const common::JsonValue &params);
    common::JsonValue rpc_multi_listWallets(const common::JsonValue &params);
    common::JsonValue rpc_multi_changePassword(const common::JsonValue &params);
    common::JsonValue rpc_multi_importFromStateFile(const common::JsonValue &params);

    // Helpers
    common::JsonValue makeErrorObj(int code, const std::string &message) const;
    std::string extractWalletId(const common::JsonValue &params) const;

    // Members
    uint16_t m_port;
    cn::INode *m_node; // pointer — nullptr in multi mode
    const cn::Currency &m_currency;
    std::string m_dataDir;
    std::string m_corsDomain;
    std::string m_daemonHost;
    uint16_t m_daemonPort;

    // Mode
    bool m_multiWalletMode = false;

    // Single-wallet
    std::unique_ptr<WalletManager> m_walletManager;

    // Multi-wallet (non-owning — owned by main)
    MultiWalletManager *m_multiWalletManager = nullptr;

    std::unordered_map<std::string, RpcMethod> m_methods;
    std::atomic<bool> m_running{false};
    int m_serverSocket = -1;

    static constexpr int JSON_RPC_PARSE_ERROR = -32700;
    static constexpr int JSON_RPC_METHOD_NOT_FOUND = -32601;
    static constexpr int JSON_RPC_INVALID_PARAMS = -32602;
    static constexpr int JSON_RPC_INTERNAL_ERROR = -32603;
    static constexpr int WALLET_LOCKED = -1;
    static constexpr int WALLET_NOT_FOUND = -2;
    static constexpr int WALLET_ALREADY_EXISTS = -3;
    static constexpr int INVALID_PASSWORD = -4;
    static constexpr int SYNC_IN_PROGRESS = -5;
    static constexpr int NO_WALLET_LOADED = -6;
  };

} // namespace BoltRPC