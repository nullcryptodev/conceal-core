// ReceiveScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "ReceiveScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include <iomanip>
#include <sstream>

namespace ClientWallet
{

  ReceiveScreen::ReceiveScreen(std::shared_ptr<BoltCore::Wallet> wallet)
      : m_wallet(std::move(wallet)) {}

  void ReceiveScreen::onEnter()
  {
    m_mainAddress = m_wallet->getMainAddress();
    m_subAddresses = m_wallet->getSubAddresses();
    m_selectedIndex = 0;
    m_showNewSub = false;
  }

  void ReceiveScreen::onKey(int key)
  {
    int maxIndex = 1 + static_cast<int>(m_subAddresses.size()); // +1 for main

    switch (key)
    {
    case 1000:
      if (m_selectedIndex > 0)
        m_selectedIndex--;
      break;
    case 1001:
      if (m_selectedIndex < maxIndex - 1)
        m_selectedIndex++;
      break;
    case 'n':
    case 'N': // Generate new sub-address
      if (m_wallet->getType() == BoltCore::WalletType::Full)
      {
        m_wallet->generateSubAddress();
        m_subAddresses = m_wallet->getSubAddresses();
        m_showNewSub = true;
      }
      break;
    case 27:
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      break;
    }
  }

  void ReceiveScreen::render(Tui::ScreenBuffer &buf)
  {
    auto balance = m_wallet->getBalance();
    drawHeader(buf, title(), balance.currentHeight, balance.actual, "");

    int termW = Tui::terminalWidth();
    int boxW = std::min(74, termW - 4);
    int boxTop = 5;

    // Get current address
    std::string currentAddress;
    std::string label;
    if (m_selectedIndex == 0)
    {
      currentAddress = m_mainAddress;
      label = "Main Address";
    }
    else
    {
      int subIdx = m_selectedIndex - 1;
      if (subIdx < static_cast<int>(m_subAddresses.size()))
      {
        currentAddress = m_subAddresses[subIdx].address;
        label = "Sub-address #" + std::to_string(subIdx + 1);
      }
    }

    // ── Address selector ──────────────────────────────────────────────
    buf.write(Tui::drawBox(boxTop, 2, 3, boxW, "Address Selector"));

    std::string mainLine = (m_selectedIndex == 0 ? Tui::bold() + Tui::brightWhite() + " > Main Address" + Tui::reset()
                                                 : Tui::dim() + "   Main Address" + Tui::reset());
    buf.writeAt(boxTop + 1, 4, mainLine);

    int row = boxTop + 2;
    for (size_t i = 0; i < m_subAddresses.size() && row < boxTop + 8; ++i)
    {
      int idx = static_cast<int>(i) + 1;
      std::string subLine = (m_selectedIndex == idx ? Tui::bold() + Tui::brightWhite() + " > Sub #" + std::to_string(i + 1) + Tui::reset()
                                                    : Tui::dim() + "   Sub #" + std::to_string(i + 1) + Tui::reset());
      buf.writeAt(row, 4, subLine);
      row++;
    }

    // ── Address display ───────────────────────────────────────────────
    int addrTop = boxTop + 10;
    buf.write(Tui::drawBox(addrTop, 2, 8, boxW, label));

    // Display address in chunks for readability
    buf.writeAt(addrTop + 1, 4, Tui::brightGreen() + Tui::bold() + currentAddress.substr(0, 59) + Tui::reset());
    if (currentAddress.size() > 59)
      buf.writeAt(addrTop + 2, 4, Tui::brightGreen() + Tui::bold() + currentAddress.substr(59) + Tui::reset());

    // QR-like ASCII representation
    buf.writeAt(addrTop + 4, 4, Tui::dim() + "QR: [Scan this address with a Conceal wallet]" + Tui::reset());

    // Simple ASCII QR placeholder (8x8 block pattern based on address hash)
    std::string qrLines[8];
    crypto::Hash addrHash;
    crypto::cn_fast_hash(currentAddress.data(), currentAddress.size(), addrHash);
    for (int y = 0; y < 8; ++y)
    {
      qrLines[y] = "  ";
      for (int x = 0; x < 8; ++x)
      {
        int byteIdx = y + x * 8;
        bool filled = (addrHash.data[byteIdx % 32] >> (x % 8)) & 1;
        qrLines[y] += filled ? "██" : "  ";
      }
    }
    for (int y = 0; y < 8; ++y)
      buf.writeAt(addrTop + 5 + y, 4, qrLines[y]);

    // ── Help text ─────────────────────────────────────────────────────
    buf.writeAt(addrTop + 14, 4, Tui::dim() + "Arrow keys: switch address  |  N: new sub-address  |  Esc: back" + Tui::reset());

    if (m_showNewSub)
    {
      buf.writeAt(addrTop + 15, 4, Tui::brightGreen() + "New sub-address created!" + Tui::reset());
      m_showNewSub = false;
    }

    drawMenuBar(buf, {"Back", "New Sub", "Switch"}, {"Esc", "N", "↑↓"});
  }

} // namespace ClientWallet