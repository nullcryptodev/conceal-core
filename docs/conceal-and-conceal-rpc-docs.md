# Conceal Unified RPC API Reference

## Contents

### Conceal RPC (Port 8070)

#### System

| Method | Description |
|--------|-------------|
| [getVersion](#getversion) | Get Conceal version string |

#### Wallet Lifecycle

| Method | Description |
|--------|-------------|
| [generateWallet](#generatewallet) | Generate a new wallet with random keys |
| [importWallet](#importwallet) | Import a wallet by view key and optional spend key |
| [unlock](#unlock) | Unlock wallet by reloading saved keys |
| [lock](#lock) | Lock wallet, zeroing spend key from memory |
| [getViewKey](#getviewkey) | Get the wallet view key (if unlocked) |
| [getSpendKey](#getspendkey) | Get the wallet spend key (if unlocked) |
| [getWalletHeight](#getwalletheight) | Get current wallet sync height and output count |
| [exportWallet](#exportwallet) | Export wallet keys as base64-encoded JSON blob |

#### Mainchain Wallet

| Method | Description |
|--------|-------------|
| [getBalance](#getbalance) | Get wallet mainchain CCX balance |
| [getAddress](#getaddress) | Get wallet mainchain address |
| [getStatus](#getstatus) | Get daemon and wallet sync status |
| [getSyncStatus](#getsyncstatus) | Get detailed wallet scan progress |
| [getNetworkHeight](#getnetworkheight) | Get network block height from daemon |
| [transfer](#transfer) | Send CCX to one or more addresses |
| [getTransactions](#gettransactions) | List wallet transaction history with pagination |
| [createDeposit](#createdeposit) | Lock CCX in a time-locked deposit |
| [withdrawDeposit](#withdrawdeposit) | Withdraw a matured deposit |
| [getDeposits](#getdeposits) | List all deposits |
| [estimateFusion](#estimatefusion) | Estimate outputs available for fusion |
| [sendFusionTransaction](#sendfusiontransaction) | Consolidate small outputs |
| [reset](#reset) | Mark wallet for full rescan on restart |
| [save](#save) | Persist wallet state to disk |

#### Sidechain Proxy

| Method | Description |
|--------|-------------|
| [getSidechainStatus](#getsidechainstatus) | Get sidechain connection or full status |
| [getSidechainTokens](#getsidechaintokens) | List all sidechain tokens |
| [getTokenBalance](#gettokenbalance) | Get sidechain token balance |
| [sidechainTransfer](#sidechaintransfer) | Transfer sidechain tokens |
| [sidechainCreateToken](#sidechaincreatetoken) | Create a new sidechain token |

#### DEX Proxy

| Method | Description |
|--------|-------------|
| [dexGetOrderBook](#dexgetorderbook) | Get open orders for a trading pair |
| [dexPlaceOrder](#dexplaceorder) | Submit a new DEX order |
| [dexCancelOrder](#dexcancelorder) | Cancel an open DEX order |
| [dexGetMyOrders](#dexgetmyorders) | Get orders owned by an address |
| [dexGetTradeHistory](#dexgettradehistory) | Get recent trades for a pair |
| [dexGetEscrowBalance](#dexgetescrowbalance) | Get DEX escrow balance |

#### Bridge Proxy

| Method | Description |
|--------|-------------|
| [bridgeGetStatus](#bridgegetstatus) | Get bridge assets and pending unlocks |
| [bridgeLock](#bridgelock) | Lock CCX on mainchain for bridging |
| [bridgeUnlock](#bridgeunlock) | Request CCX unlock from bridge |

---

### Sidechain RPC (Port 8080)

#### Account Methods

| Method | Description |
|--------|-------------|
| [getBalance](#sidechain-getbalance) | Get native SCCX balance |
| [getTokenBalance](#sidechain-gettokenbalance) | Get token balance |

#### Token Methods

| Method | Description |
|--------|-------------|
| [getTokens](#gettokens) | List all registered tokens |
| [getTokenByFingerprint](#gettokenbyfingerprint) | Look up token by fingerprint |
| [createToken](#createtoken) | Create a new token |

#### Transaction Methods

| Method | Description |
|--------|-------------|
| [transfer](#sidechain-transfer) | Transfer tokens between addresses |
| [mintToken](#minttoken) | Mint new tokens (bridge only) |
| [burnToken](#burntoken) | Burn tokens (bridge unlock) |
| [getTransactions](#sidechain-gettransactions) | Get address transaction history |
| [getPendingTransactions](#getpendingtransactions) | Get mempool transaction count |

#### Status and Validators

| Method | Description |
|--------|-------------|
| [getStatus](#sidechain-getstatus) | Get sidechain status |
| [getValidators](#getvalidators) | List active validators |

#### Asset Registry and Bridge

| Method | Description |
|--------|-------------|
| [getAssetRegistry](#getassetregistry) | List all bridged assets |
| [getEquivalenceGroup](#getequivalencegroup) | Get tokens in equivalence class |
| [getBridgeStatus](#getbridgestatus) | Get bridge status and pending unlocks |
| [bridgeUnlock](#sidechain-bridgeunlock) | Request bridge unlock (burns wrapped tokens) |

#### Faucet

| Method | Description |
|--------|-------------|
| [faucet](#faucet) | Claim test SCCX tokens |

#### DEX Methods

| Method | Description |
|--------|-------------|
| [dex_getOrders](#dex_getorders) | Get open orders for a pair |
| [dex_getTrades](#dex_gettrades) | Get recent trades for a pair |
| [dex_getAllTrades](#dex_getalltrades) | Get recent trades across all pairs |
| [dex_submitOrder](#dex_submitorder) | Submit a new order |
| [dex_cancelOrder](#dex_cancelorder) | Cancel an open order |
| [dex_deposit](#dex_deposit) | Get DEX deposit address |
| [dex_withdraw](#dex_withdraw) | Withdraw from DEX escrow |
| [dex_getEscrowBalance](#sidechain-dex_getescrowbalance) | Get DEX escrow balance |

---

### Real-time Streams (Port 8070 and 8080)

#### SSE Events (conceal-rpc)

| Event | Description |
|-------|-------------|
| [status](#sse-status) | Periodic wallet status broadcast (every 2s) |
| [walletSync](#sse-walletsync) | New outputs detected from chain scan |
| [transaction](#sse-transaction) | Transaction submitted by this wallet |

#### SSE Events (sidechain)

| Event | Description |
|-------|-------------|
| [status](#sse-status-sc) | Sidechain height and token count on connect |

#### WebSocket (sidechain)

| Message Type | Description |
|--------------|-------------|
| [subscribe_orders](#ws-subscribe_orders) | Subscribe to real-time order book for a pair |
| [subscribe_trades](#ws-subscribe_trades) | Subscribe to real-time trade history for a pair |
| [orderBookSnapshot](#ws-orderbooksnapshot) | Order book snapshot pushed to client |
| [tradeHistory](#ws-tradehistory) | Trade history snapshot pushed to client |
| [block](#ws-block) | New sidechain block committed |
| [dexTrade](#ws-dextrade) | New DEX trade executed |
| [bridgeDeposit](#ws-bridgedeposit) | Bridge deposit detected |

---

## Overview

The Conceal unified client provides two JSON-RPC 2.0 endpoints plus real-time event streams:

| Service | Default Port | Purpose |
|---|---|---|
| **Conceal RPC** | 8070 | Unified wallet backend (mainchain CCX + proxy to sidechain/DEX/bridge) |
| **Sidechain RPC** | 8080 | Sidechain validator node (tokens, transfers, DEX, bridge) |

| Stream Type | Port | Protocol | Purpose |
|---|---|---|---|
| **Conceal SSE** | 8070 | Server-Sent Events | Real-time wallet sync and transaction events |
| **Sidechain SSE** | 8080 | Server-Sent Events | Sidechain status on connect |
| **Sidechain WebSocket** | 8080 | WebSocket (RFC 6455) | Real-time DEX order book, trades, blocks, bridge deposits |

### Request Format

All JSON-RPC requests are `POST` to `/json_rpc` with `Content-Type: application/json`.

```json
{
  "jsonrpc": "2.0",
  "method": "methodName",
  "params": { },
  "id": 1
}
```

### Response Format

**Success:**
```json
{
  "jsonrpc": "2.0",
  "result": { },
  "id": 1
}
```

**Error:**
```json
{
  "jsonrpc": "2.0",
  "error": {
    "code": -32603,
    "message": "description"
  },
  "id": 1
}
```

### Standard Error Codes

| Code | Meaning |
|------|---------|
| -32600 | Invalid request |
| -32601 | Method not found |
| -32603 | Internal error (see message) |

### Sidechain Error Codes

| Code | Meaning |
|------|---------|
| -32000 | Insufficient balance |
| -32001 | Insufficient escrow balance |
| -32002 | Rate limited (token creation cooldown) |
| -32003 | Unauthorized mint (not bridge operator) |
| -32004 | Order not found |
| -32005 | Order not owned by sender |
| -32006 | DEX engine not enabled |
| -32007 | Faucet already claimed |
| -32008 | Address has no transaction history |
| -32009 | Deposit not found |
| -32010 | Deposit not matured |
| -32011 | Invalid parameters |
| -32012 | Transaction rejected by validator |

### Atomic Units

All amounts in this API are denominated in **atomic units** (the smallest divisible unit). For CCX, 1 CCX = 1,000,000 atomic units. For sidechain tokens, the number of decimal places is defined per token and can be queried via `getTokens`.

### Retry and Timeout Guidance

During daemon synchronization or validator startup, mutating calls may fail with internal errors. A recommended retry strategy:

- **Query methods** (`getBalance`, `getStatus`, `getTokens`): Retry immediately up to 3 times, then fall back to cached data.
- **Mutating methods** (`transfer`, `dex_submitOrder`, `createDeposit`): Retry with exponential backoff starting at 1 second, up to 5 attempts. If the daemon is syncing, wait for `synced: true` from `getSyncStatus` before retrying.
- **SSE/WebSocket connections**: Reconnect with a 1-5 second delay on disconnect.

SSE streams are accessed via HTTP GET with the `Accept: text/event-stream` header. WebSocket connections use the standard HTTP upgrade handshake with `Upgrade: websocket`.

---

## Security Considerations

The Conceal RPC, sidechain RPC, SSE, and WebSocket endpoints do not require API keys or authentication. They are designed to run on localhost or a trusted network. This section describes the security model and recommended deployment practices.

### Key Material Protection

- **Spend key exposure**: The spend key can sign transactions on behalf of your wallet. Anyone with access to the spend key can drain all funds. Use `lock` after every sending session and run in view-only mode for balance monitoring.
- **State file security**: The wallet state file (`bolt-wallet.state`) contains plaintext view keys and, if the wallet was not locked, the spend key. Restrict filesystem permissions to the user running the daemon only (`chmod 600`).
- **Key export**: The `exportWallet` RPC returns a base64-encoded blob containing all key material. Treat this blob like a private key. Never log it, store it in version control, or transmit it over unencrypted channels.
- **In-memory zeroing**: The `lock` RPC method zeroes the spend key from memory and reconstructs the wallet as view-only. This is a defence-in-depth measure: if the process memory is dumped after locking, the spend key will not be recoverable.

### Network Binding

**Always bind to `127.0.0.1` unless you have a specific, well-understood reason to do otherwise.** The default configuration binds all services to localhost:

| Service | Default Bind Address |
|---------|----------------------|
| Conceal RPC (wallet) | 127.0.0.1 |
| Sidechain RPC | 127.0.0.1 |
| Mainchain P2P | 0.0.0.0 (required for peer discovery) |

If you must expose the RPC endpoints to a network interface, apply compensating controls:

### Reverse Proxy with Authentication

Deploy nginx or Caddy as a TLS-terminating reverse proxy with basic authentication. Example nginx configuration:

```nginx
server {
    listen 443 ssl;
    server_name conceal.example.com;

    ssl_certificate     /etc/ssl/conceal.crt;
    ssl_certificate_key /etc/ssl/conceal.key;

    auth_basic "Conceal RPC";
    auth_basic_user_file /etc/nginx/.htpasswd;

    location /json_rpc {
        proxy_pass http://127.0.0.1:8070;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
    }

    location / {
        proxy_pass http://127.0.0.1:8070;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
    }
}
```

This enables:
- TLS encryption for all RPC and stream traffic
- Basic authentication requiring username/password per connection
- WebSocket upgrade forwarding for real-time DEX streams

### Firewall Rules

If running on a multi-user server or cloud instance, restrict inbound traffic to the RPC ports:

```bash
# Allow localhost only
iptables -A INPUT -p tcp --dport 8070 -s 127.0.0.1 -j ACCEPT
iptables -A INPUT -p tcp --dport 8080 -s 127.0.0.1 -j ACCEPT
iptables -A INPUT -p tcp --dport 8070 -j DROP
iptables -A INPUT -p tcp --dport 8080 -j DROP
```

### Operational Best Practices

| Practice | Rationale |
|----------|-----------|
| Run as a dedicated OS user | Limits filesystem access if the daemon process is compromised |
| Enable disk encryption | Protects the state file at rest |
| Rotate wallets for large holdings | Separate cold storage from the hot wallet used by conceal-rpc |
| Monitor `getSyncStatus` before sending | Submitting transactions while the daemon is syncing may result in rejected transactions or unexpected fees |
| Audit RPC access logs | If using a reverse proxy, log all RPC calls for anomaly detection |

---

## 1. Conceal RPC (Port 8070)

The Conceal RPC server is the unified wallet backend. It handles mainchain CCX operations directly and proxies sidechain, DEX, and bridge calls when connected to a sidechain validator.

### 1.1 System

#### getVersion

Get the Conceal binary version string. Useful for GUI compatibility checks.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `version` | string | Conceal release version string |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getVersion","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "version": "8.1.2"
  },
  "id": 1
}
```

---

### 1.2 Wallet Lifecycle

#### generateWallet

Generate a new random wallet with view key, spend key, address, and mnemonic seed phrase (25 words). The wallet is immediately active and persisted to the state file.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" on success |
| `address` | string | Mainchain CCX address |
| `viewKey` | string | 64-char hex private view key |
| `spendKey` | string | 64-char hex private spend key |
| `mnemonic` | string | 25-word mnemonic seed phrase |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"generateWallet","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ1234567890abcdef...",
    "viewKey": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2",
    "spendKey": "b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3",
    "mnemonic": "witch collapse practice feed shame open despair creek road again ..."
  },
  "id": 1
}
```

**Notes:** The mnemonic is derived from the spend key. Save the mnemonic and keys securely. The wallet is persisted to the state file automatically so keys survive restarts. A rescan begins immediately in the background.

---

#### importWallet

Import an existing wallet by view key and optional spend key. Destroys any existing wallet and replaces it. No restart needed.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `viewKey` | string | Yes | 64-char hex private view key |
| `spendKey` | string | No | 64-char hex private spend key (omit for view-only) |

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" on success |
| `address` | string | Resolved mainchain CCX address |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"importWallet",
    "params":{
      "viewKey":"a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2",
      "spendKey":"b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2c3"
    },
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Invalid view key hex | The `viewKey` parameter is not a valid 64-char hex string |
| Invalid spend key hex | The `spendKey` parameter is not a valid 64-char hex string |

**Notes:** After import, a full chain rescan begins in the background if the data directory is available. The wallet is persisted to state file immediately.

---

#### unlock

Reload the wallet keys from the state file without requiring the user to re-enter hex keys. Restores the wallet to its pre-lock state.

**Parameters:** none

**Response:** Same as `importWallet` (returns `{"status":"ok","address":"..."}`).

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"unlock","params":{},"id":1}'
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| State manager not configured | Internal error: state file path not set |
| No saved keys found. Use importWallet to set keys first. | Wallet was never imported or generated |

**Notes:** Uses `StateManager::loadKeys()` internally. If a spend key was saved, the wallet becomes a full wallet; otherwise view-only.

---

#### lock

Lock the wallet by zeroing out sensitive key material from memory. The RPC server stays alive so you can still call `unlock` later. View-only wallets are locked trivially.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" |
| `message` | string | Status message |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"lock","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "message": "Wallet locked. Use unlock to restore keys."
  },
  "id": 1
}
```

**Notes:** After locking, the wallet is reconstructed as view-only. The spend key is zeroed in memory. The state file is saved with empty keys to respect the locked state across restarts. Calling `lock` when already locked returns `"Already locked"`.

---

#### getViewKey

Return the current wallet view key as a 64-char hex string. Returns empty string if the wallet is locked.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `viewKey` | string | 64-char hex view key or empty string |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getViewKey","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "viewKey": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6e7f8a9b0c1d2e3f4a5b6c7d8e9f0a1b2"
  },
  "id": 1
}
```

---

#### getSpendKey

Return the current wallet spend key as a 64-char hex string. Returns empty string if the wallet is locked or view-only.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `spendKey` | string | 64-char hex spend key or empty string |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getSpendKey","params":{},"id":1}'
```

---

#### getWalletHeight

Get the current wallet sync height and total output count in the tracked set.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `walletHeight` | uint32 | Last scanned block height |
| `outputCount` | uint32 | Number of tracked outputs |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getWalletHeight","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "walletHeight": 1850000,
    "outputCount": 245
  },
  "id": 1
}
```

---

#### exportWallet

Export the wallet keys and metadata as a base64-encoded JSON blob. Useful for backup or migrating to another instance.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `wallet` | string | Base64-encoded JSON containing version, viewKey, spendKey, walletHeight, address |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"exportWallet","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "wallet": "eyJ2ZXJzaW9uIjoxLCJ2aWV3S2V5IjoiY..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Wallet is locked. Unlock first. | Cannot export key material while wallet is locked |

**Notes:** The decoded JSON contains: `{"version":1,"viewKey":"...","spendKey":"...","walletHeight":...,"address":"..."}`. Spend key is empty string for view-only wallets.

---

### 1.3 Mainchain Wallet Methods

#### getBalance

Get the wallet's mainchain CCX balance. All amounts are in atomic units (1 CCX = 1,000,000 atomic units).

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `availableBalance` | uint64 | Spendable CCX (atomic units) |
| `lockedAmount` | uint64 | Pending/spend-locked CCX |
| `lockedDepositBalance` | uint64 | CCX locked in active deposits |
| `unlockedDepositBalance` | uint64 | CCX from matured deposits (ready to spend) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getBalance","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "availableBalance": 500000000000,
    "lockedAmount": 0,
    "lockedDepositBalance": 100000000000,
    "unlockedDepositBalance": 0
  },
  "id": 1
}
```

**Notes:** If the wallet has no outputs, returns all zeros without blocking.

---

#### getAddress

Returns the wallet's mainchain address.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `address` | string | Mainchain CCX address |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getAddress","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ..."
  },
  "id": 1
}
```

---

#### getStatus

General daemon and wallet status. If sidechain is configured, connection details are included.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `blockCount` | uint32 | Daemon's local block height |
| `knownBlockCount` | uint32 | Network height reported by peers |
| `peerCount` | size_t | Number of connected peers |
| `walletHeight` | uint32 | Last scanned block by the wallet |
| `sidechainHost` | string | (optional) Sidechain host |
| `sidechainPort` | uint16 | (optional) Sidechain port |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getStatus","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "blockCount": 2069294,
    "knownBlockCount": 2069294,
    "peerCount": 8,
    "walletHeight": 2068500,
    "sidechainHost": "127.0.0.1",
    "sidechainPort": 8080
  },
  "id": 1
}
```

**Notes:** If the daemon is unreachable, blockCount, knownBlockCount, and peerCount return 0.

---

#### getSyncStatus

Detailed wallet sync progress. Shows how far the wallet scan has reached relative to the daemon.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `walletHeight` | uint32 | Last block scanned by wallet |
| `nodeHeight` | uint32 | Current daemon height |
| `synced` | bool | True when walletHeight >= nodeHeight and nodeHeight > 0 |
| `sidechainHost` | string | (optional) Sidechain host |
| `sidechainPort` | uint16 | (optional) Sidechain port |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getSyncStatus","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "walletHeight": 1850000,
    "nodeHeight": 2069294,
    "synced": false
  },
  "id": 1
}
```

---

#### getNetworkHeight

Get the network block height from the connected daemon.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `networkHeight` | uint32 | Network block height |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getNetworkHeight","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "networkHeight": 2069294
  },
  "id": 1
}
```

---

#### transfer

Send CCX to one or more addresses. Amounts are in atomic units (1 CCX = 1,000,000 atomic units).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `destinations` | array | Yes | Array of `{address, amount}` objects |
| `mixin` | uint64 | No | Ring size (default: network minimum) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"transfer",
    "params":{
      "destinations": [
        {"address":"ccx7aBcDeFg...", "amount":1000000000}
      ],
      "mixin": 5
    },
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Cannot send from view-only wallet | Wallet has no spend key. Unlock first. |
| (wallet engine error) | Build/relay failure; message describes the specific issue |

---

#### getTransactions

List wallet transaction history derived from tracked outputs, with pagination support.

**CryptoNote output model:** Unlike account-based blockchains, CryptoNote wallets track individual outputs (UTXOs) rather than transactions. There is no "sent transaction" record. When you send CCX, one or more of your existing outputs are consumed and new outputs are created for the recipient. The `spent` field on an output indicates whether it has been used as an input in a subsequent transaction. When an output is spent, its key image is recorded on-chain. The wallet detects this during scanning and marks the output `spent: true`. There is no separate "sent transaction" record because the wallet cannot deterministically know which inputs in a ring signature belong to it. For developers building custom transaction scanners, the key image is the canonical identifier for detecting spent outputs.

Items are returned in descending block height order (newest first).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `firstBlockIndex` | uint32 | No | Offset into the sorted output list (default: 0) |
| `blockCount` | uint32 | No | Maximum items to return (default: unlimited, 0 = all) |

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `items` | array | List of output-based transaction entries |
| `items[].hash` | string | Transaction hash (hex) |
| `items[].amount` | uint64 | Amount in atomic units |
| `items[].blockHeight` | uint32 | Block containing the output |
| `items[].spent` | bool | Whether this output has been consumed |
| `items[].isDeposit` | bool | Whether this output is a time-locked deposit |
| `totalItems` | uint32 | Total number of outputs in the wallet |
| `firstBlockIndex` | uint32 | The offset that was requested |
| `blockCount` | uint32 | The limit that was requested (or totalItems if 0) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTransactions","params":{"firstBlockIndex":0,"blockCount":100},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "items": [
      {
        "hash": "a1b2c3d4e5f6...",
        "amount": 1000000000,
        "blockHeight": 2060000,
        "spent": false,
        "isDeposit": false
      }
    ],
    "totalItems": 245,
    "firstBlockIndex": 0,
    "blockCount": 100
  },
  "id": 1
}
```

---

#### createDeposit

Lock CCX in a time-locked deposit that earns interest. Amounts are in atomic units (1 CCX = 1,000,000 atomic units).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `amount` | uint64 | Yes | Amount to lock (atomic units) |
| `term` | uint32 | Yes | Lock duration in blocks |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"createDeposit",
    "params":{"amount":100000000000,"term":100000},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "b2c3d4e5f6a7..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Cannot send from view-only wallet | Wallet has no spend key |
| (wallet engine error) | Build/relay failure; message describes the specific issue |

---

#### withdrawDeposit

Withdraw a matured deposit.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `depositId` | uint64 | Yes | ID of the deposit |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"withdrawDeposit","params":{"depositId":1},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "c3d4e5f6a7b8..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Cannot send from view-only wallet | Wallet has no spend key |
| (wallet engine error) | Deposit not found, not matured, or relay failure |

---

#### getDeposits

List all deposits tracked by the wallet.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `deposits` | array | Array of deposit objects |
| `deposits[].id` | uint64 | Deposit ID |
| `deposits[].amount` | uint64 | Locked amount (atomic units) |
| `deposits[].term` | uint32 | Lock duration (blocks) |
| `deposits[].unlockHeight` | uint32 | Block when deposit matures |
| `deposits[].locked` | bool | Whether still locked |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getDeposits","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "deposits": [
      {
        "id": 1,
        "amount": 100000000000,
        "term": 100000,
        "unlockHeight": 2169294,
        "locked": true
      }
    ]
  },
  "id": 1
}
```

---

#### estimateFusion

Estimate how many outputs are available for fusion (consolidation).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `threshold` | uint64 | No | Minimum amount in atomic units to consider (default: 1,000,000) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"estimateFusion","params":{"threshold":1000000},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "fusionReadyCount": 12,
    "totalOutputCount": 45
  },
  "id": 1
}
```

---

#### sendFusionTransaction

Create and send a fusion transaction to consolidate small outputs.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `threshold` | uint64 | No | Minimum amount in atomic units (default: 1,000,000) |
| `mixin` | uint64 | No | Ring size (default: network minimum) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"sendFusionTransaction",
    "params":{"threshold":1000000,"mixin":5},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "d4e5f6a7b8c9..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Cannot send from view-only wallet | Wallet has no spend key |
| (wallet engine error) | No fusion outputs available or relay failure |

---

#### reset

Mark wallet for a full rescan from genesis on the next sync cycle. Clears all tracked outputs and sets scanned height to 0. Keys are preserved so unlock still works after reset.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `status` | string | "ok" |
| `message` | string | Status description |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"reset","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "message": "Wallet reset. Rescan will begin on next sync cycle."
  },
  "id": 1
}
```

---

#### save

Persist current wallet state to disk.

**Parameters:** none

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{"jsonrpc":"2.0","method":"save","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok"
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Failed to write state file | Disk full, permission denied, or state manager not configured |

**Notes:** Also triggered automatically at a configurable interval (walletAutoSaveInterval).

---

### 1.4 Sidechain Proxy Methods

These methods forward requests to the configured sidechain validator. They require `--sidechain-host` and `--sidechain-port` set on conceal-rpc, or they are auto-configured when the sidechain runs in the same process.

#### getSidechainStatus

Returns sidechain connection status or proxies to the sidechain's `getStatus` if connected.

**Parameters:** none

When connected, the response is the full sidechain status (see sidechain `getStatus`).

When not connected:
```json
{
  "jsonrpc": "2.0",
  "result": { "sidechainEnabled": false },
  "id": 1
}
```

---

#### getSidechainTokens

List all tokens registered on the sidechain. Proxies to sidechain's `getTokens`.

**Parameters:** none

Example response:
```json
{
  "jsonrpc": "2.0",
  "result": [
    {
      "id": 0,
      "fingerprint": "",
      "name": "SCCX",
      "symbol": "SCCX",
      "totalSupply": 1000000,
      "maxSupply": 0,
      "decimals": 6,
      "backingModel": 0,
      "backingRatio": 0,
      "lockedCCXAmount": 0
    }
  ],
  "id": 1
}
```

---

#### getTokenBalance

Get the balance of a specific sidechain token for an address. Proxies to sidechain's `getTokenBalance`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Hex-encoded public key (64 hex chars) |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance in atomic units.

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"getTokenBalance",
    "params":{"address":"a1b2c3d4...","tokenId":0},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": "500000",
  "id": 1
}
```

---

#### sidechainTransfer

Transfer sidechain tokens between addresses. Proxies to sidechain's `transfer`. Amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Sender public key (hex) |
| `to` | string | Yes | Recipient public key (hex) |
| `amount` | uint64 | Yes | Amount to send (atomic units) |
| `tokenId` | uint64 | Yes | Token ID |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"sidechainTransfer",
    "params":{
      "from":"a1b2...",
      "to":"c3d4...",
      "amount":1000,
      "tokenId":1
    },
    "id":1
  }'
```

**Errors:** See sidechain `transfer` for error details.

---

#### sidechainCreateToken

Create a new token on the sidechain. Proxies to sidechain's `createToken`. All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Creator public key (hex) |
| `name` | string | Yes | Token name (up to 16 chars) |
| `symbol` | string | Yes | Token symbol (up to 8 chars) |
| `initialSupply` | uint64 | Yes | Initial minted supply (atomic units) |
| `backingModel` | uint64 | Yes | 0=Unbacked, 1=Backed, 2=Hybrid |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"sidechainCreateToken",
    "params":{
      "from":"a1b2...",
      "name":"MyToken",
      "symbol":"MTK",
      "initialSupply":1000000,
      "backingModel":0
    },
    "id":1
  }'
```

**Errors:** See sidechain `createToken` for error details.

---

### 1.5 DEX Proxy Methods

All DEX methods require a connected sidechain with DEX enabled.

#### dexGetOrderBook

Get open orders for a trading pair. Proxies to `dex_getOrders`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Returns an array of open orders (see sidechain `dex_getOrders`).

---

#### dexPlaceOrder

Submit a new DEX order. Proxies to `dex_submitOrder`. All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `amount` | uint64 | Yes | Order amount (atomic units) |
| `price` | uint64 | Yes | Price in quote token units (atomic units) |
| `side` | string | Yes | "buy" or "sell" |

**Errors:** See sidechain `dex_submitOrder` for error details.

---

#### dexCancelOrder

Cancel an open DEX order. Proxies to `dex_cancelOrder`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `orderId` | uint64 | Yes | Order ID to cancel |
| `owner` | string | Yes | Owner public key (hex) |

**Errors:** See sidechain `dex_cancelOrder` for error details.

---

#### dexGetMyOrders

Get orders owned by an address for a specific pair. Proxies to `dex_getOrders` and filters by owner on the sidechain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

---

#### dexGetTradeHistory

Get recent trades for a trading pair. Proxies to `dex_getTrades`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

Returns recent trades for the pair (see sidechain `dex_getTrades`).

---

#### dexGetEscrowBalance

Get DEX escrow balance for a user. Proxies to `dex_getEscrowBalance`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Returns balance locked in DEX escrow as a plain string (atomic units).

---

### 1.6 Bridge Proxy Methods

#### bridgeGetStatus

Get bridge status including asset registry and pending unlocks. Proxies to sidechain's `getBridgeStatus`.

**Parameters:** none

---

#### bridgeLock

Lock CCX on mainchain to be bridged to the sidechain. This sends a CCX transfer to the bridge address on mainchain. Amounts are in atomic units (1 CCX = 1,000,000 atomic units).

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `amount` | uint64 | Yes | CCX amount to lock (atomic units) |
| `bridgeAddress` | string | Yes | Bridge mainchain address (destination for the CCX transfer) |

```bash
curl -s -X POST http://127.0.0.1:8070/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"bridgeLock",
    "params":{"amount":100000000,"bridgeAddress":"ccxBridgeAddr..."},
    "id":1
  }'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "transactionHash": "e5f6a7b8c9d0...",
    "status": "locked"
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Cannot send from view-only wallet | Wallet has no spend key |
| (wallet engine error) | Insufficient balance or relay failure |

---

#### bridgeUnlock

Request unlocking of CCX from the bridge (burns wrapped tokens on sidechain). Proxies to sidechain's `bridgeUnlock`.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `lockId` | uint64 | Yes | ID of the bridge lock to release |
| `userAddress` | string | Yes | User's sidechain public key (hex) |

**Errors:** See sidechain `bridgeUnlock` for error details.

---

## 2. Sidechain RPC (Port 8080)

The sidechain validator exposes its own JSON-RPC 2.0 endpoint. All methods are directly accessible; they are also proxied by the Conceal RPC when it is connected.

### 2.1 Account Methods

#### sidechain-getBalance

Get native SCCX balance of an address. Balance is returned in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex, 64 chars) |

Returns a plain string with the balance in atomic units.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getBalance","params":{"address":"a1b2c3..."},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "1500000",
  "id": 1
}
```

---

#### sidechain-getTokenBalance

Get token balance of an address. Balance is returned in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance in atomic units.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTokenBalance","params":{"address":"a1b2...","tokenId":1},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "10000",
  "id": 1
}
```

---

### 2.2 Token Methods

#### getTokens

List all registered tokens on the sidechain.

**Parameters:** none

**Response:** Array of token objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Token ID |
| `fingerprint` | string | Asset fingerprint |
| `name` | string | Token name |
| `symbol` | string | Token symbol |
| `totalSupply` | uint64 | Current total supply (atomic units) |
| `maxSupply` | uint64 | Maximum supply (0 = unlimited) |
| `decimals` | uint8 | Decimal places |
| `backingModel` | uint8 | 0=Unbacked, 1=Backed, 2=Hybrid |
| `backingRatio` | uint64 | Backing ratio (if backed) |
| `lockedCCXAmount` | uint64 | CCX locked for backing (atomic units) |
| `sourceChain` | string | Source chain (if bridged asset) |
| `sourceAsset` | string | Source asset (if bridged asset) |
| `bridgeOperator` | string | Bridge operator public key (hex) |
| `equivalenceClass` | string | DEX equivalence group |
| `verified` | bool | Asset verified status |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTokens","params":{},"id":1}'
```

---

#### getTokenByFingerprint

Look up a token by its asset fingerprint.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `fingerprint` | string | Yes | Asset fingerprint |

Returns a token object on success, or an error if no token matches the fingerprint.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTokenByFingerprint","params":{"fingerprint":"abc123..."},"id":1}'
```

---

#### createToken

Create a new token on the sidechain. All supply amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Creator public key (hex) |
| `nameHex` | string | Yes | Token name as hex string (max 16 bytes) |
| `symbolHex` | string | Yes | Token symbol as hex string (max 8 bytes) |
| `initialSupply` | uint64 | Yes | Initial supply to mint (atomic units) |
| `backingModel` | uint64 | Yes | 0=Unbacked, 1=Backed, 2=Hybrid |
| `decimals` | uint8 | No | Decimal places (default 6) |
| `backingRatio` | uint64 | No | For backed models |
| `lockedCCXAmount` | uint64 | No | CCX amount locked for backing (atomic units) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"createToken",
    "params":{
      "from":"a1b2...",
      "nameHex":"4d79546f6b656e",
      "symbolHex":"4d544b",
      "initialSupply":1000000,
      "backingModel":0
    },
    "id":1
  }'
```

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "txHash": "abc123...",
    "symbol": "MTK"
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Token creation rejected by validator | Duplicate symbol, rate limited, or validation failure |

---

### 2.3 Transaction Methods

#### sidechain-transfer

Transfer tokens between sidechain addresses. All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Sender public key (hex) |
| `to` | string | Yes | Recipient public key (hex) |
| `amount` | uint64 | Yes | Amount to send (atomic units) |
| `tokenId` | uint64 | Yes | Token ID |

A fee is automatically added (DEFAULT_FEE or TESTNET_FEE, in token ID 0 = SCCX).

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"transfer",
    "params":{
      "from":"a1b2...",
      "to":"c3d4...",
      "amount":500,
      "tokenId":1
    },
    "id":1
  }'
```

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "txHash": "abc123...",
    "fee": 10
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Insufficient balance | Sender does not have enough tokens (amount + fee) |
| Transaction rejected by validator | Validation failure on the sidechain |

---

#### mintToken

Mint new tokens. Requires authorization from the bridge key (the `from` address must match the bridge operator). All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Bridge public key (must match bridge operator) |
| `to` | string | Yes | Recipient public key |
| `amount` | uint64 | Yes | Amount to mint (atomic units) |
| `tokenId` | uint64 | Yes | Token ID |
| `mainChainTxHash` | string | No | Associated mainchain tx hash |

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "txHash": "abc123...",
    "mintedAmount": 1000,
    "tokenId": 1,
    "recipient": "a1b2c3..."
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Sender is not the bridge operator for this token | The `from` address does not match the registered bridge operator |
| Mint transaction rejected by validator | Validation failure on the sidechain |

---

#### burnToken

Burn tokens (for bridge unlock). All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `from` | string | Yes | Owner public key |
| `amount` | uint64 | Yes | Amount to burn (atomic units) |
| `tokenId` | uint64 | Yes | Token ID |

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "txHash": "abc123...",
    "burnedAmount": 500,
    "tokenId": 1
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Insufficient balance to burn | Sender does not have enough tokens (amount + fee) |
| Burn transaction rejected by validator | Validation failure on the sidechain |

---

#### sidechain-getTransactions

Get transaction history for an address. Scans all sidechain blocks from genesis to current height.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

**Response:** Array of transactions involving the address:
| Field | Type | Description |
|-------|------|-------------|
| `txHash` | string | Transaction hash (hex) |
| `type` | string | "Transfer", "CreateToken", "Mint", or "Burn" |
| `from` | string | Sender public key |
| `to` | string | Recipient public key |
| `amount` | uint64 | Amount (atomic units) |
| `tokenId` | uint64 | Token ID |
| `fee` | uint64 | Fee paid (atomic units) |
| `blockHeight` | uint64 | Block containing the transaction |
| `timestamp` | uint64 | Unix timestamp |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getTransactions","params":{"address":"a1b2..."},"id":1}'
```

---

#### getPendingTransactions

Returns the number of transactions currently in the validator's mempool.

**Parameters:** none

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getPendingTransactions","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "3",
  "id": 1
}
```

---

### 2.4 Status and Validators

#### sidechain-getStatus

Get current sidechain status.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `height` | uint64 | Current sidechain block height |
| `tokenCount` | uint64 | Number of registered tokens |
| `pendingTransactions` | uint64 | Mempool transaction count |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getStatus","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "height": 50000,
    "tokenCount": 12,
    "pendingTransactions": 3
  },
  "id": 1
}
```

---

#### getValidators

List active validators.

**Parameters:** none

**Response:** Array of validator objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint32 | Validator ID |
| `host` | string | Hostname/IP |
| `port` | uint16 | RPC port |
| `stake` | uint64 | Stake amount |
| `active` | bool | Whether currently active |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getValidators","params":{},"id":1}'
```

---

### 2.5 Asset Registry and Bridge

#### getAssetRegistry

List all bridged assets with full provenance information.

**Parameters:** none

**Response:** Array of asset registry entries, each containing:
| Field | Type | Description |
|-------|------|-------------|
| `tokenId` | uint64 | Sidechain token ID |
| `fingerprint` | string | Asset fingerprint |
| `sourceChain` | string | Origin chain |
| `sourceAsset` | string | Origin asset |
| `bridgeOperator` | string | Bridge operator public key (hex) |
| `equivalenceClass` | string | DEX equivalence group |
| `verified` | bool | Verification status |
| `name` | string | Token name |
| `symbol` | string | Token symbol |
| `totalSupply` | uint64 | Current supply (atomic units) |
| `lockedCCXAmount` | uint64 | CCX locked in backing (atomic units) |
| `backingModel` | uint8 | Backing model |
| `backingRatio` | uint64 | Backing ratio |
| `decimals` | uint8 | Decimal places |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getAssetRegistry","params":{},"id":1}'
```

---

#### getEquivalenceGroup

Get all tokens belonging to a DEX equivalence class.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `equivalenceClass` | string | Yes | Equivalence class string (e.g., "conceal:native") |

**Response:** Array of token summary objects:
| Field | Type | Description |
|-------|------|-------------|
| `tokenId` | uint64 | Token ID |
| `fingerprint` | string | Asset fingerprint |
| `symbol` | string | Token symbol |
| `name` | string | Token name |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getEquivalenceGroup","params":{"equivalenceClass":"conceal:native"},"id":1}'
```

---

#### getBridgeStatus

Returns bridge-related information: a list of bridge assets with locked amounts, total pending unlocks, and details of the first 20 pending unlocks.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `bridgeAssets` | array | Bridged asset details |
| `bridgeAssets[].tokenId` | uint64 | Token ID |
| `bridgeAssets[].sourceChain` | string | Origin chain |
| `bridgeAssets[].sourceAsset` | string | Origin asset |
| `bridgeAssets[].bridgeOperator` | string | Bridge operator public key (hex) |
| `bridgeAssets[].totalLocked` | uint64 | Total CCX locked for this asset (atomic units) |
| `pendingUnlocks` | uint64 | Total number of queued CCX unlocks |
| `pendingUnlockDetails` | array | First 20 unlock entries |
| `pendingUnlockDetails[].lockId` | uint64 | Lock ID |
| `pendingUnlockDetails[].userAddress` | string | User public key (hex) |
| `pendingUnlockDetails[].tokenId` | uint64 | Token ID |
| `pendingUnlockDetails[].amount` | uint64 | Unlock amount (atomic units) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"getBridgeStatus","params":{},"id":1}'
```

---

#### sidechain-bridgeUnlock

Request an unlock of bridged CCX. Submits a burn transaction for the locked tokens. When the burn is processed in a block, the bridge lock is marked as unlocked and CCX is queued for release on the mainchain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `lockId` | uint64 | Yes | Bridge lock ID to release |
| `userAddress` | string | Yes | User's sidechain public key (hex) |

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "txHash": "abc123...",
    "lockId": 5,
    "burnedAmount": 100000,
    "tokenId": 2
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Bridge lock not found | The `lockId` does not exist |
| Bridge lock does not belong to this address | The `userAddress` does not match the lock owner |
| Bridge lock already unlocked | The lock has already been processed |
| Bridge unlock transaction rejected by validator | Validation failure on the sidechain |

---

### 2.6 Faucet (Testnet)

#### faucet

Claim test SCCX tokens on testnet. Requires the address to have prior transaction history on the sidechain.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `address` | string | Yes | Public key (hex) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"faucet","params":{"address":"a1b2..."},"id":1}'
```

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "status": "ok",
    "amount": 2,
    "balance": 1500002
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| Faucet already claimed by this address | Each address can only claim once |
| Address has no transaction history | The address must have received a transaction first |

---

### 2.7 DEX Methods

All DEX methods are available directly on the sidechain RPC when the DEX engine is enabled.

#### dex_getOrders

Get open orders for a trading pair.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |

**Response:** Array of order objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Order ID |
| `type` | string | "buy" or "sell" |
| `owner` | string | Owner public key (hex) |
| `amount` | uint64 | Total order amount (atomic units) |
| `price` | uint64 | Price in quote token units (atomic units) |
| `filled` | uint64 | Amount already filled (atomic units) |
| `status` | string | "open", "filled", or "cancelled" |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_getOrders",
    "params":{"baseTokenId":0,"quoteTokenId":1},
    "id":1
  }'
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |

---

#### dex_getTrades

Get recent trades for a pair.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `limit` | uint64 | No | Max trades to return (default 50) |

**Response:** Array of trade objects:
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Trade ID |
| `buyer` | string | Buyer public key (hex) |
| `seller` | string | Seller public key (hex) |
| `amount` | uint64 | Trade amount (atomic units) |
| `price` | uint64 | Execution price (atomic units) |
| `settled` | bool | Whether settlement completed |
| `timestamp` | uint64 | Unix timestamp |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_getTrades",
    "params":{"baseTokenId":0,"quoteTokenId":1,"limit":20},
    "id":1
  }'
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |

---

#### dex_getAllTrades

Get the most recent trades across all pairs.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `limit` | uint64 | No | Max trades (default 100) |

**Response:** Array of trade objects (includes `baseTokenId` and `quoteTokenId` in addition to `dex_getTrades` fields):
| Field | Type | Description |
|-------|------|-------------|
| `id` | uint64 | Trade ID |
| `buyer` | string | Buyer public key (hex) |
| `seller` | string | Seller public key (hex) |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |
| `amount` | uint64 | Trade amount (atomic units) |
| `price` | uint64 | Execution price (atomic units) |
| `settled` | bool | Whether settlement completed |
| `timestamp` | uint64 | Unix timestamp |

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |

---

#### dex_submitOrder

Submit a new order to the DEX order book. The owner must have sufficient escrow balance. All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | "buy" or "sell" |
| `owner` | string | Yes | Owner public key (hex) |
| `baseTokenId` | uint64 | Yes | Base token ID |
| `quoteTokenId` | uint64 | Yes | Quote token ID |
| `amount` | uint64 | Yes | Order amount (atomic units) |
| `price` | uint64 | Yes | Price in quote token units (atomic units) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_submitOrder",
    "params":{
      "type":"buy",
      "owner":"a1b2...",
      "baseTokenId":0,
      "quoteTokenId":1,
      "amount":1000,
      "price":500
    },
    "id":1
  }'
```

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "orderId": 42,
    "type": "buy",
    "baseTokenId": 0,
    "quoteTokenId": 1,
    "amount": 1000,
    "price": 500
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |
| Order amount must be greater than zero | The `amount` parameter is zero |
| Order price must be greater than zero | The `price` parameter is zero |
| Insufficient escrow balance | Owner does not have enough funds in DEX escrow |

> **Atomicity:** The escrow balance check and order placement are performed atomically under a single mutex lock. There is no race condition where two concurrent orders could overdraw the same escrow balance. If `submitOrder` returns success, the funds have been irrevocably debited from escrow.

---

#### dex_cancelOrder

Cancel an open order. Funds are returned to escrow.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `orderId` | uint64 | Yes | Order ID to cancel |
| `owner` | string | Yes | Owner public key (hex) |

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "orderId": 42
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |
| Order not found or not owned by sender | The `orderId` does not exist or the `owner` does not match |

---

#### dex_deposit

Get the DEX deposit address. Users send tokens to this address to fund their escrow balance.

**Parameters:** none

**Response:**
| Field | Type | Description |
|-------|------|-------------|
| `dexAddress` | string | DEX public key (hex) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"dex_deposit","params":{},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": {
    "dexAddress": "f1e2d3c4b5a6..."
  },
  "id": 1
}
```

**Notes:** Users initiate a standard sidechain `transfer` to this address to deposit tokens into DEX escrow. Deposits are detected automatically when the transfer is included in a block.

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |

---

#### dex_withdraw

Withdraw tokens from DEX escrow back to the user's sidechain balance. All amounts are in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token to withdraw |
| `amount` | uint64 | Yes | Amount to withdraw (atomic units) |

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{
    "jsonrpc":"2.0",
    "method":"dex_withdraw",
    "params":{"owner":"a1b2...","tokenId":0,"amount":500},
    "id":1
  }'
```

**Response on success:**
```json
{
  "jsonrpc": "2.0",
  "result": {
    "success": true,
    "owner": "a1b2c3...",
    "tokenId": 0,
    "amount": 500
  },
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |
| Withdrawal amount must be greater than zero | The `amount` parameter is zero |
| Insufficient escrow balance | Owner does not have enough funds in DEX escrow |

---

#### sidechain-dex_getEscrowBalance

Get the DEX escrow balance of a user for a specific token. Balance is in atomic units.

**Parameters:**
| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `owner` | string | Yes | Owner public key (hex) |
| `tokenId` | uint64 | Yes | Token ID |

Returns a plain string with the balance.

```bash
curl -s -X POST http://127.0.0.1:8080/json_rpc \
  -d '{"jsonrpc":"2.0","method":"dex_getEscrowBalance","params":{"owner":"a1b2...","tokenId":0},"id":1}'
```

```json
{
  "jsonrpc": "2.0",
  "result": "2500",
  "id": 1
}
```

**Errors:**

| Error Message | Cause |
|---------------|-------|
| DEX engine not enabled | Sidechain was started without `--enable-dex` |

---

## 3. Real-time Streams

### 3.1 Server-Sent Events (SSE)

SSE streams provide one-way server-to-client events over a persistent HTTP connection. Clients connect via HTTP GET with the `Accept: text/event-stream` header.

#### Connecting to SSE

**Conceal RPC SSE (Port 8070):**
```bash
curl -N -H "Accept: text/event-stream" http://127.0.0.1:8070/
```

**Sidechain SSE (Port 8080):**
```bash
curl -N -H "Accept: text/event-stream" http://127.0.0.1:8080/
```

The server responds with HTTP 200 and the following headers:
```
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive
Access-Control-Allow-Origin: *
```

A keep-alive comment (`: keep-alive`) is sent every 30 seconds to prevent the connection from timing out.

#### SSE Events (conceal-rpc Port 8070)

##### status

Broadcast every 2 seconds with the current wallet status.

**Event type:** `status`

**Data format:**
```json
{
  "mainchainHeight": 2069294,
  "availableBalance": 500000000000,
  "lockedBalance": 0,
  "address": "ccx7aBcDeFgHiJkLmNoPqRsTuVwXyZ..."
}
```

| Field | Type | Description |
|-------|------|-------------|
| `mainchainHeight` | uint32 | Current wallet scan height |
| `availableBalance` | uint64 | Spendable CCX (atomic units) |
| `lockedBalance` | uint64 | Pending/locked CCX (atomic units) |
| `address` | string | Mainchain wallet address |

---

##### walletSync

Broadcast when new outputs are detected from an incremental chain scan.

**Event type:** `walletSync`

**Data format:**
```json
{
  "newHeight": 1850050,
  "newOutputs": 3
}
```

| Field | Type | Description |
|-------|------|-------------|
| `newHeight` | uint32 | Block height reached by the scan |
| `newOutputs` | uint32 | Number of new outputs found |

---

##### transaction

Broadcast when the wallet submits a new transaction to the mainchain.

**Event type:** `transaction`

**Data format:**
```json
{
  "txHash": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6..."
}
```

| Field | Type | Description |
|-------|------|-------------|
| `txHash` | string | Transaction hash (hex) |

---

#### SSE Events (sidechain Port 8080)

##### status

Sent immediately when a client connects to provide a status snapshot.

**Event type:** `status`

**Data format:**
```json
{
  "height": 50000,
  "tokens": 12
}
```

| Field | Type | Description |
|-------|------|-------------|
| `height` | uint64 | Current sidechain block height |
| `tokens` | uint64 | Number of registered tokens |

---

### 3.2 WebSocket (Sidechain Port 8080)

The sidechain RPC server supports WebSocket upgrades for bidirectional real-time communication, primarily for DEX order book and trade streaming.

#### Connecting

Connect via standard WebSocket upgrade handshake:
```
GET / HTTP/1.1
Host: 127.0.0.1:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Key: <client-key>
```

The server responds with HTTP 101 Switching Protocols and the connection remains open for bidirectional framed messages.

All messages are JSON text frames (opcode 0x1). The server also handles ping/pong (opcodes 0x9/0xA) for keep-alive.

---

#### Client-to-Server Messages

##### subscribe_orders

Request a snapshot of the current order book for a trading pair. After connecting, the client will also receive real-time updates via the `block`, `dexTrade`, and `bridgeDeposit` server push events (these are broadcast to all connected WebSocket clients).

**Message format:**
```json
{
  "type": "subscribe_orders",
  "baseTokenId": 0,
  "quoteTokenId": 1
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Must be "subscribe_orders" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |

---

##### subscribe_trades

Request a snapshot of recent trade history for a trading pair.

**Message format:**
```json
{
  "type": "subscribe_trades",
  "baseTokenId": 0,
  "quoteTokenId": 1
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | Must be "subscribe_trades" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |

---

#### Server-to-Client Push Events

These events are broadcast to all connected WebSocket clients when the corresponding on-chain event occurs.

##### orderBookSnapshot

Sent in response to a `subscribe_orders` message. Contains the full current order book for the requested pair.

**Message format:**
```json
{
  "type": "orderBookSnapshot",
  "baseTokenId": 0,
  "quoteTokenId": 1,
  "orders": [
    {
      "id": 1,
      "type": "buy",
      "amount": 1000,
      "price": 500,
      "filled": 200
    }
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "orderBookSnapshot" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |
| `orders` | array | Array of open orders |
| `orders[].id` | uint64 | Order ID |
| `orders[].type` | string | "buy" or "sell" |
| `orders[].amount` | uint64 | Total order amount (atomic units) |
| `orders[].price` | uint64 | Price in quote token units (atomic units) |
| `orders[].filled` | uint64 | Amount already filled (atomic units) |

---

##### tradeHistory

Sent in response to a `subscribe_trades` message. Contains the 20 most recent trades for the requested pair.

**Message format:**
```json
{
  "type": "tradeHistory",
  "baseTokenId": 0,
  "quoteTokenId": 1,
  "trades": [
    {
      "id": 1,
      "amount": 500,
      "price": 500,
      "timestamp": 1715472000
    }
  ]
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "tradeHistory" |
| `baseTokenId` | uint64 | Base token ID |
| `quoteTokenId` | uint64 | Quote token ID |
| `trades` | array | Array of recent trades |
| `trades[].id` | uint64 | Trade ID |
| `trades[].amount` | uint64 | Trade amount (atomic units) |
| `trades[].price` | uint64 | Execution price (atomic units) |
| `trades[].timestamp` | uint64 | Unix timestamp |

---

##### block

Broadcast to all connected WebSocket clients when a new sidechain block is committed.

**Message format:**
```json
{
  "type": "block",
  "data": {
    "height": 50001,
    "txCount": 5,
    "votes": 3
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "block" |
| `data.height` | uint64 | New block height |
| `data.txCount` | uint64 | Number of transactions in the block |
| `data.votes` | uint64 | Number of validator votes for this block |

---

##### dexTrade

Broadcast to all connected WebSocket clients when a new DEX trade is executed.

**Message format:**
```json
{
  "type": "dexTrade",
  "data": {
    "id": 42,
    "buyer": "a1b2c3d4e5f6...",
    "seller": "f6e5d4c3b2a1...",
    "baseTokenId": 0,
    "quoteTokenId": 1,
    "amount": 500,
    "price": 500,
    "timestamp": 1715472000
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "dexTrade" |
| `data.id` | uint64 | Trade ID |
| `data.buyer` | string | Buyer public key (hex) |
| `data.seller` | string | Seller public key (hex) |
| `data.baseTokenId` | uint64 | Base token ID |
| `data.quoteTokenId` | uint64 | Quote token ID |
| `data.amount` | uint64 | Trade amount (atomic units) |
| `data.price` | uint64 | Execution price (atomic units) |
| `data.timestamp` | uint64 | Unix timestamp |

---

##### bridgeDeposit

Broadcast to all connected WebSocket clients when a bridge deposit is detected.

**Message format:**
```json
{
  "type": "bridgeDeposit",
  "data": {
    "amount": 100000000,
    "destination": "a1b2c3d4e5f6...",
    "txHash": "e5f6a7b8c9d0..."
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `type` | string | "bridgeDeposit" |
| `data.amount` | uint64 | CCX amount deposited (atomic units) |
| `data.destination` | string | Destination public key (hex) |
| `data.txHash` | string | Mainchain transaction hash (hex) |

---

## Service Architecture

The Conceal unified client runs three subsystems that may be selectively enabled:

| Subsystem | CLI Flag | Description |
|-----------|----------|-------------|
| Mainchain (daemon) | `--run-mainchain` | Embedded conceald node for blockchain and P2P |
| Sidechain | `--run-sidechain` | Validator node with tokens, DEX, bridge, gossip |
| Wallet (conceal-rpc) | `--run-wallet` | BoltRPC server with wallet, SSE, sidechain proxy |

When the sidechain and wallet run in the same process, the wallet automatically connects to the sidechain at `127.0.0.1:<sidechain-port>`, enabling all proxy methods without manual configuration.

### Default Ports

| Service | Port |
|---------|------|
| Mainchain P2P | 16000 |
| Mainchain RPC (conceald) | 16001 |
| Mainchain SSE | 16101 (RPC port + 100) |
| Sidechain RPC | 8080 |
| Sidechain Gossip | 9080 (RPC port + GOSSIP_PORT_OFFSET) |
| Wallet RPC (conceal-rpc) | 8070 |
