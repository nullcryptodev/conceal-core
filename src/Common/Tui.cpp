// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "Tui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#endif

namespace Tui
{

#if !defined(_WIN32)
  namespace
  {
    int queryTerminalDimension(bool height)
    {
#if defined(TIOCGWINSZ)
      struct winsize ws {};
      if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
      {
        const int dim = height ? ws.ws_row : ws.ws_col;
        if (dim >= 1)
          return dim;
      }
#endif
      const char *env = height ? std::getenv("LINES") : std::getenv("COLUMNS");
      return parseTerminalDimension(env, height ? 24 : 80);
    }
  }
#endif

  int terminalWidth()
  {
#if defined(_WIN32)
    return parseTerminalDimension(std::getenv("COLUMNS"), 80);
#else
    return queryTerminalDimension(false);
#endif
  }

  int terminalHeight()
  {
#if defined(_WIN32)
    return parseTerminalDimension(std::getenv("LINES"), 24);
#else
    return queryTerminalDimension(true);
#endif
  }

#if defined(_WIN32)

  void enableRawMode()
  {
    // Windows: _getch() already reads without echo
  }

  void disableRawMode()
  {
  }

  bool keyAvailable()
  {
    return _kbhit();
  }

  int readKey()
  {
    return _getch();
  }

#else

  static struct termios g_original;

  void enableRawMode()
  {
    tcgetattr(STDIN_FILENO, &g_original);
    struct termios raw = g_original;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  }

  void disableRawMode()
  {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_original);
  }

  bool keyAvailable()
  {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    struct timeval tv = {0, 0};
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
  }

  int readKey()
  {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1)
    {
      // Handle escape sequences (arrow keys, etc.)
      if (c == '\033')
      {
        char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
          return c;
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
          return c;
        if (seq[0] == '[')
        {
          switch (seq[1])
          {
          case 'A':
            return KEY_UP;
          case 'B':
            return KEY_DOWN;
          case 'C':
            return KEY_RIGHT;
          case 'D':
            return KEY_LEFT;
          case 'H':
            return KEY_HOME;
          case 'F':
            return KEY_END;
          }
        }
      }
      return c;
    }
    return -1;
  }

#endif

  void runWithStatusSpinner(const std::string &message, const std::function<void()> &work)
  {
    std::atomic<bool> done{false};
    std::exception_ptr error;
    std::thread worker([&]()
                       {
      try
      {
        work();
      }
      catch (...)
      {
        error = std::current_exception();
      }
      done = true; });

    static const char frames[] = {'|', '/', '-', '\\'};
    size_t frame = 0;
    const std::string line = std::string(1, frames[0]) + " " + message;

    while (!done.load())
    {
      const int height = terminalHeight();
      const int width = terminalWidth();
      const int row = std::max(1, height / 2);
      const int col = std::max(1, (width - static_cast<int>(line.size())) / 2);
      const char spinner = frames[frame % (sizeof(frames) / sizeof(frames[0]))];
      ++frame;

      std::cout << clearScreen() << hideCursor() << cursorTo(row, col)
                << dim() << spinner << reset() << " " << message << std::flush;

      std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    worker.join();
    if (error)
      std::rethrow_exception(error);
  }

} // namespace Tui