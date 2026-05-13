#pragma once

#include <CLI/CLI.hpp>

#include <string>
#include <vector>

/** Namen aller Subcommands (gleiche Reihenfolge wie in der Haupt-Hilfe); für `gtt help <cmd>`. */
const std::vector<std::string>& gtt_cli_subcommand_names();

/** Registriert Subcommands und führt Parse aus. Rückgabe: Exit-Code für main. */
int run_gtt_cli(CLI::App& app, int argc, char** argv);
