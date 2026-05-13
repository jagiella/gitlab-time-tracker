#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace gtt {

enum class ArrowSelectKind {
  /** User chose a row (index valid). */
  Selected,
  /** q / Esc (where applicable). */
  Cancelled,
  /** Stdin/Stdout not a TTY or raw mode failed — use typed input. */
  NeedFallback,
};

struct ArrowSelectResult {
  ArrowSelectKind kind = ArrowSelectKind::NeedFallback;
  std::size_t index = 0;
};

/**
 * Inquirer-style list: ↑/↓ move highlight, Enter confirms (like Node `gtt edit`).
 * `lines_selected[i]` ist die Darstellung von Zeile i, wenn i ausgewählt ist (Cyan auf ID / „ to “ / Titel).
 * Non-TTY → NeedFallback.
 */
ArrowSelectResult tty_arrow_select(const std::vector<std::string>& lines_normal,
                                   const std::vector<std::string>& lines_selected, std::size_t default_index,
                                   const std::string& title);

}  // namespace gtt
