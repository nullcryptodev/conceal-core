// ReceiveScreen implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "ReceiveScreen.h"
#include "BoltCore/BoltCore.h"
#include "BoltCore/BoltCoreTypes.h"
#include "qrcodegen.hpp"
#include <algorithm>

namespace ClientWallet
{

  namespace
  {

    constexpr int kQrQuietModules = 4;

    inline std::string qrPaper() { return "\033[48;5;255m"; }
    inline std::string qrInk() { return "\033[38;5;16m"; }
    inline std::string qrOn() { return qrPaper() + qrInk() + Tui::bold(); }

    std::string qrLightCell()
    {
      return qrPaper() + " " + Tui::reset();
    }

    std::string qrDarkCell()
    {
      return qrOn() + "\xe2\x96\x88" + Tui::reset(); // █
    }

    // 2×4 QR modules per terminal cell (compact); block glyphs keep modules square-ish.
    void appendQrBlockCell(std::string &line, bool top, bool bottom)
    {
      if (top && bottom)
        line += qrDarkCell();
      else if (top)
        line += qrOn() + "\xe2\x96\x80" + Tui::reset(); // ▀
      else if (bottom)
        line += qrOn() + "\xe2\x96\x84" + Tui::reset(); // ▄
      else
        line += qrLightCell();
    }

    bool qrModuleAt(const qrcodegen::QrCode &qr, int displaySize, int px, int py)
    {
      const int mx = px - kQrQuietModules;
      const int my = py - kQrQuietModules;
      if (mx < 0 || my < 0 || mx >= qr.getSize() || my >= qr.getSize())
        return false;
      return qr.getModule(mx, my);
    }

    std::string buildQrLine(const qrcodegen::QrCode &qr, int displaySize, int rowPair)
    {
      const int py = rowPair * 2;
      std::string line;
      for (int px = 0; px < displaySize; ++px)
      {
        const bool top = qrModuleAt(qr, displaySize, px, py);
        const bool bottom =
            (py + 1 < displaySize) && qrModuleAt(qr, displaySize, px, py + 1);
        appendQrBlockCell(line, top, bottom);
      }
      return line;
    }

    int qrTerminalRows(int displaySize)
    {
      return (displaySize + 1) / 2;
    }

    int qrTerminalCols(int displaySize)
    {
      return displaySize;
    }

    void drawAddressPanel(Tui::ScreenBuffer &buf, int addrTop, int boxW, const std::string &label,
                        const std::string &currentAddress, int addrTextRows, int qrLabelRow,
                        int qrFirstRow, int qrTermRows, bool qrOk, const qrcodegen::QrCode *qr)
    {
      const int qrHintRows = qrOk ? 1 : 0;
      const int addrContentRows =
          addrTextRows + 1 + (qrOk ? 1 + qrTermRows + qrHintRows : 1) + 1;
      const int addrBoxH = addrContentRows + 2;

      buf.write(Tui::drawBox(addrTop, 2, addrBoxH, boxW, label));

      buf.writeAt(addrTop + 1, 4, Tui::brightGreen() + Tui::bold() + currentAddress.substr(0, 59) + Tui::reset());
      if (currentAddress.size() > 59)
        buf.writeAt(addrTop + 2, 4, Tui::brightGreen() + Tui::bold() + currentAddress.substr(59) + Tui::reset());

      int nextRow = qrLabelRow + 1;

      if (qrOk && qr)
      {
        const int displaySize = qr->getSize() + 2 * kQrQuietModules;
        const int qrCols = qrTerminalCols(displaySize);
        const int padLeft = std::max(0, (boxW - 4 - qrCols) / 2);
        const std::string pad(padLeft, ' ');

        buf.writeAt(qrLabelRow, 4, Tui::dim() + "QR: Scan with a Conceal wallet" + Tui::reset());

        const int rowPairs = qrTerminalRows(displaySize);
        for (int rowPair = 0; rowPair < rowPairs; ++rowPair)
          buf.writeAt(qrFirstRow + rowPair, 4, pad + buildQrLine(*qr, displaySize, rowPair));

        nextRow = qrFirstRow + qrTermRows;
        buf.writeAt(nextRow, 4,
                    Tui::dim() + "Zoom terminal out if clipped; code must look square to scan" + Tui::reset());
        ++nextRow;
      }
      else
      {
        buf.writeAt(qrLabelRow, 4, Tui::dim() + "QR: (encode failed)" + Tui::reset());
      }

      buf.writeAt(nextRow, 4, Tui::dim() + "Arrow keys: switch  |  N: new sub-address  |  Esc: back" + Tui::reset());
    }

  } // namespace

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
    int maxIndex = 1 + static_cast<int>(m_subAddresses.size());

