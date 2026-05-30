// BoltDexTypes.h — order book, trade, and settlement types
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include "crypto/crypto.h"

namespace Sidechain
{
  namespace BoltDex
  {
    enum class OrderType : uint8_t
    {
      Buy = 0,
      Sell = 1
    };

    enum class OrderStatus : uint8_t
    {
      Open = 0,
      Filled = 1,
      Cancelled = 2
    };

    struct Order
    {
      uint64_t id = 0;
      OrderType type = OrderType::Buy;
      crypto::PublicKey owner;
      uint64_t baseTokenId = 0;
      uint64_t quoteTokenId = 0;
      uint64_t amount = 0;
      uint64_t price = 0;
      uint64_t filled = 0;
      OrderStatus status = OrderStatus::Open;
      uint64_t timestamp = 0;
      crypto::Hash orderHash;
    };

    struct Trade
    {
      uint64_t id = 0;
      uint64_t buyOrderId = 0;
      uint64_t sellOrderId = 0;
      crypto::PublicKey buyer;
      crypto::PublicKey seller;
      uint64_t baseTokenId = 0;
      uint64_t quoteTokenId = 0;
      uint64_t amount = 0;
      uint64_t price = 0;
      uint64_t timestamp = 0;
      crypto::Hash tradeHash;
      crypto::Hash settlementTxHash;
      bool settled = false;
    };

    struct Settlement
    {
      uint64_t tradeId = 0;
      crypto::PublicKey from;
      crypto::PublicKey to;
      uint64_t tokenId = 0;
      uint64_t amount = 0;
      std::string description;
    };
  }
}