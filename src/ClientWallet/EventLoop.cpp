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
#include "Screens/ReceiveScreen.h"

#include "Common/Tui.h"

#include "BoltCore/BoltCore.h"

#include "CryptoNoteCore/Currency.h"

#include "INode.h"

#include <iostream>
#include <thread>
#include <fstream>

namespace ClientWallet
{

  EventLoop::EventLoop() = default;
  EventLoop::~EventLoop() { stop(); }

  void EventLoop::setWallet(std::shared_ptr<BoltCore::Wallet> wallet) { m_wallet = std::move(wallet); }
  void EventLoop::setSyncEngine(std::shared_ptr<SyncEngine> sync) { m_sync = std::move(sync); }
  void EventLoop::setNode(cn::INode *node) { m_node = node; }
  void EventLoop::setStatePath(const std::string &path) { m_statePath = path; }

  void EventLoop::run()
  {
    if (!m_wallet || !m_sync)
      return;
    if (m_running)
      return;

    m_running = true;
    m_lastRender = std::chrono::steady_clock::now();
    m_lastHeightCheck = std::chrono::steady_clock::now();
    m_lastMempoolCheck = std::chrono::steady_clock::now();

    // Enter alternate screen
    std::cout << Tui::enterAltScreen() << Tui::hideCursor();
    Tui::enableRawMode();

    // Start sync engine
    m_sync->start(
        [this](const std::vector<BoltCore::OutputInfo> &outputs)
        {
          for (const auto &out : outputs)
          {
            m_wallet->addOutput(out);

            // Create incoming transaction record
            BoltCore::TransactionRecord tx;
            tx.txHash = out.txHash;
            tx.blockHeight = out.blockHeight;
            tx.timestamp = 0; // Could look up block timestamp
            tx.totalReceived = out.amount;
            tx.totalSent = 0;
            tx.type = out.isDeposit ? BoltCore::TransactionType::Deposit : BoltCore::TransactionType::Incoming;
            tx.confirmed = true;
            tx.outputs.push_back(out);
            m_wallet->addTransaction(tx);
          }
          m_wallet->setCurrentHeight(m_sync->lastScannedHeight());
          m_needsRedraw = true;
        },
        [this](const SyncStatus &status)
        {
          m_lastKnownHeight = status.currentHeight;
          if (status.scannedHeight > 0)
            m_wallet->setCurrentHeight(status.scannedHeight);
          m_needsRedraw = true;
        });

    // Main event loop
    while (m_running)
    {
      auto now = std::chrono::steady_clock::now();

      checkKeyboard();

      auto sinceHeightCheck = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastHeightCheck).count();
      if (sinceHeightCheck >= HEIGHT_CHECK_INTERVAL_SEC)
      {
        checkDaemonHeight();
        m_lastHeightCheck = now;
      }

      auto sinceMempool = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastMempoolCheck).count();
      if (sinceMempool >= MEMPOOL_CHECK_INTERVAL_SEC)
      {
        checkMempool();
        m_lastMempoolCheck = now;
      }

      auto sinceRender = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastRender).count();
      if (m_needsRedraw && sinceRender >= RENDER_INTERVAL_MS)
      {
        renderCurrentScreen();
        m_lastRender = now;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Save state on exit
    if (!m_statePath.empty() && m_sync)
    {
      m_sync->saveStateFile(m_statePath);
    }

    Tui::disableRawMode();
    std::cout << Tui::showCursor() << Tui::exitAltScreen();
  }

  void EventLoop::stop()
  {
    m_running = false;
    if (m_sync)
    {
      // Detach the sync thread so we don't block on it.
      // The sync engine destructor will handle cleanup.
      m_sync->stop();
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
        case ScreenAction::Pop:
          popScreen();
          break;
        case ScreenAction::Quit:
          stop();
          break;
        default: break;
      } });
    screen->onEnter();
    m_screens.push_back(std::move(screen));
    m_needsRedraw = true;
  }

  void EventLoop::popScreen()
  {
    if (!m_screens.empty())
    {
      m_screens.back()->onExit();
      m_screens.pop_back();
    }
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
      case 27:
        if (m_screens.size() > 1)
        {
          popScreen();
          return;
        }
        break;
      }

      processEvent(Event(EventType::KeyPress, key));
    }
  }

  void EventLoop::checkDaemonHeight()
  {
    if (!m_node)
      return;
    uint32_t currentHeight = m_node->getLastKnownBlockHeight();
    if (currentHeight > m_lastKnownHeight)
    {
      m_lastKnownHeight = currentHeight;
      m_sync->onNewBlockHeight(currentHeight);
      m_needsRedraw = true;
    }
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
        m_needsRedraw = true;
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

  void EventLoop::checkMempool()
  {
    if (!m_node || !m_wallet)
      return;

    // Get pool changes since last check
    // For now, just update pending balance display
    auto pending = m_wallet->getPendingOutgoingAmount();
    if (pending > 0)
      m_needsRedraw = true;
  }
} // namespace ClientWallet