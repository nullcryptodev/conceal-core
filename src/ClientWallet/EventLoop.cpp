// EventLoop implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "EventLoop.h"
#include "SyncEngine.h"

#include "Screens/Screen.h"
#include "Screens/OverviewScreen.h"
#include "Screens/SendScreen.h"
#include "Screens/HistoryScreen.h"
#include "Screens/DepositScreen.h"
#include "Screens/FusionScreen.h"
#include "Screens/ReceiveScreen.h"

#include "Common/Tui.h"

#include "BoltCore/BoltCore.h"

#include "CryptoNoteCore/Currency.h"

#include "INode.h"
#include "NodeClient/NodeClient.h"

#include <iostream>
#include <thread>
#include <fstream>
#include <exception>
#include <unordered_map>

#include <boost/functional/hash.hpp>

namespace ClientWallet
{

  namespace
  {
    uint32_t maxBlockHeight(const std::vector<BoltCore::OutputInfo> &outputs)
    {
      uint32_t height = 0;
      for (const auto &out : outputs)
      {
        if (out.blockHeight > height)
          height = out.blockHeight;
      }
      return height;
    }

    std::unordered_map<crypto::Hash, std::vector<BoltCore::OutputInfo>, boost::hash<crypto::Hash>>
    groupOutputsByTxHash(const std::vector<BoltCore::OutputInfo> &outputs)
    {
      std::unordered_map<crypto::Hash, std::vector<BoltCore::OutputInfo>, boost::hash<crypto::Hash>> grouped;
      for (const auto &out : outputs)
        grouped[out.txHash].push_back(out);
      return grouped;
    }

    void mergeOutputsIntoCache(SyncEngine *sync,
                               const crypto::Hash &txHash,
                               const std::vector<BoltCore::OutputInfo> &outputs,
                               uint32_t blockHeight)
    {
      if (!sync)
        return;
      for (const auto &out : outputs)
      {
        BoltCore::OutputInfo cached = out;
        cached.txHash = txHash;
        cached.blockHeight = blockHeight > 0 ? blockHeight : 0;
        sync->mergeCachedOutput(cached);
      }
    }

    void addDiscoveredTx(BoltCore::Wallet &wallet,
                         SyncEngine *sync,
                         const crypto::Hash &txHash,
                         const std::vector<BoltCore::OutputInfo> &outputs,
                         uint32_t blockHeight,
                         bool &changed)
    {
      if (outputs.empty())
        return;

      std::vector<BoltCore::OutputInfo> normalized = outputs;
      for (auto &out : normalized)
        out.txHash = txHash;

      wallet.addDiscoveredTransaction(txHash, normalized, blockHeight);
      mergeOutputsIntoCache(sync, txHash, normalized, blockHeight);
      if (sync && blockHeight > 0 && !wallet.hasPendingOutgoing(txHash))
        sync->noteIncomingTxApplied(txHash);
      changed = true;
    }

    void wireGlobalIndexResolver(const std::shared_ptr<BoltCore::Wallet> &wallet,
                                 const std::shared_ptr<SyncEngine> &sync)
    {
      if (!wallet || !sync)
        return;
      std::weak_ptr<SyncEngine> weakSync = sync;
      wallet->setGlobalOutputIndexResolver(
          [weakSync](const BoltCore::OutputInfo &out, uint32_t &globalIndex)
          {
            const auto engine = weakSync.lock();
            if (!engine)
              return false;
            return engine->lookupGlobalOutputIndex(out, globalIndex);
          });
    }
  }

  EventLoop::EventLoop() = default;
  EventLoop::~EventLoop() { stop(); }

  void EventLoop::setWallet(std::shared_ptr<BoltCore::Wallet> wallet)
  {
    m_wallet = std::move(wallet);
    wireGlobalIndexResolver(m_wallet, m_sync);
  }
  void EventLoop::setSyncEngine(std::shared_ptr<SyncEngine> sync)
  {
    m_sync = std::move(sync);
    wireGlobalIndexResolver(m_wallet, m_sync);
  }
  void EventLoop::setNode(cn::INode *node) { m_node = node; }
  void EventLoop::setStatePath(const std::string &path) { m_statePath = path; }

