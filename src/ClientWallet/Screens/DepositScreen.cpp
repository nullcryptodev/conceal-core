// DepositScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "DepositScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "CryptoNoteCore/Currency.h"
#include <iomanip>
#include <sstream>

namespace ClientWallet
{

  DepositScreen::DepositScreen(std::shared_ptr<BoltCore::Wallet> wallet, const cn::Currency &currency)
      : m_wallet(std::move(wallet)), m_currency(currency) {}

  void DepositScreen::onEnter()
  {
    m_tab = Tab::Create;
    m_state = State::Idle;
    m_selectedMonth = 0;
    m_amountStr.clear();
    m_amount = 0;
    m_selectedRow = 0;
    m_scrollOffset = 0;
    m_error.clear();
    m_txHash.clear();

    // Refresh unlocked deposits
    auto allDeposits = m_wallet->getDeposits();
    m_unlockedDeposits.clear();
    for (const auto &d : allDeposits)
      if (!d.locked)
        m_unlockedDeposits.push_back(d);
  }

  void DepositScreen::onKey(int key)
  {
    if (m_state == State::Done || m_state == State::Error)
    {
      if (key == 13 || key == 10 || key == 27)
        onEnter();
      return;
    }

    // Tab switching
    if (key == 1002 && m_tab == Tab::Create)
    {
      m_tab = Tab::Withdraw;
      m_selectedRow = 0;
      return;
    } // Right
    if (key == 1003 && m_tab == Tab::Withdraw)
    {
      m_tab = Tab::Create;
      return;
    } // Left

    if (key == 27)
    {
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (m_tab == Tab::Create)
      handleCreateKeys(key);
    else
      handleWithdrawKeys(key);
  }

  // ── Create tab ──────────────────────────────────────────────────────────

  void DepositScreen::handleCreateKeys(int key)
  {
    if (m_state == State::Creating)
      return;

    if (m_amountStr.empty() && m_state == State::Idle)
    {
      // Selecting month
      switch (key)
      {
      case 1000:
        if (m_selectedMonth > 0)
          m_selectedMonth--;
        break;
      case 1001:
        if (m_selectedMonth < 11)
          m_selectedMonth++;
        break;
      case 10:
      case 13:
        m_amountStr = "";
        m_state = State::Idle; // ready for amount
        break;
      }
      return;
    }

    // Entering amount or confirming
    if (m_state == State::ConfirmCreate)
    {
      if (key == 'y' || key == 'Y' || key == 13 || key == 10)
        doCreateDeposit();
      else if (key == 'n' || key == 'N')
        onEnter();
      return;
    }

    // Amount entry
    if (key == 13 || key == 10)
    {
      try
      {
        double amt = std::stod(m_amountStr);
        m_amount = static_cast<uint64_t>(amt * 1000000.0);
        if (m_amount < m_currency.depositMinAmount())
          m_error = "Min: " + m_currency.formatAmount(m_currency.depositMinAmount()) + " CCX";
        else if (m_amount > m_wallet->getBalance().actual)
          m_error = "Insufficient balance";
        else
        {
          m_state = State::ConfirmCreate;
          m_error.clear();
        }
      }
      catch (...)
      {
        m_error = "Invalid amount";
      }
      return;
    }
    if (key == 127 || key == 8)
    {
      if (!m_amountStr.empty())
        m_amountStr.pop_back();
      return;
    }
    if ((key >= '0' && key <= '9') || key == '.')
      m_amountStr += static_cast<char>(key);
  }

  void DepositScreen::doCreateDeposit()
  {
    m_state = State::Creating;
    uint32_t termBlocks = (m_selectedMonth + 1) * m_currency.depositMinTermV3();
    auto result = m_wallet->createDeposit(m_amount, termBlocks);
    if (result.success)
    {
      m_state = State::Done;
      m_txHash = result.txHash;
    }
    else
    {
      m_state = State::Error;
      m_error = result.error.empty() ? "Failed" : result.error;
    }
  }

  uint64_t DepositScreen::estimateInterest() const
  {
    if (m_amount == 0)
      return 0;
    uint32_t termBlocks = (m_selectedMonth + 1) * m_currency.depositMinTermV3();
    return m_currency.calculateInterest(m_amount, termBlocks, 0);
  }

  // ── Withdraw tab ────────────────────────────────────────────────────────

  void DepositScreen::handleWithdrawKeys(int key)
  {
    if (m_state == State::Withdrawing)
      return;

    if (m_state == State::ConfirmWithdraw)
    {
      if (key == 'y' || key == 'Y' || key == 13 || key == 10)
        doWithdrawDeposit();
      else if (key == 'n' || key == 'N')
      {
        m_state = State::Idle;
      }
      return;
    }

    int maxOffset = std::max(0, static_cast<int>(m_unlockedDeposits.size()) - VISIBLE_ROWS);
    switch (key)
    {
    case 1000:
      if (m_selectedRow > 0)
        m_selectedRow--;
      else if (m_scrollOffset > 0)
        m_scrollOffset--;
      break;
    case 1001:
      if (m_selectedRow < VISIBLE_ROWS - 1 &&
          m_selectedRow + m_scrollOffset < static_cast<int>(m_unlockedDeposits.size()) - 1)
        m_selectedRow++;
      else if (m_scrollOffset < maxOffset)
        m_scrollOffset++;
      break;
    case 10:
    case 13:
      if (!m_unlockedDeposits.empty())
      {
        int idx = m_scrollOffset + m_selectedRow;
        if (idx < static_cast<int>(m_unlockedDeposits.size()))
        {
          m_selectedDepositId = m_unlockedDeposits[idx].id;
          m_state = State::ConfirmWithdraw;
        }
      }
      break;
    }
  }

  void DepositScreen::doWithdrawDeposit()
  {
    m_state = State::Withdrawing;
    auto result = m_wallet->withdrawDeposit(m_selectedDepositId);
    if (result.success)
    {
      m_state = State::Done;
      m_txHash = result.txHash;
    }
    else
    {
      m_state = State::Error;
      m_error = result.error.empty() ? "Failed" : result.error;
    }
  }

  // ── Render ──────────────────────────────────────────────────────────────

  std::string formatAmount(uint64_t amount);

  void DepositScreen::render(Tui::ScreenBuffer &buf)
  {
    if (m_wallet->getType() == BoltCore::WalletType::ViewOnly)
    {
      buf.write(Tui::drawBox(5, 2, 6, 70, "View-Only Wallet"));
      buf.writeAt(7, 4, Tui::brightYellow() + "This feature requires a spend key." + Tui::reset());
      buf.writeAt(8, 4, Tui::dim() + "Import a full wallet or connect with a spend key to send funds." + Tui::reset());
      drawMenuBar(buf, {"Back"}, {"Esc"});
      return;
    }

    auto balance = m_wallet->getBalance();
    drawHeader(buf, title(), balance.currentHeight, balance.actual, "");

    int termW = Tui::terminalWidth();
    int boxW = std::min(70, termW - 4);
    int boxTop = 5;

    // ── Tab bar ──────────────────────────────────────────────────────
    std::string createTab = (m_tab == Tab::Create ? Tui::bold() + Tui::brightWhite() + "[Create]" + Tui::reset()
                                                  : Tui::dim() + " Create " + Tui::reset());
    std::string withdrawTab = (m_tab == Tab::Withdraw ? Tui::bold() + Tui::brightWhite() + "[Withdraw]" + Tui::reset()
                                                      : Tui::dim() + " Withdraw " + Tui::reset());
    buf.writeAt(boxTop - 1, 4, createTab + "  " + withdrawTab + Tui::dim() + "  (← → to switch tabs)" + Tui::reset());

    // ── State overlays ───────────────────────────────────────────────
    if (m_state == State::Done)
    {
      buf.write(Tui::drawBox(boxTop, 2, 6, boxW, "Success"));
      buf.writeAt(boxTop + 2, 4, Tui::brightGreen() + "Transaction sent!" + Tui::reset());
      buf.writeAt(boxTop + 3, 4, Tui::dim() + "TX: " + m_txHash.substr(0, 50) + Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Press any key to continue" + Tui::reset());
      drawMenuBar(buf, {"Back"}, {"Esc"});
      return;
    }
    if (m_state == State::Error)
    {
      buf.write(Tui::drawBox(boxTop, 2, 6, boxW, "Error"));
      buf.writeAt(boxTop + 2, 4, Tui::brightRed() + m_error + Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Press any key to try again" + Tui::reset());
      drawMenuBar(buf, {"Back"}, {"Esc"});
      return;
    }
    if (m_state == State::Creating || m_state == State::Withdrawing)
    {
      buf.write(Tui::drawBox(boxTop, 2, 5, boxW, "Processing"));
      buf.writeAt(boxTop + 2, 4, Tui::brightYellow() + "Creating transaction..." + Tui::reset());
      drawMenuBar(buf, {"Back"}, {"Esc"});
      return;
    }

    // ── Create tab ───────────────────────────────────────────────────
    if (m_tab == Tab::Create)
    {
      buf.write(Tui::drawBox(boxTop, 2, 15, boxW, "Create New Deposit"));

      // Month selector
      for (int i = 0; i < 12; ++i)
      {
        std::string label = std::to_string(i + 1) + " month" + (i == 0 ? " " : "s");
        while (label.size() < 12)
          label += " ";
        std::string line = (i == m_selectedMonth ? Tui::bold() + Tui::brightWhite() + "  > " + label + Tui::reset()
                                                 : Tui::dim() + "    " + label + Tui::reset());
        buf.writeAt(boxTop + 1 + i, 4, line);
      }

      // Amount entry
      int amtRow = boxTop + 14;
      if (!m_amountStr.empty() || m_state == State::ConfirmCreate)
      {
        buf.writeAt(amtRow, 4, Tui::dim() + "Amount: " + Tui::reset() + Tui::brightWhite() + m_amountStr + " CCX" + Tui::reset());
        if (m_state == State::ConfirmCreate)
        {
          uint64_t interest = estimateInterest();
          buf.writeAt(amtRow + 1, 4, Tui::dim() + "Interest: " + Tui::reset() + Tui::cyan() + formatAmount(interest) + " CCX" + Tui::reset());
          buf.writeAt(amtRow + 2, 4, Tui::dim() + "Total return: " + Tui::reset() + Tui::brightGreen() + formatAmount(m_amount + interest) + " CCX" + Tui::reset());
          buf.writeAt(amtRow + 3, 4, Tui::brightYellow() + "Confirm? [Y/N]" + Tui::reset());
        }
      }
      else
      {
        buf.writeAt(amtRow, 4, Tui::dim() + "Select a term above, then type amount + Enter" + Tui::reset());
      }
    }

    // ── Withdraw tab ─────────────────────────────────────────────────
    if (m_tab == Tab::Withdraw)
    {
      buf.write(Tui::drawBox(boxTop, 2, std::min(VISIBLE_ROWS + 4, 16), boxW,
                             "Unlocked Deposits (" + std::to_string(m_unlockedDeposits.size()) + ")"));

      if (m_unlockedDeposits.empty())
      {
        buf.writeAt(boxTop + 2, 4, Tui::dim() + "No unlocked deposits" + Tui::reset());
      }
      else
      {
        buf.writeAt(boxTop + 1, 4, Tui::bold() + Tui::underline() + " ID   Amount (CCX)       Interest      Term" + Tui::reset());

        for (int i = 0; i < VISIBLE_ROWS; ++i)
        {
          int idx = m_scrollOffset + i;
          if (idx >= static_cast<int>(m_unlockedDeposits.size()))
            break;
          const auto &d = m_unlockedDeposits[idx];
          bool sel = (i == m_selectedRow && m_state == State::Idle);

          std::ostringstream line;
          line << (sel ? Tui::bold() + Tui::brightWhite() : "")
               << " " << std::setw(3) << d.id
               << "  " << std::setw(14) << formatAmount(d.amount)
               << "  " << std::setw(12) << formatAmount(d.interest)
               << "  " << (d.term / 64800) << "mo"
               << Tui::reset();
          buf.writeAt(boxTop + 2 + i, 4, line.str());
        }
      }

      if (m_state == State::ConfirmWithdraw)
      {
        int row = boxTop + VISIBLE_ROWS + 3;
        // Find the deposit
        for (const auto &d : m_unlockedDeposits)
        {
          if (d.id == m_selectedDepositId)
          {
            buf.writeAt(row, 4, Tui::brightYellow() + "Withdraw " + formatAmount(d.amount + d.interest) + " CCX? [Y/N]" + Tui::reset());
            break;
          }
        }
      }
    }

    if (!m_error.empty())
      buf.writeAt(boxTop + 14, 4, Tui::red() + m_error + Tui::reset());

    drawMenuBar(buf, {"Back"}, {"Esc"});
  }

} // namespace ClientWallet