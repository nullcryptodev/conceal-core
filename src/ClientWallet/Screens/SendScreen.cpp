// SendScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "SendScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "CryptoNoteConfig.h"
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
      if (key == Tui::KEY_ENTER || key == Tui::KEY_LF || key == ' ')
      {            // Enter or Space to go back
        onEnter(); // Reset form
      }
      break;
    }
  }

  void SendScreen::handleAddressInput(int key)
  {
    if (key == Tui::KEY_ENTER || key == Tui::KEY_LF)
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

    if (key == Tui::KEY_ESC)
    {
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (key == Tui::KEY_BACKSPACE || key == Tui::KEY_DEL)
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
    if (key == Tui::KEY_ENTER || key == Tui::KEY_LF)
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
          const uint64_t fee = cn::parameters::MINIMUM_FEE_V2;
          if (m_amount + fee > BoltCore::spendableAmountBeforeFee(balance))
          {
            m_error = "Insufficient balance (need amount + 0.001 CCX fee)";
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

    if (key == Tui::KEY_ESC)
    {
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      return;
    }

    if (key == Tui::KEY_BACKSPACE || key == Tui::KEY_DEL)
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
    if (key == 'y' || key == 'Y' || key == Tui::KEY_ENTER || key == Tui::KEY_LF)
    {
      doSend();
    }
    else if (key == 'n' || key == 'N' || key == Tui::KEY_ESC)
    {
      onEnter(); // Reset
    }
  }

  void SendScreen::doSend()
  {
    if (!m_submitWalletTask)
    {
      m_state = State::Error;
      m_error = "Internal error: wallet task runner not available";
      return;
    }

    m_state = State::Sending;
    m_error.clear();

    const std::string address = m_address;
    const uint64_t amount = m_amount;
    auto wallet = m_wallet;

    m_submitWalletTask(
        [wallet, address, amount]()
        { return wallet->transfer(address, amount); },
        [this](BoltCore::TransferResult result)
        {
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
        });
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
    drawHeader(buf, title(), balance.currentHeight, BoltCore::spendableAmountBeforeFee(balance), "");

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
      buf.write(Tui::drawBox(boxTop, 2, 9, boxW, "Enter Amount"));
      buf.writeAt(boxTop + 1, 4, Tui::dim() + "To: " + Tui::reset() + m_address.substr(0, 60));
      buf.writeAt(boxTop + 3, 4, Tui::dim() + "Amount (CCX):" + Tui::reset());
      buf.writeAt(boxTop + 4, 4, Tui::brightWhite() + "> " + m_amountStr + (m_amountStr.empty() ? Tui::dim() + "0.0" + Tui::reset() : "") + Tui::reset());
      buf.writeAt(boxTop + 6, 4, Tui::dim() + "Network fee: " + Tui::reset() +
                                    formatAmount(cn::parameters::MINIMUM_FEE_V2) + " CCX");
      break;
    }
    case State::Confirm:
    {
      const uint64_t fee = cn::parameters::MINIMUM_FEE_V2;
      const uint64_t spendable = BoltCore::spendableAmountBeforeFee(balance);
      buf.write(Tui::drawBox(boxTop, 2, 12, boxW, "Confirm Transaction"));
      buf.writeAt(boxTop + 1, 4, Tui::dim() + "To:     " + Tui::reset() + m_address.substr(0, 55));
      buf.writeAt(boxTop + 2, 4, Tui::dim() + "Amount: " + Tui::reset() + Tui::brightGreen() + m_amountStr + " CCX" + Tui::reset());
      buf.writeAt(boxTop + 3, 4, Tui::dim() + "Fee:    " + Tui::reset() + formatAmount(fee) + " CCX");
      buf.writeAt(boxTop + 4, 4, Tui::dim() + "Total:  " + Tui::reset() + Tui::bold() + formatAmount(m_amount + fee) + " CCX" + Tui::reset());
      buf.writeAt(boxTop + 6, 4, Tui::dim() + "Available: " + Tui::reset() + formatAmount(spendable) + " CCX");
      buf.writeAt(boxTop + 8, 4, Tui::brightYellow() + "Send this transaction? [Y/N]" + Tui::reset());
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