  void EventLoop::enqueueSyncUpdate(SyncWalletUpdate update)
  {
    std::lock_guard<std::mutex> lock(m_syncUpdateMutex);
    m_pendingSyncUpdates.push_back(std::move(update));
  }

  void EventLoop::applyPendingSyncUpdates()
  {
    if (!m_wallet || m_walletTaskRunning.load())
      return;

    std::deque<SyncWalletUpdate> pending;
    {
      std::lock_guard<std::mutex> lock(m_syncUpdateMutex);
      if (m_pendingSyncUpdates.empty())
        return;
      pending.swap(m_pendingSyncUpdates);
    }

    std::unique_lock<std::mutex> walletLock(m_walletMutex, std::try_to_lock);
    if (!walletLock.owns_lock())
    {
      std::lock_guard<std::mutex> lock(m_syncUpdateMutex);
      for (auto &update : pending)
        m_pendingSyncUpdates.push_front(std::move(update));
      return;
    }

    for (auto &update : pending)
    {
      const auto grouped = groupOutputsByTxHash(update.outputs);
      for (const auto &entry : grouped)
      {
        const uint32_t blockHeight = maxBlockHeight(entry.second);
        addDiscoveredTx(*m_wallet, m_sync.get(), entry.first, entry.second, blockHeight,
                        m_needsRedraw);
      }

      for (const auto &out : update.outputMetadata)
        m_wallet->mergeOutput(out);

      for (const auto &ki : update.spentKeyImages)
        m_wallet->markOutputSpent(ki);
      for (const auto &ref : update.outputSpends)
        m_wallet->markOutputSpentByRef(ref.first, ref.second);
      for (const auto &dep : update.depositSpends)
        m_wallet->markDepositOutputSpent(dep.first, dep.second);

      if (update.updateHeight && update.scannedHeight > 0)
        m_wallet->setCurrentHeight(update.scannedHeight);


      if (!update.spentKeyImages.empty() || !update.outputSpends.empty() ||
          !update.depositSpends.empty() || (!grouped.empty() && update.scannedHeight > 0))
        m_wallet->confirmPendingOutgoing(update.scannedHeight);
    }

    if (!pending.empty())
    {
      m_needsRedraw = true;
      mergeBackfilledGlobalIndices();
    }
  }

  void EventLoop::run()
  {
    if (!m_wallet || !m_sync)
      return;
    if (m_running)
      return;

    m_running = true;
    m_lastRender = std::chrono::steady_clock::now();

    // Enter alternate screen and paint immediately — no blocking work before this.
    std::cout << Tui::enterAltScreen() << Tui::hideCursor();
    Tui::enableRawMode();
    renderCurrentScreen();
    m_lastRender = std::chrono::steady_clock::now();

    // Start sync engine (background thread)
    m_sync->start(
        [this](const std::vector<BoltCore::OutputInfo> &outputs)
        {
          SyncWalletUpdate update;
          update.outputs = outputs;
          update.scannedHeight = m_sync->lastScannedHeight();
          update.updateHeight = true;
          enqueueSyncUpdate(std::move(update));
        },
        [this](const SyncStatus &status)
        {
          m_lastKnownHeight = status.currentHeight;
          if (status.scannedHeight > 0)
          {
            SyncWalletUpdate update;
            update.scannedHeight = status.scannedHeight;
            update.updateHeight = true;
            enqueueSyncUpdate(std::move(update));
          }
        },
        [this](const std::vector<crypto::KeyImage> &keyImages,
               const std::vector<std::pair<crypto::Hash, uint32_t>> &depositSpends,
               const std::vector<std::pair<crypto::Hash, uint32_t>> &outputSpends)
        {
          if (keyImages.empty() && depositSpends.empty() && outputSpends.empty())
            return;
          SyncWalletUpdate update;
          update.spentKeyImages = keyImages;
          update.depositSpends = depositSpends;
          update.outputSpends = outputSpends;
          update.scannedHeight = m_sync->lastScannedHeight();
          enqueueSyncUpdate(std::move(update));
        },
        [this](const std::vector<BoltCore::OutputInfo> &updates)
        {
          if (updates.empty())
            return;
          SyncWalletUpdate update;
          update.outputMetadata = updates;
          enqueueSyncUpdate(std::move(update));
        });

    m_pollThread = std::thread([this]()
                               { backgroundPollLoop(); });

    m_walletTaskThread = std::thread([this]()
                                     { walletTaskLoop(); });

    // Main event loop
    while (m_running)
    {
      auto now = std::chrono::steady_clock::now();

      checkKeyboard();
      applyCompletedWalletTasks();

      if (m_walletTaskRunning.load())
        m_needsRedraw = true;

      if (m_needsRedraw)
      {
        auto sinceRender = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRender).count();
        if (sinceRender >= RENDER_INTERVAL_MS)
        {
          renderCurrentScreen();
          m_lastRender = now;
        }
      }

      applyPendingSyncUpdates();
      applyPendingMempoolUpdates();

      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    Tui::disableRawMode();

    Tui::runWithStatusSpinner("Saving wallet", [this]()
                              {
                                if (m_sync)
                                  m_sync->stop();

                                if (m_pollThread.joinable())
                                  m_pollThread.join();

                                if (m_walletTaskThread.joinable())
                                  m_walletTaskThread.join();

                                applyCompletedWalletTasks();
                                applyPendingSyncUpdates();
                                applyPendingMempoolUpdates();

                                if (!m_statePath.empty() && m_sync && m_wallet)
                                {
                                  if (!m_sync->saveStateFile(m_statePath, m_wallet->getOutputs()))
                                    std::cerr << "Warning: failed to save wallet state to "
                                              << m_statePath << std::endl;
                                }
                              });

    std::cout << Tui::showCursor() << Tui::exitAltScreen() << std::flush;
  }

