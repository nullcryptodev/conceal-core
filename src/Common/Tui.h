// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#pragma once

#include <string>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <functional>
#include <cstdlib>

namespace Tui
{

  // ── ANSI escape code generators ─────────────────────────────────────────

  inline std::string reset() { return "\033[0m"; }
  inline std::string bold() { return "\033[1m"; }
  inline std::string dim() { return "\033[2m"; }
  inline std::string underline() { return "\033[4m"; }

  // Foreground colors
  inline std::string black() { return "\033[30m"; }
  inline std::string red() { return "\033[31m"; }
  inline std::string green() { return "\033[32m"; }
  inline std::string yellow() { return "\033[33m"; }
  inline std::string blue() { return "\033[34m"; }
  inline std::string magenta() { return "\033[35m"; }
  inline std::string cyan() { return "\033[36m"; }
  inline std::string white() { return "\033[37m"; }
  inline std::string brightBlack() { return "\033[90m"; }
  inline std::string brightRed() { return "\033[91m"; }
  inline std::string brightGreen() { return "\033[92m"; }
  inline std::string brightYellow() { return "\033[93m"; }
  inline std::string brightBlue() { return "\033[94m"; }
  inline std::string brightMagenta() { return "\033[95m"; }
  inline std::string brightCyan() { return "\033[96m"; }
  inline std::string brightWhite() { return "\033[97m"; }

  // Brand accents (256-color; terminals without 256-color may fall back gracefully)
  inline std::string orange() { return "\033[38;5;208m"; }
  inline std::string accentGreen() { return "\033[38;5;82m"; }

  // Background colors
  inline std::string bgBlack() { return "\033[40m"; }
  inline std::string bgRed() { return "\033[41m"; }
  inline std::string bgGreen() { return "\033[42m"; }
  inline std::string bgYellow() { return "\033[43m"; }
  inline std::string bgBlue() { return "\033[44m"; }
  inline std::string bgMagenta() { return "\033[45m"; }
  inline std::string bgCyan() { return "\033[46m"; }
  inline std::string bgWhite() { return "\033[47m"; }

  // ── Cursor and screen control ───────────────────────────────────────────

  inline std::string cursorTo(int row, int col)
  {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
  }

  inline std::string cursorUp(int n) { return "\033[" + std::to_string(n) + "A"; }
  inline std::string cursorDown(int n) { return "\033[" + std::to_string(n) + "B"; }
  inline std::string cursorRight(int n) { return "\033[" + std::to_string(n) + "C"; }
  inline std::string cursorLeft(int n) { return "\033[" + std::to_string(n) + "D"; }

  inline std::string clearScreen() { return "\033[2J"; }
  inline std::string clearToEndOfLine() { return "\033[K"; }
  inline std::string clearToEndOfScreen() { return "\033[0J"; }

  inline std::string enterAltScreen() { return "\033[?1049h"; }
  inline std::string exitAltScreen() { return "\033[?1049l"; }

  inline std::string showCursor() { return "\033[?25h"; }
  inline std::string hideCursor() { return "\033[?25l"; }

  // ── Terminal size ───────────────────────────────────────────────────────

  inline int parseTerminalDimension(const char *value, int fallback)
  {
    if (!value || !*value)
      return fallback;
    try
    {
      const int dim = std::stoi(value);
      if (dim >= 1)
        return dim;
    }
    catch (...)
    {
    }
    return fallback;
  }

  int terminalWidth();
  int terminalHeight();

  // ── Box drawing characters ──────────────────────────────────────────────

  inline std::string boxTopLeft() { return "+"; }
  inline std::string boxTopRight() { return "+"; }
  inline std::string boxBottomLeft() { return "+"; }
  inline std::string boxBottomRight() { return "+"; }
  inline std::string boxHorizontal() { return "-"; }
  inline std::string boxVertical() { return "|"; }

  inline std::string thinTopLeft() { return "+"; }
  inline std::string thinTopRight() { return "+"; }
  inline std::string thinBottomLeft() { return "+"; }
  inline std::string thinBottomRight() { return "+"; }
  inline std::string thinHorizontal() { return "-"; }
  inline std::string thinVertical() { return "|"; }

  // ── Color helper ────────────────────────────────────────────────────────

  inline std::string colorize(const std::string &text, const std::string &fg, const std::string &bg = "", bool bld = false)
  {
    std::string result;
    if (bld)
      result += bold();
    result += fg;
    if (!bg.empty())
      result += bg;
    result += text;
    result += reset();
    return result;
  }

  // ── Progress bar ────────────────────────────────────────────────────────

