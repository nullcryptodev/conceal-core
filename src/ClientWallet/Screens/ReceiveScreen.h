// ReceiveScreen - display wallet address for receiving funds
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Screen.h"
#include <memory>
#include <string>
#include <vector>

namespace BoltCore
{
  class Wallet;
}

namespace BoltCore
{
  struct SubAddress;
}

namespace ClientWallet
{

  class ReceiveScreen : public Screen
  {
  public:
    ReceiveScreen(std::shared_ptr<BoltCore::Wallet> wallet);

    void onEnter() override;
    void onKey(int key) override;
    void render(Tui::ScreenBuffer &buf) override;
    std::string title() const override { return "Receive CCX"; }

  private:
    std::shared_ptr<BoltCore::Wallet> m_wallet;
    std::string m_mainAddress;
    std::vector<BoltCore::SubAddress> m_subAddresses;
    int m_selectedIndex = 0; // 0 = main address, 1+ = sub-addresses
    bool m_showNewSub = false;
  };

} // namespace ClientWallet