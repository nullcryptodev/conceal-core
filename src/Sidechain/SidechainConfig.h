// SidechainConfig.h — chain constants (like CryptoNoteConfig.h for the main chain)
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <string>

namespace SidechainConfig
{
  // Chain identity
  const std::string NAME = "Conceal Sidechain";
  const uint32_t NETWORK_ID_MAIN = 0x53434358; // "SCCX"
  const uint32_t NETWORK_ID_TEST = 0x54434358; // "TCCX"
  const std::string SYMBOL = "sCCX";

  // Block timing
  const uint32_t BLOCK_TIME_SECONDS = 1;
  const uint32_t BLOCK_TIME_MS = BLOCK_TIME_SECONDS * 1000;

  // Genesis
  const uint32_t GENESIS_TIMESTAMP = 1714867200;

  // Validators
  const uint32_t MIN_VALIDATORS = 1;
  const uint32_t MAX_VALIDATORS = 21;

  // Fees (in smallest unit)
  const uint64_t MINIMUM_FEE = 1;
  const uint64_t DEFAULT_FEE = 10;
  const uint64_t TESTNET_FEE = 1;

  // Faucet
  const uint64_t FAUCET_AMOUNT = 2; // enough for 2 transfers at 1 SCCX each

  // Block reward (minted to proposer every committed block)
  const uint64_t BLOCK_REWARD = 1;

  // Rate limiting: minimum blocks between token creations per address
  const uint64_t TOKEN_CREATE_COOLDOWN_BLOCKS = 10; // TODO time based

  // Minimum time between blocks to prevent spam (milliseconds)
  const uint64_t MIN_BLOCK_INTERVAL_MS = 500;

  // Token defaults
  const uint64_t DEFAULT_BACKING_RATIO = 100;
  const uint64_t MAX_TOKEN_NAME_LENGTH = 16;
  const uint64_t MAX_TOKEN_SYMBOL_LENGTH = 8;

  // Transaction limits
  const uint64_t MAX_TRANSACTIONS_PER_BLOCK = 1000;
  const uint64_t MAX_TRANSACTION_SIZE_BYTES = 1024 * 10;

  // MDBX
  const std::string DATABASE_NAME = "sidechain_blocks";

  // RPC
  const std::string DEFAULT_RPC_BIND_IP = "127.0.0.1";
  const uint16_t DEFAULT_RPC_BIND_PORT = 8080;

  // Fork heights (for future upgrades)
  const uint32_t FORK_HEIGHT_V1_BASIC = 0;
  const uint32_t FORK_HEIGHT_V2_STAKING = 10000000000;
  const uint32_t FORK_HEIGHT_V3_NFTS = 25000000000;

  // BFT Consensus
  const uint32_t BFT_BLOCK_THRESHOLD = 1;       // minimum signatures to commit (1=testing, 3=2-of-3)
  const uint32_t GOSSIP_PORT_OFFSET = 1000;     // RPC port + offset = gossip port
  const uint32_t BFT_PROPOSAL_TIMEOUT_MS = 500; // max wait to collect transactions before proposing
  const uint32_t BFT_VOTE_TIMEOUT_MS = 1000;    // max wait for votes after proposal
  const uint32_t BFT_MAX_PROPOSAL_RETRIES = 3;  // retries if proposal fails
}