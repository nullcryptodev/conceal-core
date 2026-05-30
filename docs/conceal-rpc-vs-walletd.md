## How `conceal-rpc` improves on `walletd`

### Chain scanning speed

`walletd`
* Scanned blocks one at a time by requesting them from the daemon over RPC.
* Single threaded. Each block required a network round trip.
* A full 2 million block scan took hours.

`conceal-rpc`
* Reads the MDBX blockchain database directly from disk with no RPC overhead.
* Multi threaded scanning with a configurable thread pool. Defaults to auto (uses all available cores, capped at 16).
* A full scan completes in minutes instead of hours.

### Restart behaviour

`walletd`
* Held all wallet state in memory. Nothing was persisted to disk.
* Every restart triggered a complete chain rescan from block 0.
* If the daemon was busy or slow, the wallet was unusable until the rescan finished.

`conceal-rpc`
* Persists wallet state to a binary file after the initial scan and after every incremental sync.
* On restart, loading the state file is instant. No rescan needed.
* A background sync monitor keeps the wallet up to date as new blocks arrive, without blocking the RPC server.
* The service/user can also call the `save` RPC method to force a state write at any time.

### Memory usage

`walletd`
* Stored every wallet output in RAM. Memory grew linearly as the chain got longer.
* On machines with limited RAM, this could make running a wallet impractical.

`conceal-rpc`
* Wallet outputs are held in memory only briefly during scanning. The state file on disk is the source of truth.
* Memory usage stays low and flat regardless of how many blocks have been scanned.

### Crash recovery

`walletd`
* A crash during scanning corrupted the in memory cache.
* Recovery meant starting the entire chain scan over from block 0.

`conceal-rpc`
* Wallet state is saved incrementally. A crash only loses the blocks scanned since the last save.
* The state file is written atomically, so it is never left in a corrupted state.
* Resuming is automatic on the next launch.

### Key management

`walletd`
* Required both view key and spend key at startup. The wallet would not start without them.
* No way to change keys without restarting the entire process.

`conceal-rpc`
* Keys are optional at startup. The server starts in setup mode and waits.
* The `generateWallet` RPC creates a brand new wallet with random keys and returns them to the caller.
* The `importWallet` RPC loads an existing wallet at runtime without restarting the process.
* A view only wallet can be created by providing only the view key.

### Transactions and deposits

`conceal-rpc`
* Full deposit support: create, withdraw, and list all time locked deposits with details per deposit.
* Fusion transactions for consolidating small outputs into fewer larger ones, reducing wallet bloat.
* `estimateFusion` lets the GUI show how many outputs are ready to consolidate before committing.
* Transaction history returns every tracked output directly from the wallet engine.

### Sidechain, DEX, and bridge

`conceal-rpc`
* Connects to a sidechain validator and proxies all sidechain RPC methods through the same port.
* Token operations: list all tokens, check balances, transfer tokens, and create new tokens.
* Full DEX access: view order books, place and cancel orders, check trade history, and manage escrow balances.
* Bridge operations: view bridge status, lock CCX for bridging, and request unlocks.
* Every sidechain, DEX, and bridge feature is available through the same single HTTP endpoint on port 8070.

### Architecture and dependencies

`conceal-rpc`
* A standalone process built on three focused libraries: `BoltSync` for scanning, `BoltCore` for transaction logic, and `NodeRpcProxy` for daemon communication.
* No P2P code. No block processing. No consensus logic.
* A smaller binary with a much smaller attack surface.

### External integration

`conceal-rpc`
* Single JSON RPC 2.0 endpoint at `/json_rpc` on port 8070.
* Every feature, mainchain, sidechain, DEX, bridge, is one HTTP POST and one JSON response.
* Any HTTP client in any language can integrate with it. No special libraries needed.
* The server can run in setup mode with no keys, letting the GUI drive the entire wallet lifecycle through RPC calls alone.

---

## What the user experience is like now

A user visits the webwallet. The server starts `conceal-rpc` in the background. It is ready to accept RPC calls within seconds. The user is not blocked by sync progress.

The first screen offers two buttons: Create Wallet or Import Wallet.

If they click Create Wallet, the frontend sends one POST request to `generateWallet`. The server generates cryptographically random keys, creates the wallet, saves it to disk, and returns the address, view key, and spend key. The user sees their keys displayed once and is told to save them. That is it. Wallet created.

If they already have keys, they paste them into the Import Wallet form. One POST to `importWallet` and their wallet is loaded. If they only have a view key, they get a view only wallet for balance checking.

While the wallet is scanning in the background for past transactions, the user can already see their address, share it to receive funds, and monitor the scan progress. The scan does not block any wallet features.

If they close the browser and come back tomorrow, the wallet state file loads instantly. No rescan. The background sync monitor picks up any new blocks from where it left off.

If they want to use sidechain tokens, they click the Tokens tab. The webwallet sends a request to the same port 8070 and gets back the token list, balances, and transfer options. The DEX tab shows live order books and lets them trade. The bridge tab lets them move assets between chains. All through the same single endpoint.

If something goes wrong and the wallet gets out of sync, they click a Reset button. The server clears the state, rescans from block 0 in minutes with multi threaded scanning, and the wallet is correct again.

---

## The practical difference hosting it

Before, hosting a webwallet meant running a full daemon, managing wallet files per user, dealing with crash recovery, and accepting that first time users would stare at a progress bar for hours. Support requests were constant: why is it slow, why did it crash, why do I have to start over.

Now, hosting a webwallet means running three small binaries (daemon, sidechain, conceal-rpc) and writing a frontend that makes HTTP requests to a single port. First time users are using their wallet within seconds of arriving. Returning users are in instantly. The only thing the hosting team needs to manage is keeping the blockchain synced on the server, which happens independently of any user's wallet state.

## About threads

The `--rpc-threads` flag only affects the HTTP server inside `conceal-rpc`. It controls how many worker threads handle incoming `JSON-RPC` requests from the GUI.

It does not:
- Increase the number of connections to the daemon. There is only one connection to `conceald`, regardless of RPC threads.
- Increase the number of connections to the sidechain. There is only one connection to `conceal-side`.
- Make `BoltSync` scan faster; that is controlled by `--threads` (scan threads), which is a separate flag.
- Strain any remote node. The daemon connection is the same lightweight RPC client whether you have 1 or 16 RPC threads.

For a single user desktop wallet, 1 RPC thread is plenty. More threads would only be useful if you were serving many simultaneous users from a single `conceal-rpc` instance, like a web wallet backend handling hundreds of concurrent requests.

