// RelayHandler - sends signed transactions to the network
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "BoltCoreTypes.h"
#include "INode.h"
#include <functional>
#include <string>

namespace cn
{
  class Transaction;
}

namespace BoltCore
{
  class RelayHandler
  {
  public:
    RelayHandler(cn::INode &node);

    using Callback = std::function<void(TransferResult)>;

    // Relay a signed transaction to the network
    void relay(const cn::Transaction &tx, Callback callback);

    // Synchronous relay (blocks until complete)
    TransferResult relaySync(const cn::Transaction &tx);

  private:
    cn::INode &m_node;
  };
}