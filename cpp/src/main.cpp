#include <CLI/CLI.hpp>
#include <clocale>

#include "commands.hpp"

int main(int argc, char** argv) {
  std::setlocale(LC_CTYPE, "");
  // Empty description: bare `gtt` should match Node-style help (no extra title line).
  CLI::App app{"", "gtt"};
  app.set_version_flag("-V,--version", "1.0.0-cpp");
  return run_gtt_cli(app, argc, argv);
}
