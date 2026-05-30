// HistoryScreen - transaction history browser
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Screen.h"
#include <memory>
#include <vector>

namespace BoltCore
{
  class Wallet;
  struct OutputInfo;
}

namespace ClientWallet
{

  class HistoryScreen : public Screen
  {
  public:
    HistoryScreen(std::shared_ptr<BoltCore::Wallet> wallet);

    void onEnter() override;
    void onKey(int key) override;
    void render(Tui::ScreenBuffer &buf) override;
    std::string title() const override { return "Transaction History"; }

  private:
    std::shared_ptr<BoltCore::Wallet> m_wallet;
    std::vector<BoltCore::OutputInfo> m_transactions;
    int m_scrollOffset = 0;
    int m_selectedRow = 0;
    static constexpr int VISIBLE_ROWS = 20;
  
    bool m_showDeposits;
    bool m_showSpent;
    bool m_showReceived;
  };

} // namespace ClientWallet