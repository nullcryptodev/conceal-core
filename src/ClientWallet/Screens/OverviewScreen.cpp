// OverviewScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "OverviewScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include <iomanip>
#include <sstream>

namespace ClientWallet
{

  OverviewScreen::OverviewScreen(std::shared_ptr<BoltCore::Wallet> wallet)
      : m_wallet(std::move(wallet)) {}

  void OverviewScreen::onEnter() { m_selectedRow = 0; }

  void OverviewScreen::onKey(int key)
  {
    bool isViewOnly = m_wallet->getType() == BoltCore::WalletType::ViewOnly;

    switch (key)
    {
    case 's':
    case 'S':
      if (!isViewOnly && m_onAction)
        m_onAction(ScreenAction::GoToSend);
      break;
    case 'r':
    case 'R':
      if (m_onAction)
        m_onAction(ScreenAction::GoToReceive);
      break;
    case 'h':
    case 'H':
      if (m_onAction)
        m_onAction(ScreenAction::GoToHistory);
      break;
    case 'd':
    case 'D':
      if (!isViewOnly && m_onAction)
        m_onAction(ScreenAction::GoToDeposit);
      break;
    case 'q':
    case 'Q':
      if (m_onAction)
        m_onAction(ScreenAction::Quit);
      break;
    case 27:
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      break;
    case 1000:
      if (m_selectedRow > 0)
        m_selectedRow--;
      break;
    case 1001:
      m_selectedRow++;
      break;
    }
  }

  std::string formatAmount(uint64_t amount)
  {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << (amount / 1000000.0);
    return oss.str();
  }

  void OverviewScreen::render(Tui::ScreenBuffer &buf)
  {
    auto balance = m_wallet->getBalance();
    auto deposits = m_wallet->getDeposits();

    // Count locked vs unlocked deposits
    uint32_t lockedCount = 0, unlockedCount = 0;
    uint64_t totalDepositAmount = 0;
    for (const auto &d : deposits)
    {
      if (d.locked)
        lockedCount++;
      else
        unlockedCount++;
      totalDepositAmount += d.amount;
    }

    drawHeader(buf, title(), balance.currentHeight, balance.actual,
               m_wallet->getType() == BoltCore::WalletType::Full ? "Full" : "View-only");

    // ── Balance box ──────────────────────────────────────────────────
    int boxTop = 5;
    buf.write(Tui::drawBox(boxTop, 2, 10, 76, "Balance"));

    buf.writeAt(boxTop + 1, 4, Tui::dim() + "Available:      " + Tui::reset() + Tui::brightGreen() + formatAmount(balance.actual) + " CCX" + Tui::reset());
    buf.writeAt(boxTop + 2, 4, Tui::dim() + "Pending:        " + Tui::reset() + Tui::yellow() + formatAmount(balance.pending) + " CCX" + Tui::reset());
    buf.writeAt(boxTop + 3, 4, Tui::dim() + "Locked Deposit: " + Tui::reset() + Tui::red() + formatAmount(balance.lockedDeposit) + " CCX" + Tui::reset() + " (" + std::to_string(lockedCount) + " locked)");
    buf.writeAt(boxTop + 4, 4, Tui::dim() + "Unlocked Dep:   " + Tui::reset() + Tui::green() + formatAmount(balance.unlockedDeposit) + " CCX" + Tui::reset() + " (" + std::to_string(unlockedCount) + " ready)");
    buf.writeAt(boxTop + 5, 4, Tui::dim() + "Accrued Int:    " + Tui::reset() + Tui::cyan() + formatAmount(balance.accruedInterest) + " CCX" + Tui::reset());

    // Total line
    uint64_t total = balance.actual + balance.lockedDeposit + balance.unlockedDeposit;
    buf.writeAt(boxTop + 7, 4, Tui::bold() + "Total:          " + Tui::brightWhite() + formatAmount(total) + " CCX" + Tui::reset());

    // ── Recent transactions ───────────────────────────────────────────
    int txTop = boxTop + 11;
    buf.write(Tui::drawBox(txTop, 2, 12, 76, "Recent Activity"));

    auto outputs = m_wallet->getOutputs();
    size_t count = 0;
    int row = txTop + 1;

    // Sort by block height descending, show last 10
    std::sort(outputs.begin(), outputs.end(),
              [](const BoltCore::OutputInfo &a, const BoltCore::OutputInfo &b)
              {
                return a.blockHeight > b.blockHeight;
              });

    for (const auto &tx : outputs)
    {
      if (count >= 10)
        break;

      std::string direction;
      std::string color;
      if (tx.isDeposit)
      {
        direction = "D";
        color = tx.spent ? Tui::dim() + Tui::magenta() : Tui::magenta();
      }
      else if (tx.spent)
      {
        direction = "↑";
        color = Tui::red();
      }
      else
      {
        direction = "↓";
        color = Tui::green();
      }

      std::string amount = formatAmount(tx.amount);
      std::string status = tx.spent ? "spent" : (tx.isDeposit ? "deposit" : "recv");

      std::ostringstream line;
      line << " " << color << direction << " " << amount << " CCX" << Tui::reset()
           << Tui::dim() << "  " << status
           << "  (block " << tx.blockHeight << ")" << Tui::reset();

      buf.writeAt(row, 4, line.str());
      row++;
      count++;
    }

    // ── Menu bar ──────────────────────────────────────────────────────
    bool isViewOnly = m_wallet->getType() == BoltCore::WalletType::ViewOnly;

    // Menu bar
    if (isViewOnly)
      drawMenuBar(buf, {"Receive", "History", "Quit"}, {"R", "H", "Q"});
    else
      drawMenuBar(buf, {"Send", "Receive", "History", "Deposit", "Quit"}, {"S", "R", "H", "D", "Q"});
  }

} // namespace ClientWallet