// Screen base class implementation
// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "Screen.h"
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace ClientWallet
{

  namespace
  {
    constexpr const char *kSyncLogPath = "/tmp/conceal-wallet-sync.log";
    constexpr int kSyncLogBoxWidth = 76;
    std::mutex g_syncLogReadMutex;

    std::string truncateLogLine(const std::string &line, size_t maxWidth)
    {
      if (line.size() <= maxWidth)
        return line;
      if (maxWidth <= 3)
        return line.substr(0, maxWidth);
      return line.substr(0, maxWidth - 3) + "...";
    }

    std::vector<std::string> readLastLogLines(const char *path, size_t lineCount)
    {
      std::lock_guard<std::mutex> lock(g_syncLogReadMutex);
      std::ifstream file(path);
      if (!file)
        return {};

      std::deque<std::string> tail;
      std::string line;
      while (std::getline(file, line))
      {
        tail.push_back(std::move(line));
        if (tail.size() > lineCount)
          tail.pop_front();
      }

      return {tail.begin(), tail.end()};
    }
  }

  void Screen::drawHeader(Tui::ScreenBuffer &buf,
                          const std::string &title,
                          uint32_t height,
                          uint64_t balance,
                          const std::string &status)
  {
    std::ostringstream balStr;
    balStr << std::fixed << std::setprecision(6) << (balance / 1000000.0) << " CCX";

    buf.writeAt(1, 0,
                Tui::bold() + Tui::brightCyan() + " " + title + Tui::reset());
    buf.writeAt(1, 50,
                Tui::dim() + "Height: " + std::to_string(height) + Tui::reset());
    buf.writeAt(1, 70,
                Tui::dim() + status + Tui::reset());

    buf.writeAt(2, 0,
                Tui::brightGreen() + " Balance: " + balStr.str() + Tui::reset());

    buf.writeAt(3, 0, std::string(80, '-'));
  }

  void Screen::drawMenuBar(Tui::ScreenBuffer &buf,
                             const std::vector<std::string> &items,
                             const std::vector<std::string> &keys,
                             int left)
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
    if (terminalHeight >= 1)
      buf.writeAt(terminalHeight - 1, left, bar);
  }

  void Screen::drawLastSyncLog(Tui::ScreenBuffer &buf, int boxTop, size_t lineCount)
  {
    const int termH = Tui::terminalHeight();
    const int menuRow = termH - 1;
    const int boxH = static_cast<int>(lineCount) + 2;

    if (boxTop < 1 || boxTop + boxH > menuRow || boxH < 3)
      return;

    const size_t maxTextWidth = static_cast<size_t>(kSyncLogBoxWidth - 4);

    buf.write(Tui::drawBox(boxTop, 2, boxH, kSyncLogBoxWidth, "Last Log", false));

    const auto lines = readLastLogLines(kSyncLogPath, lineCount);
    for (size_t i = 0; i < lineCount; ++i)
    {
      const int row = boxTop + 1 + static_cast<int>(i);
      if (row >= menuRow)
        break;

      std::string text;
      if (i < lines.size())
        text = truncateLogLine(lines[i], maxTextWidth);
      else if (lines.empty() && i == 0)
        text = "(no sync log yet)";

      if (!text.empty())
        buf.writeAt(row, 4, Tui::dim() + text + Tui::reset());
    }
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
