Written as of: https://github.com/nullcryptodev/conceal-core/pull/1/commits/030371e4ef3e6845b91fbd37a28083274b4a14e2

## What We Have Right Now

We have rebuilt the entire wallet backend. `BoltSync` handles multi-threaded blockchain scanning directly from MDBX.
`BoltCore` handles transaction building, deposits, fusion, and sub-addresses. `BoltRPC` replaces `walletd` entirely
with instant state loading and background sync. The main chain daemon now runs on MDBX with header snapshots,
auto-generated checkpoints, and sub-20-minute bootstrap sync. Anyone can run a full Conceal node on modest hardware
and be synced in minutes instead of days.

On top of this we have built a sidechain with its own BFT validator network. It currently supports token creation (hybrid
and fully backed soon), token transfers with balance validation, double-spend protection, and a native gas token called
SCCX (we can change the name and all its values). SCCX has no premine. It is minted block by block as validators earn
rewards. Token creation is free. Transfers cost SCCX. There is a rate limiter to prevent spam, a minimum block interval,
and a faucet so testnet users can get SCCX without running a validator, enough for 2 transactions. The wallet shows balances,
transaction history, and everything works currently through a terminal menu. Validators can be run on separate machines and
the network shuts down gracefully.

We're also building a DEX running as a separate process. It has an order book, a matching engine, and settlement logic. Users
deposit tokens to the DEX address, place buy and sell orders, and the DEX matches them. It can charge a configurable trading fee.
The new client wallet has DEX menus built in. Everything is functional.

## The Bridge Between Chains

The sidechain is not an island. SCCX will be pegged 1:1 with CCX on the main chain. One CCX locked on the main chain equals one SCCX
on the sidechain. The bridge watcher we have already scaffolded watches the main chain for CCX deposits to a known bridge address.
When it detects a deposit, it mints the equivalent SCCX on the sidechain. When someone burns SCCX on the sidechain, the bridge unlocks
the CCX back on the main chain. This creates a trustless, verifiable link between the two chains.

This matters because it connects the ecosystems. Miners on the main chain earn CCX through proof-of-work. They can bridge that CCX to the
sidechain and use it for trading or providing liquidity on the DEX. Validators on the sidechain earn SCCX from fees and block rewards.
They can bridge it back to the main chain for long-term holding or staking. Value flows in both directions. Every part of the network
benefits from the activity of every other part.

For hybrid and backed tokens, the bridge is the foundation. A stablecoin on the sidechain would be backed by locked CCX on the main chain.
The backing is verifiable on-chain. The supply cannot exceed the locked collateral. This gives sidechain tokens real backing without relying
on third-party custodians or external oracles for the core peg.

## Where We Are Going

The DEX is currently a separate binary that polls the sidechain every few seconds for new deposits. The next step is to merge the DEX directly
into the validator node. The DEX engine becomes part of the validator process. When a new block is committed, the DEX scans it instantly for
deposits. When orders match, settlement transactions are created and submitted without ever leaving the validator. The DEX RPC server stays
available so wallets and frontends can query the order book and trade history. Everything is optional behind a `--dex-key` flag. If you do not
provide a DEX key, the validator runs without the DEX and uses zero extra resources.

Beyond that, the bridge watcher will be merged into the validator and enabled by default. It is core infrastructure. Without it there is no
connection between CCX and SCCX, no 1:1 peg, and no way for value to move between the main chain and the sidechain. The bridge is what makes
them one network. The same binary that secures the sidechain will also operate the bridge, keeping both chains in sync without requiring separate
services.

## How This Benefits Everyone

**Main chain miners:** Their CCX has a new use case. They can bridge it to the sidechain, trade on the DEX, provide liquidity, or back new tokens.
Mining revenue gains additional utility beyond selling on an external exchange or bring locked into a fixed APR on stakes.

**Sidechain validators:** Every trade on the DEX creates settlement transactions that pay SCCX fees. These fees go to the validator who proposes
the block. Even validators who do not run the DEX earn from the trading activity. More trading means more transactions, more fees, and more SCCX
flowing to every validator on the network. If you do run the DEX, you also earn the trading fee on every matched order.

**Traders:** Orders are matched instantly because the DEX lives inside the validator. There is no delay waiting for a separate service to notice
a new block. Deposits are credited the moment the block is committed. They can move between CCX and SCCX freely through the bridge.

**The network:** Conceal becomes a self-contained financial system. The main chain provides privacy, security, and proof-of-work settlement. The
sidechain provides speed, tokens, and a native exchange. The bridge connects them so value flows freely. One ecosystem. Three layers. No external
dependencies. No third-party contracts. Everything happens on Conceal infrastructure with Conceal's privacy and security model underneath it all.

---

## Block Times and Per-Transaction Processing

Block times on the sidechain are already fast. The validator processes blocks with a minimum interval of 500ms and commits as soon as the BFT threshold
is met. With threshold set to 1 for testing, blocks are near-instant.

But the real optimisation is moving from per-block thinking to per-transaction thinking. Right now the DEX credits deposits after a block is committed.
That is fast. But it can be faster. The validator sees a transaction the moment it is submitted to the mempool. It validates it immediately. If we fire
an event at validation time instead of commit time, the DEX can credit the escrow balance instantly. The user sees their deposit before the block is even
proposed. The balance updates in real time.

If the block later fails consensus or the transaction turns out to be invalid, the optimistic balance update is reversed. This is called optimistic execution.
You assume success and roll back if needed. Because our validators are a known set with reputation, the chance of a bad transaction making it through validation
is nearly zero. The optimistic path is correct almost all the time. The user experience is instant. The rare failure is handled silently in the background.

This is how centralised exchanges work. They credit your account the moment your transaction hits their mempool. We can do the same thing natively on a
decentralised validator network that also has a privacy-preserving main chain underneath it. No public mempool means no front-running. BFT consensus means
near-instant finality. The Cryptonote privacy model protects user identities on the main chain. The architecture supports all of it.

---

## The Big Picture

Conceal is not a single-purpose chain. It is a platform. The main chain provides proof-of-work security, privacy through ring signatures and stealth addresses,
and a battle-tested Cryptonote foundation. The sidechain was not built as a separate project bolted on afterwards. It was built directly inside the Conceal
codebase, sharing the same MDBX storage engine, the same cryptographic primitives, the same serialization layer, and the same C++ performance. That is why it
can do what it does. Token creation, fast transfers, and a native exchange all running on the same binary that secures the network. The bridge connects the two
chains with a verifiable 1:1 peg. A validator can run the DEX and the bridge alongside consensus with no external dependencies. A miner can bridge their earnings
and trade on the DEX. A user can hold CCX for privacy and SCCX for utility and move between them freely. One codebase, one binary, one ecosystem. No compromises.
That is the vision.