  void EventLoop::stop()
  {
    m_running = false;
    if (m_sync)
      m_sync->requestStop();
    {
      std::lock_guard<std::mutex> lock(m_walletTaskMutex);
      m_walletTaskCv.notify_all();
    }
  }

  void EventLoop::pushScreen(std::shared_ptr<Screen> screen)
  {
    screen->setActionCallback([this](ScreenAction action)
                              {
      switch (action) {
        case ScreenAction::GoToOverview:
          while (!m_screens.empty()) m_screens.pop_back();
          pushScreen(std::make_shared<OverviewScreen>(m_wallet));
          break;
        case ScreenAction::GoToSend:
          pushScreen(std::make_shared<SendScreen>(m_wallet));
          break;
        case ScreenAction::GoToReceive:
          pushScreen(std::make_shared<ReceiveScreen>(m_wallet));
          break;
        case ScreenAction::GoToHistory:
          pushScreen(std::make_shared<HistoryScreen>(m_wallet));
          break;
        case ScreenAction::GoToDeposit:
          if (m_currency)
            pushScreen(std::make_shared<DepositScreen>(m_wallet, *m_currency));
          break;
        case ScreenAction::GoToFusion:
          if (m_currency)
            pushScreen(std::make_shared<FusionScreen>(m_wallet, *m_currency));
          break;
        case ScreenAction::Pop:
          popScreen();
          break;
        case ScreenAction::Quit:
          stop();
          break;
        default: break;
      } });
    screen->setWalletTaskSubmit([this](std::function<BoltCore::TransferResult()> task,
                                       std::function<void(BoltCore::TransferResult)> done)
                                { submitWalletTask(std::move(task), std::move(done)); });
    screen->onEnter();
    m_screens.push_back(std::move(screen));
    m_needsRedraw = true;
  }

  void EventLoop::popScreen()
  {
    if (m_screens.size() <= 1)
      return;

    m_screens.back()->onExit();
    m_screens.pop_back();
    if (!m_screens.empty())
      m_screens.back()->onEnter();
    m_needsRedraw = true;
  }

  void EventLoop::checkKeyboard()
  {
    while (Tui::keyAvailable())
    {
      int key = Tui::readKey();

      switch (key)
      {
      case 'q':
      case 'Q':
        if (m_screens.size() <= 1)
        {
          processEvent(Event(EventType::Quit));
          return;
        }
        break;
      }

      processEvent(Event(EventType::KeyPress, key));
    }
  }

