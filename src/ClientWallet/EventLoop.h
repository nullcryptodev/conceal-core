// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crypto/crypto.h"
#include "BoltCore/BoltCoreTypes.h"
#include "SyncEngine.h"

namespace cn
{
  class INode;
  class Currency;
}
namespace BoltCore
{
  class Wallet;
}

namespace ClientWallet
{

  class SyncEngine;
  class Screen;

  struct SyncWalletUpdate
  {
    std::vector<BoltCore::OutputInfo> outputs;
    std::vector<BoltCore::OutputInfo> outputMetadata;
    std::vector<crypto::KeyImage> spentKeyImages;
    std::vector<std::pair<crypto::Hash, uint32_t>> depositSpends;
    std::vector<std::pair<crypto::Hash, uint32_t>> outputSpends;
    uint32_t scannedHeight = 0;
    bool updateHeight = false;
  };

  enum class EventType
  {
    KeyPress,
    HeightChanged,
    SyncProgress,
    OutputsReceived,
    BalanceChanged,
    Timer,
    Quit
  };

  struct Event
  {
    Event() = default;
    Event(EventType t) : type(t) {}
    Event(EventType t, int k) : type(t), keyCode(k) {}

    EventType type = EventType::KeyPress;
    int keyCode = 0;
    uint32_t height = 0;
    std::string message;
  };

  class EventLoop
  {
  public:
    EventLoop();
    ~EventLoop();

    void setWallet(std::shared_ptr<BoltCore::Wallet> wallet);
    void setSyncEngine(std::shared_ptr<SyncEngine> sync);
    void setNode(cn::INode *node);

    void run();
    void stop();

    void pushScreen(std::shared_ptr<Screen> screen);
    void popScreen();

    void setStatePath(const std::string &path);

    void setCurrency(const cn::Currency &currency) { m_currency = &currency; }

    // Called from a background thread after the UI is shown (daemon connect + mempool).
    void setDeferredDaemonInit(std::function<bool()> initFn) { m_deferredDaemonInit = std::move(initFn); }

  private:
    void processEvent(const Event &event);
    void checkKeyboard();
    void backgroundPollLoop();
    void renderCurrentScreen();
    void enqueueSyncUpdate(SyncWalletUpdate update);
    void applyPendingSyncUpdates();
    void applyPendingMempoolUpdates();
    void submitWalletTask(std::function<BoltCore::TransferResult()> task,
                          std::function<void(BoltCore::TransferResult)> done);
    void walletTaskLoop();
    void applyCompletedWalletTasks();
    BoltCore::TransferResult runSpendPreflight(std::function<BoltCore::TransferResult()> task);
    void mergeBackfilledGlobalIndices();

    std::string m_statePath;

    std::shared_ptr<BoltCore::Wallet> m_wallet;
    std::shared_ptr<SyncEngine> m_sync;

    cn::INode *m_node = nullptr;
    const cn::Currency *m_currency = nullptr;
    std::function<bool()> m_deferredDaemonInit;
    bool m_deferredDaemonInitDone = false;

    std::vector<std::shared_ptr<Screen>> m_screens;
    std::atomic<bool> m_running{false};

    std::mutex m_syncUpdateMutex;
    std::deque<SyncWalletUpdate> m_pendingSyncUpdates;

    std::mutex m_mempoolUpdateMutex;
    std::deque<SyncEngine::MempoolUpdate> m_pendingMempoolUpdates;
    std::thread m_pollThread;

    std::mutex m_walletMutex;
    std::atomic<bool> m_walletTaskRunning{false};
    std::atomic<uint64_t> m_walletTaskEpoch{0};
    std::mutex m_walletTaskMutex;
    std::condition_variable m_walletTaskCv;
    bool m_walletTaskPending = false;
    std::function<BoltCore::TransferResult()> m_walletTask;
    std::function<void(BoltCore::TransferResult)> m_walletTaskDone;
    std::deque<std::pair<std::function<void(BoltCore::TransferResult)>, BoltCore::TransferResult>> m_walletTaskResults;
    std::thread m_walletTaskThread;

    std::chrono::steady_clock::time_point m_lastRender;

    uint32_t m_lastKnownHeight = 0;
    bool m_needsRedraw = true;

    static constexpr int BACKGROUND_POLL_INTERVAL_SEC = 5;
    static constexpr int RENDER_INTERVAL_MS = 200;
  };

} // namespace ClientWallet
