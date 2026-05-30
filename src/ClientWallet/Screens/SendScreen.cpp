// SendScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SendScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include <iomanip>
#include <sstream>

namespace ClientWallet
{

  SendScreen::SendScreen(std::shared_ptr<BoltCore::Wallet> wallet)
      : m_wallet(std::move(wallet)) {}

  void SendScreen::onEnter()
  {
    if (m_wallet->getType() == BoltCore::WalletType::ViewOnly)
      return; // Will render the view-only message

    m_state = State::EnterAddress;
    m_address.clear();
    m_amountStr.clear();
    m_amount = 0;
    m_error.clear();
    m_txHash.clear();
    m_fee = 0;
    m_cursorPos = 0;
  }

  void SendScreen::onKey(int key)
  {
    switch (m_state)
    {
    case State::EnterAddress:
      handleAddressInput(key);
      break;
    case State::EnterAmount:
      handleAmountInput(key);
      break;
    case State::Confirm:
      handleConfirm(key);
      break;
    case State::Sending:
      break; // Ignore input while sending
    case State::Sent:
    case State::Error:
      if (key == 13 || key == ' ')
      {            // Enter or Space to go back
        onEnter(); // Reset form
      }
      break;
    }
  }

  void SendScreen::handleAddressInput(int key)
  {
    if (key == 13)
    { // Enter
      if (m_address.empty())
      {
        m_error = "Address cannot be empty";
      }
      else
      {
        m_state = State::EnterAmount;
        m_error.clear();
      }
      return;
    }

    if (key == 27)
    {
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (key == 127 || key == 8)
    { // Backspace
      if (!m_address.empty())
      {
        m_address.pop_back();
        if (m_cursorPos > 0)
          m_cursorPos--;
      }
      return;
    }
    if (key >= 32 && key <= 126)
    { // Printable
      m_address += static_cast<char>(key);
      m_cursorPos++;
    }
  }

  void SendScreen::handleAmountInput(int key)
  {
    if (key == 13)
    { // Enter
      try
      {
        double amountDbl = std::stod(m_amountStr);
        m_amount = static_cast<uint64_t>(amountDbl * 1000000.0);
        if (m_amount == 0)
        {
          m_error = "Amount must be greater than 0";
        }
        else
        {
          auto balance = m_wallet->getBalance();
          if (m_amount > balance.actual)
          {
            m_error = "Insufficient balance";
          }
          else
          {
            m_state = State::Confirm;
            m_error.clear();
          }
        }
      }
      catch (...)
      {
        m_error = "Invalid amount";
      }
      return;
    }

    if (key == 27)
    {
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (key == 127 || key == 8)
    {
      if (!m_amountStr.empty())
        m_amountStr.pop_back();
      return;
    }

    if ((key >= '0' && key <= '9') || key == '.')
    {
      m_amountStr += static_cast<char>(key);
    }
  }

  void SendScreen::handleConfirm(int key)
  {
    if (key == 'y' || key == 'Y' || key == 13)
    {
      doSend();
    }
    else if (key == 'n' || key == 'N' || key == 27)
    {
      onEnter(); // Reset
    }
  }

  void SendScreen::doSend()
  {
    m_state = State::Sending;

    // Call wallet transfer
    auto result = m_wallet->transfer(m_address, m_amount);

    if (result.success)
    {
      m_state = State::Sent;
      m_txHash = result.txHash;
      m_fee = result.fee;
    }
    else
    {
      m_state = State::Error;
      m_error = result.error.empty() ? "Transaction failed" : result.error;
    }
  }

  std::string formatAmount(uint64_t amount);

  void SendScreen::render(Tui::ScreenBuffer &buf)
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

    switch (m_state)
    {
    case State::EnterAddress:
    {
      buf.write(Tui::drawBox(boxTop, 2, 7, boxW, "Enter Address"));
      buf.writeAt(boxTop + 1, 4, Tui::dim() + "Enter the destination address:" + Tui::reset());
      buf.writeAt(boxTop + 3, 4, Tui::brightWhite() + "> " + m_address + (m_address.empty() ? Tui::dim() + "_" + Tui::reset() : "") + Tui::reset());
      buf.writeAt(boxTop + 5, 4, Tui::dim() + "Press Enter to continue, Esc to go back" + Tui::reset());
      break;
    }
    case State::EnterAmount:
    {
      buf.write(Tui::drawBox(boxTop, 2, 8, boxW, "Enter Amount"));
      buf.writeAt(boxTop + 1, 4, Tui::dim() + "To: " + Tui::reset() + m_address.substr(0, 60));
      buf.writeAt(boxTop + 3, 4, Tui::dim() + "Amount (CCX):" + Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::brightWhite() + "> " + m_amountStr + (m_amountStr.empty() ? Tui::dim() + "0.0" + Tui::reset() : "") + Tui::reset());
      break;
    }
    case State::Confirm:
    {
      buf.write(Tui::drawBox(boxTop, 2, 10, boxW, "Confirm Transaction"));
      buf.writeAt(boxTop + 1, 4, Tui::dim() + "To:     " + Tui::reset() + m_address.substr(0, 55));
      buf.writeAt(boxTop + 2, 4, Tui::dim() + "Amount: " + Tui::reset() + Tui::brightGreen() + m_amountStr + " CCX" + Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Available: " + Tui::reset() + formatAmount(balance.actual) + " CCX");
      buf.writeAt(boxTop + 6, 4, Tui::brightYellow() + "Send this transaction? [Y/N]" + Tui::reset());
      break;
    }
    case State::Sending:
    {
      buf.write(Tui::drawBox(boxTop, 2, 5, boxW, "Sending"));
      buf.writeAt(boxTop + 2, 4, Tui::brightYellow() + "Sending transaction..." + Tui::reset());
      break;
    }
    case State::Sent:
    {
      buf.write(Tui::drawBox(boxTop, 2, 8, boxW, "Transaction Sent"));
      buf.writeAt(boxTop + 1, 4, Tui::brightGreen() + "Transaction sent successfully!" + Tui::reset());
      buf.writeAt(boxTop + 3, 4, Tui::dim() + "TX Hash: " + Tui::reset() + m_txHash.substr(0, 50));
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Fee:     " + Tui::reset() + formatAmount(m_fee) + " CCX");
      buf.writeAt(boxTop + 6, 4, Tui::dim() + "Press any key to continue" + Tui::reset());
      break;
    }
    case State::Error:
    {
      buf.write(Tui::drawBox(boxTop, 2, 7, boxW, "Error"));
      buf.writeAt(boxTop + 2, 4, Tui::brightRed() + m_error + Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Press any key to try again" + Tui::reset());
      break;
    }
    }

    if (!m_error.empty() && m_state != State::Error)
    {
      buf.writeAt(boxTop + 8, 4, Tui::red() + m_error + Tui::reset());
    }

    drawMenuBar(buf, {"Back"}, {"Esc"});
  }

} // namespace ClientWallet