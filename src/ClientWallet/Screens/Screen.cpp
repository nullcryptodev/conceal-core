// Screen base class implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "Screen.h"
#include <iomanip>
#include <sstream>

namespace ClientWallet
{

  void Screen::drawHeader(Tui::ScreenBuffer &buf,
                          const std::string &title,
                          uint32_t height,
                          uint64_t balance,
                          const std::string &status)
  {
    // Format balance with 6 decimal places (CCX atomic units)
    std::ostringstream balStr;
    balStr << std::fixed << std::setprecision(6) << (balance / 1000000.0) << " CCX";

    buf.writeAt(1, 0,
                Tui::bold() + Tui::brightCyan() + " " + title + Tui::reset());
    buf.writeAt(1, 50,
                Tui::dim() + "Height: " + std::to_string(height) + Tui::reset());
    buf.writeAt(1, 70,
                Tui::dim() + status + Tui::reset());

    // Balance line
    buf.writeAt(2, 0,
                Tui::brightGreen() + " Balance: " + balStr.str() + Tui::reset());

    // Separator
    buf.writeAt(3, 0, std::string(80, '-'));
  }

  void Screen::drawMenuBar(Tui::ScreenBuffer &buf,
                           const std::vector<std::string> &items,
                           const std::vector<std::string> &keys)
  {
    int terminalHeight = Tui::terminalHeight();
    std::string bar;
    for (size_t i = 0; i < items.size() && i < keys.size(); ++i)
    {
      if (i > 0)
        bar += "  ";
      bar += Tui::brightWhite() + "[" + keys[i] + "]" + Tui::reset();
      bar += " " + items[i];
    }
    buf.writeAt(terminalHeight - 1, 0, bar);
  }

  void Screen::showMessage(Tui::ScreenBuffer &buf,
                           const std::string &title,
                           const std::string &message)
  {
    int termH = Tui::terminalHeight();
    int termW = Tui::terminalWidth();
    int boxW = std::min(60, termW - 4);
    int boxH = 5;
    int top = (termH - boxH) / 2;
    int left = (termW - boxW) / 2;

    buf.write(Tui::drawBox(top, left, boxH, boxW, title));
    buf.writeAt(top + 2, left + 2, message);
  }

} // namespace ClientWallet