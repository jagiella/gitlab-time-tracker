#include <CLI/CLI.hpp>
#include <clocale>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "commands.hpp"

namespace {

bool help_topic_is_known(const char* topic) {
  const std::string t(topic);
  for (const std::string& name : gtt_cli_subcommand_names()) {
    if (t == name) {
      return true;
    }
  }
  return false;
}

/** Commander/Node: `gtt help` → `gtt --help`, `gtt help edit` → `gtt edit --help` (CLI11 sonst zwei Callbacks). */
bool rewrite_help_argv(int& argc, char**& argv_owned, std::vector<std::string>& storage, std::vector<char*>& ptrs) {
  if (argc < 2 || std::strcmp(argv_owned[1], "help") != 0) {
    return false;
  }
  if (argc >= 3 && !help_topic_is_known(argv_owned[2])) {
    std::cerr << "Unknown command: " << argv_owned[2] << "\nRun \"gtt --help\" for a list of commands.\n";
    std::exit(2);
  }
  storage.clear();
  ptrs.clear();
  storage.emplace_back(argv_owned[0]);
  if (argc == 2) {
    storage.emplace_back("--help");
  } else {
    storage.emplace_back(argv_owned[2]);
    for (int i = 3; i < argc; ++i) {
      storage.emplace_back(argv_owned[i]);
    }
    storage.emplace_back("--help");
  }
  for (auto& s : storage) {
    ptrs.push_back(s.data());
  }
  ptrs.push_back(nullptr);
  argc = static_cast<int>(ptrs.size()) - 1;
  argv_owned = ptrs.data();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::setlocale(LC_CTYPE, "");
  std::vector<std::string> storage;
  std::vector<char*> ptrs;
  char** argv_mut = argv;
  rewrite_help_argv(argc, argv_mut, storage, ptrs);

  // Empty description: bare `gtt` should match Node-style help (no extra title line).
  CLI::App app{"", "gtt"};
  app.set_version_flag("-V,--version", "1.0.0-cpp");
  return run_gtt_cli(app, argc, argv_mut);
}
