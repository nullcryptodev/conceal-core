// DepositScreen - create and withdraw timed deposits
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include "Screen.h"
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace BoltCore
{
  class Wallet;
  struct DepositInfo;
}
namespace cn
{
  class Currency;
}

namespace ClientWallet
{

  class DepositScreen : public Screen
  {
  public:
    DepositScreen(std::shared_ptr<BoltCore::Wallet> wallet, const cn::Currency &currency);

    void onEnter() override;
    void onKey(int key) override;
    void render(Tui::ScreenBuffer &buf) override;
    std::string title() const override { return "Deposits"; }

  private:
    enum class Tab
    {
      Create,
      Withdraw
    };
    enum class State
    {
      Idle,
      EnterAmount,
      ConfirmCreate,
      Creating,
      ConfirmWithdraw,
      Withdrawing,
      Done,
      Error
    };

    void handleCreateKeys(int key);
    void handleWithdrawKeys(int key);
    void doCreateDeposit();
    void doWithdrawDeposit();
    uint64_t estimateInterest() const;

    std::shared_ptr<BoltCore::Wallet> m_wallet;
    const cn::Currency &m_currency;

    Tab m_tab = Tab::Create;
    State m_state = State::Idle;

    // Create fields
    int m_selectedMonth = 0;
    std::string m_amountStr;
    uint64_t m_amount = 0;

    // Withdraw fields
    std::vector<BoltCore::DepositInfo> m_unlockedDeposits;
    int m_selectedRow = 0;
    int m_scrollOffset = 0;
    uint64_t m_selectedDepositId = 0;

    // Result
    std::string m_error;
    std::string m_txHash;
    std::chrono::steady_clock::time_point m_taskStarted{};

    static constexpr int VISIBLE_ROWS = 8;
  };

} // namespace ClientWallet