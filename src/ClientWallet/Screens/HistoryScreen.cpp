// HistoryScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "HistoryScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "Common/StringTools.h"
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace ClientWallet
{

  HistoryScreen::HistoryScreen(std::shared_ptr<BoltCore::Wallet> wallet)
      : m_wallet(std::move(wallet)) {}

  void HistoryScreen::onEnter()
  {
    m_transactions = m_wallet->getOutputs();
    // Sort by block height descending (newest first)
    std::sort(m_transactions.begin(), m_transactions.end(),
              [](const BoltCore::OutputInfo &a, const BoltCore::OutputInfo &b)
              {
                return a.blockHeight > b.blockHeight;
              });
    m_scrollOffset = 0;
    m_selectedRow = 0;
  }

  void HistoryScreen::onKey(int key)
  {
    int maxOffset = std::max(0, static_cast<int>(m_transactions.size()) - VISIBLE_ROWS);

    switch (key)
    {
    case 1000: // Up
      if (m_selectedRow > 0)
        m_selectedRow--;
      else if (m_scrollOffset > 0)
        m_scrollOffset--;
      break;
    case 1001: // Down
      if (m_selectedRow < VISIBLE_ROWS - 1 &&
          m_selectedRow + m_scrollOffset < static_cast<int>(m_transactions.size()) - 1)
        m_selectedRow++;
      else if (m_scrollOffset < maxOffset)
        m_scrollOffset++;
      break;
    case 1004: // Home
      m_scrollOffset = 0;
      m_selectedRow = 0;
      break;
    case 1005: // End
      m_scrollOffset = maxOffset;
      m_selectedRow = std::min(VISIBLE_ROWS - 1,
                               static_cast<int>(m_transactions.size()) - m_scrollOffset - 1);
      break;
    case 'r':
      m_showReceived = !m_showReceived;
      onEnter();
      break;
    case 's':
      m_showSpent = !m_showSpent;
      onEnter();
      break;
    case 'd':
      m_showDeposits = !m_showDeposits;
      onEnter();
      break;
    case 27: // Escape
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      break;
    }
  }

  std::string formatAmount(uint64_t amount);

  std::string formatTxType(const BoltCore::OutputInfo &tx)
  {
    if (tx.isDeposit)
      return "Deposit ";
    if (tx.spent)
      return "Sent   ";
    return "Received";
  }

  std::string txDirection(const BoltCore::OutputInfo &tx)
  {
    if (tx.isDeposit)
      return "D";
    if (tx.spent)
      return "↑";
    return "↓";
  }

  std::string txColor(const BoltCore::OutputInfo &tx)
  {
    if (tx.isDeposit)
    {
      if (tx.spent)
        return Tui::dim() + Tui::magenta();
      return Tui::magenta();
    }
    if (tx.spent)
      return Tui::red();
    return Tui::green();
  }

  void HistoryScreen::render(Tui::ScreenBuffer &buf)
  {
    auto balance = m_wallet->getBalance();
    drawHeader(buf, title(), balance.currentHeight, balance.actual,
               std::to_string(m_transactions.size()) + " total | " +
                   "R:received S:sent D:deposits");

    // Column headers
    int top = 5;
    buf.writeAt(top, 2, Tui::bold() + Tui::underline() + " Dir Type      Amount (CCX)       Height    Status    TX Hash" + Tui::reset());

    // Filter toggles
    std::string filters;
    filters += Tui::dim() + "Filters: ";
    filters += (m_showReceived ? Tui::green() : Tui::dim()) + "[R]eceived " + Tui::reset();
    filters += (m_showSpent ? Tui::red() + "[S]ent " : Tui::dim() + "[S]ent ") + Tui::reset();
    filters += (m_showDeposits ? Tui::magenta() + "[D]eposits" : Tui::dim() + "[D]eposits") + Tui::reset();
    buf.writeAt(top - 1, 50, filters);

    // Transaction rows
    int visibleCount = 0;
    for (int i = 0; i < VISIBLE_ROWS; ++i)
    {
      int idx = m_scrollOffset + i;
      int row = top + 1 + i;

      // Find next visible transaction
      while (idx < static_cast<int>(m_transactions.size()))
      {
        const auto &tx = m_transactions[idx];
        if ((tx.isDeposit && m_showDeposits) ||
            (tx.spent && m_showSpent) ||
            (!tx.isDeposit && !tx.spent && m_showReceived))
          break;
        idx++;
      }

      if (idx >= static_cast<int>(m_transactions.size()))
        break;

      const auto &tx = m_transactions[idx];
      bool isSelected = (visibleCount == m_selectedRow);

      std::string color = txColor(tx);
      std::string dir = txDirection(tx);
      std::string type = formatTxType(tx);
      std::string amount = formatAmount(tx.amount);
      std::string hashStr = common::podToHex(tx.txHash).substr(0, 12);
      std::string status = tx.spent ? "spent" : (tx.isDeposit ? "locked" : "ok");

      std::ostringstream line;
      line << (isSelected ? Tui::bold() + Tui::brightWhite() : "")
           << " " << color << dir << Tui::reset()
           << "  " << std::setw(8) << type
           << "  " << std::setw(14) << amount
           << "  " << std::setw(8) << tx.blockHeight
           << "  " << std::setw(8) << status
           << "  " << hashStr
           << Tui::reset();

      buf.writeAt(row, 2, line.str());
      visibleCount++;
    }

    // Scroll indicators
    if (m_scrollOffset > 0)
      buf.writeAt(top + 1, 0, Tui::dim() + "▲" + Tui::reset());
    if (m_scrollOffset + VISIBLE_ROWS < static_cast<int>(m_transactions.size()))
      buf.writeAt(top + VISIBLE_ROWS, 0, Tui::dim() + "▼" + Tui::reset());

    // Summary at bottom
    uint64_t totalRecv = 0, totalSent = 0, totalDep = 0;
    for (const auto &tx : m_transactions)
    {
      if (tx.isDeposit)
        totalDep += tx.amount;
      else if (tx.spent)
        totalSent += tx.amount;
      else
        totalRecv += tx.amount;
    }

    std::ostringstream summary;
    summary << Tui::green() << "Recv: " << formatAmount(totalRecv) << Tui::reset()
            << "  " << Tui::red() << "Sent: " << formatAmount(totalSent) << Tui::reset()
            << "  " << Tui::magenta() << "Deposits: " << formatAmount(totalDep) << Tui::reset();
    buf.writeAt(top + VISIBLE_ROWS + 2, 2, summary.str());

    drawMenuBar(buf,
                {"Overview", "Filters", "Quit"},
                {"Esc", "R/S/D", "Q"});
  }

} // namespace ClientWallet