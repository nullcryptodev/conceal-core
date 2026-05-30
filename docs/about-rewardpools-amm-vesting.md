## Vesting Schedules

Vesting locks tokens on a time based release schedule. A creator deposits tokens that unlock gradually for a beneficiary. Useful for team allocations, investor tokens, or any situation where you want to prove tokens are locked long term.

**Create a vesting schedule:**
```
Transaction type: CreateVesting
Token: the token being vested
From: the creator who owns the tokens
Extra data: beneficiary_public_key:total_amount:cliff_timestamp:vesting_end_timestamp:revocable
Example: ccx7abc...:1000000:1717200000:1748736000:1
```
This locks 1,000,000 tokens. Nothing is released until the cliff timestamp of June 1 2024. Between the cliff and the vesting end timestamp of June 1 2025 the tokens release linearly. The 1 at the end means the creator can revoke unvested tokens. The creator must have the tokens in their balance. Timestamps are Unix timestamps in seconds.

**Revoke a vesting schedule:**
```
Transaction type: RevokeVesting
Extra data: schedule_id
```
Only the creator can revoke. Only if revocable was set to true. Unvested tokens return to the creator. Already vested tokens stay with the beneficiary.

**How it works automatically:**
Every block, the validator runs `processVestingReleases`. It checks every active vesting schedule. If the cliff timestamp has passed, it calculates how many tokens should be released based on wall clock time elapsed and transfers them from the creator to the beneficiary. No manual claiming needed.

**Query vesting schedules:**
The storage layer has `getActiveVestingSchedules()` and `getVestingSchedulesByBeneficiary()`. The RPC endpoints for these are not added yet but the storage methods are ready.

---

## Reward Pools

Reward pools let a creator deposit tokens that get distributed as rewards to stakers. Think of it like a staking farm. The creator funds the
pool upfront with a fixed reward amount and an annual rate. Users stake tokens into the pool and earn rewards proportional to their share.

**Create a reward pool:**
```
Transaction type: CreateRewardPool
Token: the token being rewarded
Amount: the initial reward deposit
Extra data: reward_amount:annual_rate_basis_points:end_block
Example: 100000:500:0
```
This creates a pool with 100,000 tokens as rewards at a 5% annual rate. End block of 0 means it runs until rewards run out. The creator pays the
reward amount from their balance.

**Stake tokens:**
```
Transaction type: Stake
Token: must match the pool's token
Amount: how many tokens to stake
Extra data: pool_id
```
Tokens are deducted from your balance and locked in the pool. You start earning rewards immediately.

**Unstake tokens:**
```
Transaction type: Unstake
Extra data: stake_entry_id
```
Returns your staked tokens plus any pending rewards. The entry ID comes from `getStakesByOwner`.

**Claim rewards without unstaking:**
```
Transaction type: ClaimReward
Extra data: stake_entry_id
```
Claims pending rewards without removing your stake from the pool.

**Fund an existing pool:**
```
Transaction type: FundRewardPool
Extra data: pool_id:amount
```
Anyone can add more rewards to a pool. The amount is deducted from their balance.

**How it works automatically:**
Every block, the validator runs `processRewardAccrual`. It calculates how many rewards to distribute per block based on the annual rate and the
total staked amount. Rewards are split proportionally among stakers and added to their pending rewards.

---

## AMM (Automated Market Maker)

The AMM provides liquidity pools where anyone can swap tokens instantly without needing a counterparty. Prices are set by the constant product
formula `reserveA * reserveB = k`.

**Create a pool:**
```
RPC: amm_createPool
Parameters: tokenIdA, tokenIdB, amountA, amountB, creator, feeBasisPoints (optional, defaults to 30 = 0.3%)
```
The creator seeds the pool with both tokens. They receive LP tokens representing their share. The pool is now live and anyone can swap against it.

**Add liquidity:**
```
RPC: amm_addLiquidity
Parameters: poolId, amountA, amountB, provider
```
You must provide both tokens in the current pool ratio. You receive LP tokens proportional to your share of the pool.

**Remove liquidity:**
```
RPC: amm_removeLiquidity
Parameters: positionId, owner
```
Burns your LP tokens and returns your share of both reserves. The position ID comes from `amm_getPositions`.

**Swap tokens:**
```
RPC: amm_swap
Parameters: poolId, tokenIdIn, amountIn, minAmountOut, from
```
Swaps token A for token B using the pool. `minAmountOut` protects against slippage. If the pool cannot give you at least that much, the
transaction is rejected.

**Get a quote without executing:**
```
RPC: amm_getQuote
Parameters: poolId, tokenIdIn, amountIn
```
Returns how much you would receive for a swap without actually executing it.

**Query pools:**
```
RPC: amm_getPools
```
Returns all active pools with their reserves, fees, and total liquidity.

**Query your positions:**
```
RPC: amm_getPositions
Parameters: owner
```
Returns all your liquidity positions across all pools.
