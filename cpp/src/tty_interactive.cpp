#include "tty_interactive.hpp"

#include <iostream>

#ifdef _WIN32

namespace gtt {

ArrowSelectResult tty_arrow_select(const std::vector<std::string>& lines_normal,
                                   const std::vector<std::string>& lines_selected, std::size_t default_index,
                                   const std::string& title) {
  (void)lines_normal;
  (void)lines_selected;
  (void)default_index;
  (void)title;
  ArrowSelectResult out{};
  out.kind = ArrowSelectKind::NeedFallback;
  return out;
}

}  // namespace gtt

#else

#include <termios.h>
#include <unistd.h>

namespace gtt {

namespace {

enum class Key { Up, Down, Enter, Escape, Quit, Ignored };

bool stdin_tty() { return isatty(STDIN_FILENO) != 0; }
bool stdout_tty() { return isatty(STDOUT_FILENO) != 0; }

class RawModeGuard {
 public:
  RawModeGuard() {
    if (!stdin_tty()) {
      return;
    }
    if (tcgetattr(STDIN_FILENO, &orig_) != 0) {
      return;
    }
    termios raw = orig_;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
      ok_ = true;
    }
  }
  ~RawModeGuard() {
    if (ok_) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_);
    }
  }
  bool ok() const { return ok_; }

 private:
  termios orig_{};
  bool ok_ = false;
};

Key read_one_key() {
  unsigned char c = 0;
  if (read(STDIN_FILENO, &c, 1) != 1) {
    return Key::Ignored;
  }
  if (c == '\r' || c == '\n') {
    return Key::Enter;
  }
  if (c == 'q' || c == 'Q') {
    return Key::Quit;
  }
  if (c != 0x1b) {
    return Key::Ignored;
  }
  unsigned char b = 0;
  if (read(STDIN_FILENO, &b, 1) != 1) {
    return Key::Escape;
  }
  if (b == '[' || b == 'O') {
    unsigned char d = 0;
    if (read(STDIN_FILENO, &d, 1) != 1) {
      return Key::Ignored;
    }
    if (d == 'A') {
      return Key::Up;
    }
    if (d == 'B') {
      return Key::Down;
    }
    // CSI with parameters (e.g. Alt-arrows): consume until final letter
    while ((d >= '0' && d <= '9') || d == ';' || d == '?') {
      if (read(STDIN_FILENO, &d, 1) != 1) {
        return Key::Ignored;
      }
    }
    if (d == 'A') {
      return Key::Up;
    }
    if (d == 'B') {
      return Key::Down;
    }
  }
  return Key::Ignored;
}

}  // namespace

ArrowSelectResult tty_arrow_select(const std::vector<std::string>& lines_normal,
                                   const std::vector<std::string>& lines_selected, std::size_t default_index,
                                   const std::string& title) {
  ArrowSelectResult out{};
  if (lines_normal.empty() || lines_selected.size() != lines_normal.size()) {
    out.kind = ArrowSelectKind::Cancelled;
    return out;
  }
  if (!stdin_tty() || !stdout_tty()) {
    out.kind = ArrowSelectKind::NeedFallback;
    return out;
  }

  RawModeGuard raw;
  if (!raw.ok()) {
    out.kind = ArrowSelectKind::NeedFallback;
    return out;
  }

  if (default_index >= lines_normal.size()) {
    default_index = lines_normal.size() - 1;
  }
  std::size_t sel = default_index;

  while (true) {
    std::cout << "\033[2J\033[H\033[1m" << title << "\033[0m (↑↓ Enter, q beenden)\n\n";
    // Zeilen wie @inquirer/select: 4 Spalten Einzug, Cursor ❯ (U+276F) + 3 Spaces — Breite wie "    "
    static const char k_cursor[] = "\xe2\x9d\xaf";
    for (std::size_t i = 0; i < lines_normal.size(); ++i) {
      const std::string& row = (i == sel) ? lines_selected[i] : lines_normal[i];
      if (i == sel) {
        std::cout << "\033[36m" << k_cursor << "\033[0m   " << row << "\n";
      } else {
        std::cout << "    " << row << "\n";
      }
    }
    std::cout << std::flush;

    const Key k = read_one_key();
    if (k == Key::Up) {
      sel = (sel + lines_normal.size() - 1) % lines_normal.size();
    } else if (k == Key::Down) {
      sel = (sel + 1) % lines_normal.size();
    } else if (k == Key::Enter) {
      out.kind = ArrowSelectKind::Selected;
      out.index = sel;
      return out;
    } else if (k == Key::Quit || k == Key::Escape) {
      out.kind = ArrowSelectKind::Cancelled;
      return out;
    }
  }
}

}  // namespace gtt

#endif