  void EventLoop::backgroundPollLoop()
  {
    while (m_running)
    {
      if (!m_deferredDaemonInitDone && m_deferredDaemonInit && m_running)
      {
        m_deferredDaemonInitDone = m_deferredDaemonInit();
        if (m_deferredDaemonInitDone && m_running)
          mergeBackfilledGlobalIndices();
      }

      if (!m_running)
        break;

      if (m_node && m_sync && m_running && !m_sync->isStopping())
      {
        const uint32_t currentHeight = m_node->getLastKnownBlockHeight();
        m_sync->notifyDaemonHeight(currentHeight);
        if (currentHeight > m_lastKnownHeight)
        {
          m_lastKnownHeight = currentHeight;
          m_needsRedraw = true;
        }
        if (m_running && !m_sync->isStopping())
          m_sync->syncIfBehind();

        if (m_running && !m_sync->isStopping() && m_wallet)
        {
          SyncEngine::MempoolUpdate update = m_sync->pollMempool();
          if (!update.newIncoming.empty() || !update.removedFromPool.empty() ||
              !update.confirmedOutputs.empty())
          {
            std::lock_guard<std::mutex> lock(m_mempoolUpdateMutex);
            m_pendingMempoolUpdates.push_back(std::move(update));
          }
        }
      }

      for (int i = 0; i < BACKGROUND_POLL_INTERVAL_SEC * 10 && m_running; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  void EventLoop::applyPendingMempoolUpdates()
  {
    if (!m_wallet || m_walletTaskRunning.load())
      return;

    std::deque<SyncEngine::MempoolUpdate> pending;
    {
      std::lock_guard<std::mutex> lock(m_mempoolUpdateMutex);
      if (m_pendingMempoolUpdates.empty())
        return;
      pending.swap(m_pendingMempoolUpdates);
    }

    std::unique_lock<std::mutex> walletLock(m_walletMutex, std::try_to_lock);
    if (!walletLock.owns_lock())
    {
      std::lock_guard<std::mutex> lock(m_mempoolUpdateMutex);
      for (auto &update : pending)
        m_pendingMempoolUpdates.push_front(std::move(update));
      return;
    }

    bool changed = false;

    for (const auto &update : pending)
    {
      const auto confirmedByTx = groupOutputsByTxHash(update.confirmedOutputs);

      std::unordered_set<crypto::Hash, boost::hash<crypto::Hash>> confirmedTxHashes;
      confirmedTxHashes.reserve(confirmedByTx.size());
      for (const auto &entry : confirmedByTx)
        confirmedTxHashes.insert(entry.first);

      for (const auto &entry : confirmedByTx)
        addDiscoveredTx(*m_wallet, m_sync.get(), entry.first, entry.second,
                        maxBlockHeight(entry.second), changed);

      for (const auto &entry : update.newIncoming)
      {
        if (confirmedTxHashes.count(entry.txHash))
          continue;
        addDiscoveredTx(*m_wallet, m_sync.get(), entry.txHash, entry.outputs, 0, changed);
      }
      for (const auto &txHash : update.removedFromPool)
      {
        bool confirmedThisBatch = false;
        for (const auto &out : update.confirmedOutputs)
        {
          if (out.txHash == txHash)
          {
            confirmedThisBatch = true;
            break;
          }
        }
        if (confirmedThisBatch)
          continue;

        for (const auto &ptx : m_wallet->getPendingTransactions())
        {
          if (ptx.txHash == txHash)
          {
            m_wallet->confirmTransaction(txHash, 0);
            changed = true;
            break;
          }
        }
      }
    }

    if (changed)
      m_needsRedraw = true;

    if (m_sync && m_wallet)
      mergeBackfilledGlobalIndices();
  }

  void EventLoop::mergeBackfilledGlobalIndices()
  {
    if (!m_running || !m_sync || !m_wallet || m_sync->isStopping())
      return;

    for (const auto &out : m_sync->backfillMissingGlobalOutputIndices())
      m_wallet->mergeOutput(out);
  }

  BoltCore::TransferResult EventLoop::runSpendPreflight(
      std::function<BoltCore::TransferResult()> task)
  {
    if (auto *client = dynamic_cast<NodeClient::NodeClient *>(m_node))
    {
      if (!client->isConnected())
        client->connectToDaemon();
    }
    else if (!m_deferredDaemonInitDone && m_deferredDaemonInit)
    {
      m_deferredDaemonInitDone = m_deferredDaemonInit();
    }

    if (task)
      return task();

    BoltCore::TransferResult empty;
    empty.success = false;
    empty.error = "Internal error: empty wallet task";
    return empty;
  }

  void EventLoop::submitWalletTask(std::function<BoltCore::TransferResult()> task,
                                   std::function<void(BoltCore::TransferResult)> done)
  {
    std::lock_guard<std::mutex> lock(m_walletTaskMutex);
    if (m_walletTaskPending)
    {
      BoltCore::TransferResult busy;
      busy.success = false;
      busy.error = "Another transaction is in progress";
      m_walletTaskResults.push_back({std::move(done), std::move(busy)});
      m_needsRedraw = true;
      return;
    }
    std::function<BoltCore::TransferResult()> innerTask = std::move(task);
    m_walletTask = [this, innerTask]() mutable
    { return runSpendPreflight(std::move(innerTask)); };
    m_walletTaskDone = std::move(done);
    m_walletTaskPending = true;
    m_walletTaskCv.notify_one();
    m_needsRedraw = true;
  }

  void EventLoop::walletTaskLoop()
  {
    while (m_running)
    {
      std::function<BoltCore::TransferResult()> task;
      std::function<void(BoltCore::TransferResult)> done;
      {
        std::unique_lock<std::mutex> lock(m_walletTaskMutex);
        m_walletTaskCv.wait(lock, [this]()
                            { return !m_running || m_walletTaskPending; });
        if (!m_running && !m_walletTaskPending)
          break;
        task = std::move(m_walletTask);
        done = std::move(m_walletTaskDone);
        m_walletTaskPending = false;
      }

      BoltCore::TransferResult result;
      if (task)
      {
        m_walletTaskRunning.store(true);
        try
        {
          result = task();
        }
        catch (const std::exception &ex)
        {
          result.success = false;
          result.error = std::string("Transaction failed: ") + ex.what();
        }
        catch (...)
        {
          result.success = false;
          result.error = "Transaction failed: unexpected error";
        }
        m_walletTaskRunning.store(false);
      }

      {
        std::lock_guard<std::mutex> lock(m_walletTaskMutex);
        m_walletTaskResults.push_back({std::move(done), std::move(result)});
      }
      m_needsRedraw = true;
    }
  }

  void EventLoop::applyCompletedWalletTasks()
  {
    std::deque<std::pair<std::function<void(BoltCore::TransferResult)>, BoltCore::TransferResult>> results;
    {
      std::lock_guard<std::mutex> lock(m_walletTaskMutex);
      if (m_walletTaskResults.empty())
        return;
      results.swap(m_walletTaskResults);
    }

    for (auto &entry : results)
    {
      // Capture before move so we can sync the cache after the callback.
      const bool success = entry.second.success;
      std::vector<BoltCore::OutputInfo> spent = entry.second.spentInputs;

      if (entry.first)
        entry.first(std::move(entry.second));

      // Propagate spent state + computed key images into the SyncEngine cache so
      // markSpentOutputs() can detect the spend via MDBX key-image lookup.
      if (success && m_sync && !spent.empty())
        m_sync->notifyOutputsSpent(spent);
    }
    m_needsRedraw = true;
  }

  void EventLoop::processEvent(const Event &event)
  {
    switch (event.type)
    {
    case EventType::Quit:
      stop();
      break;
    case EventType::KeyPress:
      if (!m_screens.empty())
      {
        m_screens.back()->onKey(event.keyCode);
        renderCurrentScreen();
        m_lastRender = std::chrono::steady_clock::now();
      }
      break;
    default:
      break;
    }
  }

  void EventLoop::renderCurrentScreen()
  {
    Tui::ScreenBuffer buf;
    buf.write(Tui::clearScreen());
    if (!m_screens.empty())
      m_screens.back()->render(buf);
    buf.flush();
    m_needsRedraw = false;
  }

} // namespace ClientWallet