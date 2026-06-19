# conceal-wallet

Terminal UI wallet for the Conceal mainchain. Supports local MDBX blockchain
scanning, remote daemon RPC, or a combination of both.

## Synopsis

```
conceal-wallet [options]
conceal-wallet <view_key> [<spend_key>] [options]
```

### Options

| Flag | Description |
|------|-------------|
| `--view-key HEX` | Private view key (64 hex characters) |
| `--spend-key HEX` | Private spend key (64 hex characters) |
| `--data-dir PATH` | Path to conceald data directory (contains `mdbx_blocks`) |
| `--daemon HOST:PORT` | Daemon JSON-RPC endpoint (default `127.0.0.1:16000`) |
| `--start-height N` | Fresh rescan from block N (skips any existing state file) |
| `--state FILE` | Resume from a saved wallet state file |
| `--offline` | MDBX only; no daemon RPC |
| `-h`, `--help` | Show usage |

### State file

The wallet persists outputs, balance, and the last scanned height to a binary
state file (`wallet_state.bin`). On restart, loading this file is instant; no
rescan is needed. The file location depends on the flags used (see scenarios
below).

The state file is written on clean exit. If the process crashes, only the
blocks scanned since the last save are lost; resuming picks up automatically.

### Sync log

All sync activity is logged to `/tmp/conceal-wallet-sync.log`.

---

## How sync works

The wallet picks one of three sync strategies at startup:

| Strategy | When | Data source |
|----------|------|-------------|
| **DirectScan** | `mdbx_blocks` found in `--data-dir` or default data dir | Local MDBX file, then daemon RPC for continuity |
| **Polling** | No local MDBX, daemon reachable | Daemon JSON-RPC (`get_filter_records` or `getTransactionsAtHeight`) |
| **Offline** | `--offline` flag | MDBX only (if present), otherwise no sync |

When both a local MDBX and a daemon are available, the wallet scans MDBX first
(fast, local disk), then the daemon ensures continuity to the chain tip. The
MDBX database is typically one block behind the live chain because `conceald`
writes blocks asynchronously.

For remote daemons that do not implement `get_filter_records` (older versions),
the wallet automatically falls back to `getTransactionsAtHeight`. This is
logged once at startup so the operator knows which path is active.

---

## Usage scenarios

### Scenario 1 — First-time import with local MDBX

```
conceal-wallet <view> <spend> \
  --data-dir ~/.conceal-mdbx \
  --start-height 1900000
```

**What happens:**

1. No state file is loaded (`--start-height` forces a fresh rescan).
2. MDBX is scanned from block 1900000 to the top of the local database.
3. Continuity of synchronization is assured by the local daemon at
   `127.0.0.1:16000`, keeping the wallet at the chain tip.
4. On exit, `wallet_state.bin` is saved inside `--data-dir`:
   `~/.conceal-mdbx/wallet_state.bin`.

If `--start-height` is omitted and no state file exists, the scan starts from
block 0.

---

### Scenario 2 — Resume with local MDBX and a different state path

```
conceal-wallet <view> <spend> \
  --data-dir ~/.conceal-mdbx \
  --state ~/.conceal/wallet_state.bin
```

**What happens:**

1. State is loaded from `~/.conceal/wallet_state.bin`.
2. MDBX is scanned from the last known height (stored in the state file) to
   the top of `~/.conceal-mdbx/mdbx_blocks`.
3. Continuity of synchronization is assured by the local daemon at
   `127.0.0.1:16000`.
4. On exit, state is saved back to `~/.conceal/wallet_state.bin`.

Use this when the state file and MDBX database live in different directories.

---

### Scenario 3 — Resume with auto-detected MDBX

```
conceal-wallet <view> <spend> \
  --state ~/.conceal/wallet_state.bin
```

**What happens:**

1. State is loaded from the explicit `--state` path.
2. The data directory defaults to `~/.conceal`.
3. If `~/.conceal/mdbx_blocks` exists: DirectScan from state height to MDBX
   top, then the local daemon takes over to stay at the chain tip.
4. If no MDBX is found: Polling via `127.0.0.1:16000` (or the daemon given
   by `--daemon`).
5. On exit, state is saved to `~/.conceal/wallet_state.bin`.

This is the simplest restart command when everything lives in the default
directory.

---

### Scenario 4 — Offline (MDBX only, no RPC)

```
conceal-wallet <view> <spend> \
  --data-dir ~/.conceal-mdbx \
  --start-height 1900000 \
  --offline
```

**What happens:**

1. No daemon connection is made.
2. MDBX is scanned from block 1900000 to the top of the local database.
3. No daemon connection. The wallet is synchronized only as far as the local
   MDBX data goes. If `conceald` is not running, the MDBX is frozen and the
   wallet reflects the chain state at that point.
4. On exit, state is saved to `~/.conceal-mdbx/wallet_state.bin`.

Use this on air-gapped machines or when the daemon is not available.

---

### Scenario 5 — Remote daemon (no local MDBX)

```
conceal-wallet <view> <spend> \
  --state ~/.conceal/wallet_state.bin \
  --daemon 66.203.178.176:16000
```

**What happens:**

1. State is loaded from `~/.conceal/wallet_state.bin`.
2. The default data dir (`~/.conceal`) is checked for `mdbx_blocks`.
   - If found: DirectScan from state height to MDBX top, then the daemon takes
     over to stay at the chain tip.
   - If not found: Polling strategy via `66.203.178.176:16000`.
3. For old daemons that lack `get_filter_records`, the wallet falls back to
   `getTransactionsAtHeight` and logs a one-time notice.
4. On exit, state is saved to `~/.conceal/wallet_state.bin`.

This enables a workflow where a user does an initial fast sync with MDBX,
copies the state file to another machine, and continues syncing via a remote
node.

---

## Quick reference: flag interactions

| Flags | MDBX | Daemon RPC | State file |
|-------|------|------------|------------|
| `--data-dir` alone | scanned | `127.0.0.1:16000` | saved in `--data-dir` |
| `--data-dir` `--start-height N` | scanned from N | `127.0.0.1:16000` | saved in `--data-dir` (fresh) |
| `--data-dir` `--state FILE` | scanned from state height | `127.0.0.1:16000` | saved to FILE |
| `--data-dir` `--offline` | scanned | none | saved in `--data-dir` |
| `--state FILE` alone | auto-detect `~/.conceal` | `127.0.0.1:16000` | saved to FILE |
| `--state FILE` `--daemon HOST:PORT` | auto-detect `~/.conceal` | HOST:PORT | saved to FILE |
| `--daemon HOST:PORT` alone | auto-detect `~/.conceal` | HOST:PORT | saved in default dir |
