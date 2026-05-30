// Screen - base class for TUI screens
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Common/Tui.h"
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
    Pop,
    Quit
  };

  using ActionCallback = std::function<void(ScreenAction)>;

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
                     const std::vector<std::string> &keys);

    // Helper: show a centered message box
    void showMessage(Tui::ScreenBuffer &buf,
                     const std::string &title,
                     const std::string &message);

    ActionCallback m_onAction;

  };

} // namespace ClientWallet