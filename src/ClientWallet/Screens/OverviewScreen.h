// OverviewScreen - main wallet overview
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Screen.h"
#include <memory>

namespace BoltCore
{
  class Wallet;
}

namespace ClientWallet
{

  class OverviewScreen : public Screen
  {
  public:
    OverviewScreen(std::shared_ptr<BoltCore::Wallet> wallet);

    void onEnter() override;
    void onKey(int key) override;
    void render(Tui::ScreenBuffer &buf) override;
    std::string title() const override { return "Conceal Wallet"; }

  private:
    std::shared_ptr<BoltCore::Wallet> m_wallet;
    int m_selectedRow = 0;
  };

} // namespace ClientWallet