// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
//
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "Blockchain.h"
#include "CryptoNoteCore/TransactionExtra.h"

namespace cn
{
  // Observer management
  bool Blockchain::addObserver(IBlockchainStorageObserver *observer)
  {
    return m_observerManager.add(observer);
  }

  bool Blockchain::removeObserver(IBlockchainStorageObserver *observer)
  {
    return m_observerManager.remove(observer);
  }

  // Message queue
  bool Blockchain::addMessageQueue(MessageQueue<BlockchainMessage> &messageQueue)
  {
    return m_messageQueueList.insert(messageQueue);
  }

  bool Blockchain::removeMessageQueue(MessageQueue<BlockchainMessage> &messageQueue)
  {
    return m_messageQueueList.remove(messageQueue);
  }

  void Blockchain::sendMessage(const BlockchainMessage &message)
  {
    for (auto &queue : m_messageQueueList)
      queue.push(message);
  }

  // Diagnostics
  std::string Blockchain::printDatabaseStats() const
  {
    if (m_mdbxStorage)
      return m_mdbxStorage->printDatabaseStats();
    return "MDBX not available";
  }

  // Checkpoint callback
  void Blockchain::setCheckpointGeneratedCallback(CheckpointGeneratedCallback callback)
  {
    m_checkpointGeneratedCallback = std::move(callback);
  }
} // namespace cn