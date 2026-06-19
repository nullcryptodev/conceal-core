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

  bool HistoryScreen::matchesFilter(const BoltCore::OutputInfo &tx) const
  {
    if (tx.isDeposit)
      return m_showDeposits;
    if (tx.spent)
      return m_showSpent;
    return m_showReceived;
  }

  void HistoryScreen::applyFilters()
  {
    m_filtered.clear();
    m_filtered.reserve(m_transactions.size());
    for (const auto &tx : m_transactions)
    {
      if (matchesFilter(tx))
        m_filtered.push_back(tx);
    }

    const int maxOffset = std::max(0, static_cast<int>(m_filtered.size()) - VISIBLE_ROWS);
    if (m_scrollOffset > maxOffset)
      m_scrollOffset = maxOffset;
    if (m_selectedRow >= static_cast<int>(m_filtered.size()))
      m_selectedRow = std::max(0, static_cast<int>(m_filtered.size()) - 1);
  }

  void HistoryScreen::onEnter()
  {
    m_transactions = m_wallet->getOutputs();
    std::sort(m_transactions.begin(), m_transactions.end(),
              [](const BoltCore::OutputInfo &a, const BoltCore::OutputInfo &b)
              {
                return a.blockHeight > b.blockHeight;
              });
    m_scrollOffset = 0;
    m_selectedRow = 0;
    applyFilters();
  }

  void HistoryScreen::toggleFilter(bool &flag)
  {
    flag = !flag;
    m_scrollOffset = 0;
    m_selectedRow = 0;
    applyFilters();
  }

  void HistoryScreen::onKey(int key)
  {
    const int maxOffset = std::max(0, static_cast<int>(m_filtered.size()) - VISIBLE_ROWS);

    switch (key)
    {
    case Tui::KEY_UP: // Up
      if (m_selectedRow > 0)
        m_selectedRow--;
      else if (m_scrollOffset > 0)
        m_scrollOffset--;
      break;
    case Tui::KEY_DOWN: // Down
      if (m_selectedRow < VISIBLE_ROWS - 1 &&
          m_selectedRow + m_scrollOffset < static_cast<int>(m_filtered.size()) - 1)
        m_selectedRow++;
      else if (m_scrollOffset < maxOffset)
        m_scrollOffset++;
      break;
    case Tui::KEY_HOME: // Home
      m_scrollOffset = 0;
      m_selectedRow = 0;
      break;
    case Tui::KEY_END: // End
      m_scrollOffset = maxOffset;
      m_selectedRow = std::min(VISIBLE_ROWS - 1,
                               static_cast<int>(m_filtered.size()) - m_scrollOffset - 1);
      break;
    case 'r':
    case 'R':
      toggleFilter(m_showReceived);
      break;
    case 's':
    case 'S':
      toggleFilter(m_showSpent);
      break;
    case 'd':
    case 'D':
      toggleFilter(m_showDeposits);
      break;
    case Tui::KEY_ESC: // Escape
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
    drawHeader(buf, title(), balance.currentHeight, BoltCore::spendableAmountBeforeFee(balance),
               std::to_string(m_filtered.size()) + " shown / " +
                   std::to_string(m_transactions.size()) + " total");

    const int top = 5;

    std::string filters;
    filters += Tui::dim() + " Filters:";
    filters += (m_showReceived ? Tui::green() : Tui::dim()) + " [R]eceived" + Tui::reset();
    filters += (m_showSpent ? Tui::red() : Tui::dim()) + " [S]ent" + Tui::reset();
    filters += (m_showDeposits ? Tui::magenta() : Tui::dim()) + " [D]eposits" + Tui::reset();
    buf.writeAt(top - 1, 2, filters);

    buf.writeAt(top, 2, Tui::bold() + Tui::underline() + " Dir Type      Amount (CCX)       Height    Status    TX Hash" + Tui::reset());

    int visibleCount = 0;
    for (int i = 0; i < VISIBLE_ROWS; ++i)
    {
      const int idx = m_scrollOffset + i;
      const int row = top + 1 + i;
      if (idx >= static_cast<int>(m_filtered.size()))
        break;

      const auto &tx = m_filtered[idx];
      const bool isSelected = (visibleCount == m_selectedRow);

      const std::string color = txColor(tx);
      const std::string dir = txDirection(tx);
      const std::string type = formatTxType(tx);
      const std::string amount = formatAmount(tx.amount);
      const std::string hashStr = common::podToHex(tx.txHash).substr(0, 12);
      const std::string status = tx.spent ? "spent" : (tx.isDeposit ? "locked" : "ok");

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

    if (m_scrollOffset > 0)
      buf.writeAt(top + 1, 0, Tui::dim() + "▲" + Tui::reset());
    if (m_scrollOffset + VISIBLE_ROWS < static_cast<int>(m_filtered.size()))
      buf.writeAt(top + VISIBLE_ROWS, 0, Tui::dim() + "▼" + Tui::reset());

    uint64_t totalRecv = 0, totalSent = 0, totalDep = 0;
    for (const auto &tx : m_filtered)
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
                {"Overview", "Quit"},
                {"Esc", "Q"},
                2);
  }

} // namespace ClientWallet
