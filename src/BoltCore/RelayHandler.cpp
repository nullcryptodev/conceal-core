// RelayHandler implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "RelayHandler.h"

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

    std::error_code ec = future.get();
    result.success = !ec;
    if (ec)
      result.error = ec.message();

    return result;
  }
}