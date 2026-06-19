// RelayHandler implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "RelayHandler.h"

#include <chrono>
#include <future>

#include "CryptoNoteCore/CryptoNoteBasic.h"
#include "Common/StringTools.h"

namespace BoltCore
{
  RelayHandler::RelayHandler(cn::INode &node) : m_node(node) {}

  void RelayHandler::relay(const cn::Transaction &tx, Callback callback)
  {
    m_node.relayTransaction(tx, [callback](std::error_code ec)
                            {
      TransferResult result;
      result.success = !ec;
      if (ec) result.error = ec.message();
      callback(result); });
  }

  TransferResult RelayHandler::relaySync(const cn::Transaction &tx)
  {
    TransferResult result;
    result.success = false;

    std::promise<std::error_code> promise;
    auto future = promise.get_future();

    m_node.relayTransaction(tx, [&promise](std::error_code ec)
                            { promise.set_value(ec); });

    if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
    {
      result.error = "Timed out relaying transaction to daemon (check conceald RPC on port 16000)";
      return result;
    }

    std::error_code ec = future.get();
    result.success = !ec;
    if (ec)
    {
      if (ec == std::errc::invalid_argument)
        result.error = "Daemon rejected transaction (invalid inputs or ring signature — rescan wallet and retry)";
      else
        result.error = ec.message().empty() ? "Failed to relay transaction" : ec.message();
    }

    return result;
  }
}