// SendScreen - send CCX to an address
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Screen.h"
#include <memory>
#include <string>

namespace BoltCore
{
  class Wallet;
}

namespace ClientWallet
{

  class SendScreen : public Screen
  {
  public:
    SendScreen(std::shared_ptr<BoltCore::Wallet> wallet);

    void onEnter() override;
    void onKey(int key) override;
    void render(Tui::ScreenBuffer &buf) override;
    std::string title() const override { return "Send CCX"; }

  private:
    enum class State
    {
      EnterAddress,
      EnterAmount,
      Confirm,
      Sending,
      Sent,
      Error
    };

    void handleAddressInput(int key);
    void handleAmountInput(int key);
    void handleConfirm(int key);
    void doSend();

    std::shared_ptr<BoltCore::Wallet> m_wallet;
    State m_state = State::EnterAddress;
    std::string m_address;
    std::string m_amountStr;
    uint64_t m_amount = 0;
    std::string m_error;
    std::string m_txHash;
    uint64_t m_fee = 0;
    int m_cursorPos = 0;
  };

} // namespace ClientWallet