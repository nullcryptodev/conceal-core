// DepositScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "DepositScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "CryptoNoteCore/Currency.h"
#include "CryptoNoteConfig.h"
#include <chrono>
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
      if (key == Tui::KEY_ENTER || key == Tui::KEY_LF || key == Tui::KEY_ESC)
        onEnter();
      return;
    }

    // Tab switching
    if (key == Tui::KEY_RIGHT && m_tab == Tab::Create)
    {
      m_tab = Tab::Withdraw;
      m_selectedRow = 0;
      return;
    } // Right
    if (key == Tui::KEY_LEFT && m_tab == Tab::Withdraw)
    {
      m_tab = Tab::Create;
      return;
    } // Left

    if (m_state == State::Creating || m_state == State::Withdrawing)
      return;

    if (key == Tui::KEY_ESC)
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

    if (m_state == State::Idle)
    {
      switch (key)
      {
      case Tui::KEY_UP:
        if (m_selectedMonth > 0)
          m_selectedMonth--;
        break;
      case Tui::KEY_DOWN:
        if (m_selectedMonth < 11)
          m_selectedMonth++;
        break;
      case Tui::KEY_LF:
      case Tui::KEY_ENTER:
        m_amountStr.clear();
        m_state = State::EnterAmount;
        m_error.clear();
        break;
      }
      return;
    }

    if (m_state == State::ConfirmCreate)
    {
      if (key == 'y' || key == 'Y' || key == Tui::KEY_ENTER || key == Tui::KEY_LF)
        doCreateDeposit();
      else if (key == 'n' || key == 'N')
        onEnter();
      return;
    }

    if (m_state != State::EnterAmount)
      return;

    // Amount entry
    if (key == Tui::KEY_ENTER || key == Tui::KEY_LF)
    {
      try
      {
        double amt = std::stod(m_amountStr);
        m_amount = static_cast<uint64_t>(amt * 1000000.0);
        if (m_amount < m_currency.depositMinAmount())
          m_error = "Min: " + m_currency.formatAmount(m_currency.depositMinAmount()) + " CCX";
        else if (m_amount + m_currency.minimumFeeV2() >
                 BoltCore::spendableAmountBeforeFee(m_wallet->getBalance()))
          m_error = "Insufficient balance (need amount + 0.001 CCX fee)";
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
    if (key == Tui::KEY_BACKSPACE || key == Tui::KEY_DEL)
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
    if (!m_submitWalletTask)
    {
      m_state = State::Error;
      m_error = "Internal error: wallet task runner not available";
      return;
    }

    m_state = State::Creating;
    m_error.clear();
    m_taskStarted = std::chrono::steady_clock::now();

    const uint64_t amount = m_amount;
    const uint32_t termBlocks =
        static_cast<uint32_t>(m_selectedMonth + 1) * cn::parameters::DEPOSIT_MIN_TERM_V3;
    auto wallet = m_wallet;

    m_submitWalletTask(
        [wallet, amount, termBlocks]()
        { return wallet->createDeposit(amount, termBlocks); },
        [this](BoltCore::TransferResult result)
        {
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
        });
  }

  uint64_t DepositScreen::estimateInterest() const
  {
    if (m_amount == 0)
      return 0;
    uint32_t termBlocks =
        static_cast<uint32_t>(m_selectedMonth + 1) * cn::parameters::DEPOSIT_MIN_TERM_V3;
    return m_currency.calculateInterest(m_amount, termBlocks, m_wallet->getCurrentHeight());
  }

  // ── Withdraw tab ────────────────────────────────────────────────────────

  void DepositScreen::handleWithdrawKeys(int key)
  {
    if (m_state == State::Withdrawing)
      return;

    if (m_state == State::ConfirmWithdraw)
    {
      if (key == 'y' || key == 'Y' || key == Tui::KEY_ENTER || key == Tui::KEY_LF)
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
    case Tui::KEY_UP:
      if (m_selectedRow > 0)
        m_selectedRow--;
      else if (m_scrollOffset > 0)
        m_scrollOffset--;
      break;
    case Tui::KEY_DOWN:
      if (m_selectedRow < VISIBLE_ROWS - 1 &&
          m_selectedRow + m_scrollOffset < static_cast<int>(m_unlockedDeposits.size()) - 1)
        m_selectedRow++;
      else if (m_scrollOffset < maxOffset)
        m_scrollOffset++;
      break;
    case Tui::KEY_LF:
    case Tui::KEY_ENTER:
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
    if (!m_submitWalletTask)
    {
      m_state = State::Error;
      m_error = "Internal error: wallet task runner not available";
      return;
    }

    m_state = State::Withdrawing;
    m_error.clear();
    m_taskStarted = std::chrono::steady_clock::now();

    const uint64_t depositId = m_selectedDepositId;
    auto wallet = m_wallet;

    m_submitWalletTask(
        [wallet, depositId]()
        { return wallet->withdrawDeposit(depositId); },
        [this](BoltCore::TransferResult result)
        {
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
        });
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
    drawHeader(buf, title(), balance.currentHeight, BoltCore::spendableAmountBeforeFee(balance), "");

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
      buf.write(Tui::drawBox(boxTop, 2, 8, boxW, "Processing"));
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - m_taskStarted);
      const char *verb = m_state == State::Creating ? "Creating deposit" : "Withdrawing deposit";
      buf.writeAt(boxTop + 2, 4, Tui::brightYellow() + std::string(verb) + "... " +
                                    std::to_string(elapsed.count()) + "s" + Tui::reset());
      buf.writeAt(boxTop + 3, 4, Tui::dim() +
                                    "Contacting daemon for mixin + relay (port 16000, up to ~2 min)" +
                                    Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Esc to go back (transaction continues in background)" +
                                    Tui::reset());
      drawMenuBar(buf, {"Back"}, {"Esc"});
      return;
    }

    // ── Create tab ───────────────────────────────────────────────────
    if (m_tab == Tab::Create)
    {
      buf.write(Tui::drawBox(boxTop, 2, 20, boxW, "Create New Deposit"));

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
      if (m_state == State::EnterAmount || !m_amountStr.empty() || m_state == State::ConfirmCreate)
      {
        buf.writeAt(amtRow, 4, Tui::dim() + "Amount: " + Tui::reset() + Tui::brightWhite() + m_amountStr +
                                    (m_state == State::EnterAmount ? "_" : "") + " CCX" + Tui::reset());
        if (m_state == State::ConfirmCreate)
        {
          const uint32_t termBlocks =
              static_cast<uint32_t>(m_selectedMonth + 1) * cn::parameters::DEPOSIT_MIN_TERM_V3;
          uint64_t interest = estimateInterest();
          buf.writeAt(amtRow + 1, 4, Tui::dim() + "Term: " + Tui::reset() +
                                          std::to_string(m_selectedMonth + 1) + " month(s) (" +
                                          std::to_string(termBlocks) + " blocks)" + Tui::reset());
          buf.writeAt(amtRow + 2, 4, Tui::dim() + "Fee: " + Tui::reset() +
                                          formatAmount(m_currency.minimumFeeV2()) + " CCX" + Tui::reset());
          buf.writeAt(amtRow + 3, 4, Tui::dim() + "Interest: " + Tui::reset() + Tui::cyan() +
                                          formatAmount(interest) + " CCX" + Tui::reset());
          buf.writeAt(amtRow + 4, 4, Tui::dim() + "Total return: " + Tui::reset() + Tui::brightGreen() +
                                          formatAmount(m_amount + interest) + " CCX" + Tui::reset());
          buf.writeAt(amtRow + 5, 4, Tui::brightYellow() + "Confirm? [Y/N]" + Tui::reset());
        }
      }
      else
      {
        buf.writeAt(amtRow, 4, Tui::dim() + "↑↓ term, Enter to confirm term, then type amount" + Tui::reset());
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
               << "  " << (d.term / cn::parameters::DEPOSIT_MIN_TERM_V3) << "mo"
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