// Copyright (c) 2018-2026 Conceal Network & Conceal Devs
// Distributed under the MIT/X11 software license

#include "Tui.h"

#if defined(_WIN32)
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/select.h>
#endif

namespace Tui
{

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
            return 1000; // Up
          case 'B':
            return 1001; // Down
          case 'C':
            return 1002; // Right
          case 'D':
            return 1003; // Left
          case 'H':
            return 1004; // Home
          case 'F':
            return 1005; // End
          }
        }
      }
      return c;
    }
    return -1;
  }

#endif

} // namespace Tui