  inline std::string progressBar(double pct, int width, bool showPct = true)
  {
    int filled = static_cast<int>(pct * width / 100.0);
    if (filled > width)
      filled = width;
    if (filled < 0)
      filled = 0;
    int empty = width - filled;

    std::string result;
    result += green();
    result += std::string(filled, '#');
    result += dim();
    result += std::string(empty, '-');
    result += reset();

    if (showPct)
    {
      std::ostringstream oss;
      oss << " " << std::fixed << std::setprecision(1) << pct << "%";
      result += oss.str();
    }
    return result;
  }

  // ── Table helper ────────────────────────────────────────────────────────

  inline std::string tableRow(const std::vector<std::string> &cols,
                              const std::vector<int> &widths,
                              const std::string &separator = " | ")
  {
    std::string result;
    for (size_t i = 0; i < cols.size() && i < widths.size(); ++i)
    {
      if (i > 0)
        result += separator;
      std::string cell = cols[i];
      if (cell.size() > static_cast<size_t>(widths[i]))
      {
        cell = cell.substr(0, widths[i] - 1) + ".";
      }
      result += cell;
      int padding = widths[i] - static_cast<int>(cell.size());
      if (padding > 0)
        result += std::string(padding, ' ');
    }
    return result;
  }

  // ── Screen buffer ───────────────────────────────────────────────────────

  class ScreenBuffer
  {
  public:
    ScreenBuffer() = default;

    void clear()
    {
      m_buffer.str("");
      m_buffer.clear();
    }

    void write(const std::string &s) { m_buffer << s; }
    void writeLine(const std::string &s) { m_buffer << s << "\r\n"; }

    void writeAt(int row, int col, const std::string &s)
    {
      m_buffer << cursorTo(row, col) << s;
    }

    void flush()
    {
      std::cout << m_buffer.str() << std::flush;
      clear();
    }

    std::string str() const { return m_buffer.str(); }

  private:
    std::ostringstream m_buffer;
  };

  // ── Menu list ───────────────────────────────────────────────────────────

  inline std::string menuList(const std::vector<std::string> &items,
                              int selected,
                              const std::string &highlightColor = "",
                              const std::string &normalColor = "")
  {
    std::string result;
    std::string hc = highlightColor.empty() ? bold() + brightWhite() : highlightColor;
    std::string nc = normalColor.empty() ? dim() : normalColor;

    for (size_t i = 0; i < items.size(); ++i)
    {
      if (static_cast<int>(i) == selected)
      {
        result += " " + hc + "> " + items[i] + reset() + "\r\n";
      }
      else
      {
        result += "  " + nc + items[i] + reset() + "\r\n";
      }
    }
    return result;
  }

  // ── Box drawing ─────────────────────────────────────────────────────────

  inline std::string drawBox(int top, int left, int height, int width,
                             const std::string &title = "",
                             bool thick = true)
  {
    std::string result;
    std::string tl, tr, bl, br, h, v;
    if (thick)
    {
      tl = boxTopLeft();
      tr = boxTopRight();
      bl = boxBottomLeft();
      br = boxBottomRight();
      h = boxHorizontal();
      v = boxVertical();
    }
    else
    {
      tl = thinTopLeft();
      tr = thinTopRight();
      bl = thinBottomLeft();
      br = thinBottomRight();
      h = thinHorizontal();
      v = thinVertical();
    }

    // Top border
    result += cursorTo(top, left);
    result += tl + h;
    if (!title.empty())
    {
      result += " " + title + " ";
    }
    int remaining = width - 3;
    if (!title.empty())
      remaining -= static_cast<int>(title.size() + 2);
    if (remaining > 0)
      result += std::string(static_cast<size_t>(remaining), h[0]);
    result += tr;

    // Sides
    for (int r = 1; r < height - 1; ++r)
    {
      result += cursorTo(top + r, left) + v;
      result += cursorTo(top + r, left + width - 1) + v;
    }

    // Bottom border
    result += cursorTo(top + height - 1, left);
    if (width > 2)
      result += bl + std::string(static_cast<size_t>(width - 2), h[0]) + br;
    else
      result += bl + br;

    return result;
  }

  // ── Input helpers ───────────────────────────────────────────────────────

  // Virtual key codes returned by readKey() for arrow keys (ANSI escape sequences).
  constexpr int KEY_UP = 1000;
  constexpr int KEY_DOWN = 1001;
  constexpr int KEY_RIGHT = 1002;
  constexpr int KEY_LEFT = 1003;
  constexpr int KEY_HOME = 1004;
  constexpr int KEY_END = 1005;
  constexpr int KEY_ESC = 27;
  constexpr int KEY_BACKSPACE = 127;
  constexpr int KEY_DEL = 8;
  constexpr int KEY_ENTER = '\r';
  constexpr int KEY_LF = '\n';

  void enableRawMode();
  void disableRawMode();
  bool keyAvailable();
  int readKey();

  // Runs work on a background thread while animating message + spinner (alt screen).
  void runWithStatusSpinner(const std::string &message, const std::function<void()> &work);

} // namespace Tui