    switch (key)
    {
    case Tui::KEY_UP:
      if (m_selectedIndex > 0)
        m_selectedIndex--;
      break;
    case Tui::KEY_DOWN:
      if (m_selectedIndex < maxIndex - 1)
        m_selectedIndex++;
      break;
    case 'n':
    case 'N':
      if (m_wallet->getType() == BoltCore::WalletType::Full)
      {
        m_wallet->generateSubAddress();
        m_subAddresses = m_wallet->getSubAddresses();
        m_showNewSub = true;
      }
      break;
    case Tui::KEY_ESC:
      if (m_onAction)
        m_onAction(ScreenAction::Pop);
      break;
    }
  }

  void ReceiveScreen::render(Tui::ScreenBuffer &buf)
  {
    auto balance = m_wallet->getBalance();
    drawHeader(buf, title(), balance.currentHeight, BoltCore::spendableAmountBeforeFee(balance), "");

    int termW = Tui::terminalWidth();
    int termH = Tui::terminalHeight();
    const int menuRow = termH - 1;
    int boxW = std::min(74, termW - 4);
    int boxTop = 5;

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

    const size_t maxVisibleSubs = 4;
    const int selectorContentRows =
        1 + static_cast<int>(std::min(m_subAddresses.size(), maxVisibleSubs));
    const int selectorBoxH = selectorContentRows + 2;
    buf.write(Tui::drawBox(boxTop, 2, selectorBoxH, boxW, "Address Selector"));

    std::string mainLine = (m_selectedIndex == 0 ? Tui::bold() + Tui::brightWhite() + " > Main Address" + Tui::reset()
                                                 : Tui::dim() + "   Main Address" + Tui::reset());
    buf.writeAt(boxTop + 1, 4, mainLine);

    int row = boxTop + 2;
    for (size_t i = 0; i < m_subAddresses.size() && i < maxVisibleSubs; ++i)
    {
      int idx = static_cast<int>(i) + 1;
      std::string subLine = (m_selectedIndex == idx ? Tui::bold() + Tui::brightWhite() + " > Sub #" + std::to_string(i + 1) + Tui::reset()
                                                    : Tui::dim() + "   Sub #" + std::to_string(i + 1) + Tui::reset());
      buf.writeAt(row, 4, subLine);
      row++;
    }

    const int addrTextRows = currentAddress.size() > 59 ? 2 : 1;

    try
    {
      const qrcodegen::QrCode qr =
          qrcodegen::QrCode::encodeText(currentAddress.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
      const int displaySize = qr.getSize() + 2 * kQrQuietModules;
      const int qrTermRows = qrTerminalRows(displaySize);
      const int qrHintRows = 1;
      const int addrContentRows = addrTextRows + 1 + 1 + qrTermRows + qrHintRows + 1;
      const int addrBoxH = addrContentRows + 2;

      int addrTop = boxTop + selectorBoxH + 1;
      if (addrTop + addrBoxH > menuRow)
        addrTop = std::max(4, menuRow - addrBoxH);

      const int qrLabelRow = addrTop + addrTextRows + 2;
      const int qrFirstRow = qrLabelRow + 1;

      drawAddressPanel(buf, addrTop, boxW, label, currentAddress, addrTextRows, qrLabelRow, qrFirstRow,
                       qrTermRows, true, &qr);

      if (m_showNewSub)
      {
        const int noticeRow = addrTop + addrBoxH;
        if (noticeRow < menuRow)
          buf.writeAt(noticeRow, 4, Tui::brightGreen() + "New sub-address created!" + Tui::reset());
        m_showNewSub = false;
      }

      drawMenuBar(buf, {"Back", "New Sub", "Switch"}, {"Esc", "N", "↑↓"});
      return;
    }
    catch (const std::exception &)
    {
    }

    const int addrContentRows = addrTextRows + 1 + 1 + 1;
    const int addrBoxH = addrContentRows + 2;
    int addrTop = boxTop + selectorBoxH + 1;
    if (addrTop + addrBoxH > menuRow)
      addrTop = std::max(4, menuRow - addrBoxH);

    const int qrLabelRow = addrTop + addrTextRows + 2;
    drawAddressPanel(buf, addrTop, boxW, label, currentAddress, addrTextRows, qrLabelRow, qrLabelRow + 1, 0,
                     false, nullptr);

    if (m_showNewSub)
    {
      const int noticeRow = addrTop + addrBoxH;
      if (noticeRow < menuRow)
        buf.writeAt(noticeRow, 4, Tui::brightGreen() + "New sub-address created!" + Tui::reset());
      m_showNewSub = false;
    }

    drawMenuBar(buf, {"Back", "New Sub", "Switch"}, {"Esc", "N", "↑↓"});
  }

} // namespace ClientWallet
