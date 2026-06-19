// Screen - base class for TUI screens
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Common/Tui.h"
#include "BoltCore/BoltCoreTypes.h"
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace ClientWallet
{
  enum class ScreenAction
  {
    None,
    GoToOverview,
    GoToSend,
    GoToReceive,
    GoToHistory,
    GoToDeposit,
    GoToFusion,
    Pop,
    Quit
  };

  using ActionCallback = std::function<void(ScreenAction)>;
  using WalletTaskSubmitFn = std::function<void(
      std::function<BoltCore::TransferResult()>,
      std::function<void(BoltCore::TransferResult)>)>;

  class Screen
  {
  public:
    virtual ~Screen() = default;

    virtual void onEnter() {}
    virtual void onExit() {}
    virtual void onKey(int key) = 0;
    virtual void render(Tui::ScreenBuffer &buf) = 0;

    virtual std::string title() const = 0;

    void setActionCallback(ActionCallback cb) { m_onAction = std::move(cb); }
    void setWalletTaskSubmit(WalletTaskSubmitFn fn) { m_submitWalletTask = std::move(fn); }

  protected:
    // Helper: draw header with title and status info
    void drawHeader(Tui::ScreenBuffer &buf,
                    const std::string &title,
                    uint32_t height,
                    uint64_t balance,
                    const std::string &status);

    // Helper: draw menu bar at bottom
    void drawMenuBar(Tui::ScreenBuffer &buf,
                     const std::vector<std::string> &items,
                     const std::vector<std::string> &keys,
                     int left = 0);

    // Helper: tail of /tmp/conceal-wallet-sync.log
    void drawLastSyncLog(Tui::ScreenBuffer &buf, int boxTop, size_t lineCount = 6);

    // Helper: show a centered message box
    void showMessage(Tui::ScreenBuffer &buf,
                     const std::string &title,
                     const std::string &message);

    ActionCallback m_onAction;
    WalletTaskSubmitFn m_submitWalletTask;

  };

} // namespace ClientWallet