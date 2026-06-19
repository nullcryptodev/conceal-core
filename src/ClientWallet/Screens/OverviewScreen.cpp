// OverviewScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "OverviewScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "BoltCore/OutputUtils.h"
#include <algorithm>
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
    case 'f':
    case 'F':
      if (!isViewOnly && m_onAction)
        m_onAction(ScreenAction::GoToFusion);
      break;
    case 'q':
    case 'Q':
      if (m_onAction)
        m_onAction(ScreenAction::Quit);
      break;
    case Tui::KEY_UP:
      if (m_selectedRow > 0)
        m_selectedRow--;
      break;
    case Tui::KEY_DOWN:
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
    auto outputs = m_wallet->getOutputs();

    uint32_t lockedCount = 0;
    uint32_t withdrawableCount = 0;
    uint32_t unlockedSpentCount = 0;
    uint64_t withdrawableAmount = 0;
    uint64_t unlockedSpentAmount = 0;
    for (const auto &out : outputs)
    {
      if (!out.isDeposit)
        continue;

      if (out.spent)
      {
        unlockedSpentAmount += out.amount;
        unlockedSpentCount++;
      }
      else if (BoltCore::isDepositUnlocked(out, balance.currentHeight))
      {
        withdrawableAmount += out.amount;
        withdrawableCount++;
      }
      else
      {
        lockedCount++;
      }
    }

    uint64_t available = BoltCore::spendableAmountBeforeFee(balance);

    drawHeader(buf, title(), balance.currentHeight, available,
               m_wallet->getType() == BoltCore::WalletType::Full ? "Full" : "View-only");

    // ── Balance box ──────────────────────────────────────────────────
    const int balanceTop = 5;
    const int balanceH = 12;
    buf.write(Tui::drawBox(balanceTop, 2, balanceH, 76, "Balance"));

    buf.writeAt(balanceTop + 1, 4, Tui::dim() + "Available:      " + Tui::reset() + Tui::accentGreen() + formatAmount(available) + " CCX" + Tui::reset());
    buf.writeAt(balanceTop + 2, 4, Tui::dim() + "Pending:        " + Tui::reset() + Tui::yellow() + formatAmount(balance.pending) + " CCX" + Tui::reset());
    buf.writeAt(balanceTop + 3, 4, Tui::dim() + "Locked Deposit: " + Tui::reset() + Tui::red() + formatAmount(balance.lockedDeposit) + " CCX" + Tui::reset() + " (" + std::to_string(lockedCount) + " locked)");
    buf.writeAt(balanceTop + 4, 4, Tui::dim() + "Withdrawable:   " + Tui::reset() + Tui::green() + formatAmount(withdrawableAmount) + " CCX" + Tui::reset() + " (" + std::to_string(withdrawableCount) + " ready)");
    buf.writeAt(balanceTop + 5, 4, Tui::dim() + "Unl. & Spent:   " + Tui::reset() + Tui::dim() + formatAmount(unlockedSpentAmount) + " CCX" + Tui::reset() + Tui::dim() + " (" + std::to_string(unlockedSpentCount) + " withdrawn)" + Tui::reset());
    buf.writeAt(balanceTop + 6, 4, Tui::dim() + "Accrued Int:    " + Tui::reset() + Tui::cyan() + formatAmount(balance.accruedInterest) + " CCX" + Tui::reset());
    buf.writeAt(balanceTop + 7, 4, Tui::dim() + "Dust:           " + Tui::reset() + Tui::brightYellow() + formatAmount(balance.dust) + " CCX" + Tui::reset() + Tui::dim() + "  (un-mixable)" + Tui::reset());

    // Total = spendable + locked + withdrawable + dust (excludes withdrawn deposits).
    uint64_t total = available + balance.lockedDeposit + withdrawableAmount + balance.dust;
    buf.writeAt(balanceTop + 9, 4, Tui::bold() + "Total:          " + Tui::brightWhite() + formatAmount(total) + " CCX" + Tui::reset());

    // ── Layout: Balance → Recent Activity → Last Log → menu bar ───────
    const int menuRow = Tui::terminalHeight() - 1;
    const int txTop = balanceTop + balanceH;
    const int availRows = menuRow - txTop;

    size_t logLineCount = 6;
    int logBoxH = static_cast<int>(logLineCount) + 2;
    constexpr int minTxBoxH = 4;
    constexpr int minLogBoxH = 4;

    if (availRows >= minTxBoxH + minLogBoxH)
    {
      if (logBoxH > availRows - minTxBoxH)
      {
        logLineCount = 2;
        logBoxH = static_cast<int>(logLineCount) + 2;
      }
    }
    else
    {
      logLineCount = 0;
      logBoxH = 0;
    }

    int txBoxH = availRows - logBoxH;
    if (txBoxH < minTxBoxH)
    {
      logLineCount = 0;
      logBoxH = 0;
      txBoxH = availRows;
    }

    const int logTop = menuRow - logBoxH;
    const int maxTxRows = std::max(0, txBoxH - 2);

    // ── Recent transactions ───────────────────────────────────────────
    if (txBoxH >= 3)
    {
      buf.write(Tui::drawBox(txTop, 2, txBoxH, 76, "Recent Activity"));

      size_t count = 0;
      int row = txTop + 1;

      std::sort(outputs.begin(), outputs.end(),
                [](const BoltCore::OutputInfo &a, const BoltCore::OutputInfo &b)
                {
                  return a.blockHeight > b.blockHeight;
                });

      for (const auto &tx : outputs)
      {
        if (count >= static_cast<size_t>(maxTxRows))
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
    }

    // ── Sync log tail ─────────────────────────────────────────────────
    if (logBoxH > 0 && logTop >= txTop + txBoxH)
      drawLastSyncLog(buf, logTop, logLineCount);

    // ── Menu bar ──────────────────────────────────────────────────────
    bool isViewOnly = m_wallet->getType() == BoltCore::WalletType::ViewOnly;

    // Menu bar
    if (isViewOnly)
      drawMenuBar(buf, {"Receive", "History", "Quit"}, {"R", "H", "Q"}, 3);
    else
      drawMenuBar(buf, {"Send", "Receive", "History", "Deposit", "Fusion", "Quit"}, {"S", "R", "H", "D", "F", "Q"}, 3);
  }

} // namespace ClientWallet