## What an external app needs to do with `conceal-rpc`

Everything goes through a single endpoint: `http://127.0.0.1:8070/json_rpc`

### Starting the wallet

* Spawn `conceal-rpc` with no keys required
* It starts in setup mode and waits for commands
* One shell command, zero configuration

### Wallet lifecycle

* **Create a new wallet:** POST `generateWallet` with no params. Returns the address, view key, and spend key. Display these to the user so they can save them.
* **Import an existing wallet:** POST `importWallet` with `viewKey` and optional `spendKey`. The wallet is ready immediately.
* **Wallet state persists automatically:** saved on generate, import, and shutdown. The GUI does not need to manage files.
* **Reset and rescan:** POST `reset`. Clears all outputs and rescans the blockchain from block 0. Useful if the wallet gets out of sync.

### Checking balances and info

* **Get mainchain CCX balance:** POST `getBalance`. Returns available balance, locked amounts, and deposit balances all in one response.
* **Get wallet address:** POST `getAddress`. Returns the CCX address string.
* **Get sync status:** POST `getStatus`. Returns daemon height, network height, peer count, and wallet scan height.
* **Get detailed sync progress:** POST `getSyncStatus`. Returns how far the wallet has scanned relative to the daemon.

### Sending and receiving

* **Send CCX:** POST `transfer` with a destinations array of address and amount pairs and optional mixin. Returns the transaction hash.
* **Get transaction history:** POST `getTransactions`. Returns all tracked outputs with amounts, block heights, and spent status.
* **Save wallet state manually:** POST `save`. Forces an immediate write of wallet state to disk.

### Time locked deposits

* **Create a deposit:** POST `createDeposit` with amount and term in blocks. Returns the transaction hash.
* **Withdraw a matured deposit:** POST `withdrawDeposit` with the deposit ID. Returns the transaction hash.
* **List all deposits:** POST `getDeposits`. Returns every deposit with its ID, amount, term, unlock height, and locked status.

### Output consolidation (fusion)

* **Estimate fusion:** POST `estimateFusion` with optional threshold. Returns how many outputs are ready to consolidate.
* **Run fusion:** POST `sendFusionTransaction` with optional threshold and mixin. Merges small outputs to reduce wallet size.

### Sidechain features (all proxied through port 8070)

* **Check sidechain connection:** POST `getSidechainStatus`. Returns whether the sidechain is connected.
* **List all sidechain tokens:** POST `getSidechainTokens`. Returns every registered token with name, symbol, supply, and decimals.
* **Get token balance:** POST `getTokenBalance` with address and token ID. Returns the balance for that specific token.
* **Transfer sidechain tokens:** POST `sidechainTransfer` with sender, recipient, amount, and token ID.
* **Create a new token:** POST `sidechainCreateToken` with name, symbol, initial supply, and backing model.

### DEX trading (all proxied through port 8070)

* **View order book:** POST `dexGetOrderBook` with base and quote token IDs. Returns all open buy and sell orders.
* **Place an order:** POST `dexPlaceOrder` with owner, token pair, amount, price, and side as buy or sell.
* **Cancel an order:** POST `dexCancelOrder` with order ID and owner.
* **View my orders:** POST `dexGetMyOrders` with owner and token pair.
* **View trade history:** POST `dexGetTradeHistory` with token pair.
* **Check escrow balance:** POST `dexGetEscrowBalance` with owner and token ID.

### Bridge (all proxied through port 8070)

* **View bridge status:** POST `bridgeGetStatus`. Returns all bridged assets and pending unlocks.
* **Lock CCX for bridging:** POST `bridgeLock` with amount and bridge address. Moves CCX from mainchain to sidechain.
* **Unlock CCX from bridge:** POST `bridgeUnlock` with lock ID and user address. Burns wrapped tokens and releases CCX.

### Summary

The entire wallet is controlled through JSON-RPC calls to a single port. The GUI never touches blockchain files, never derives keys, and
never manages state. `conceal-rpc` handles all of that internally. Every feature is one HTTP request and one JSON response.
