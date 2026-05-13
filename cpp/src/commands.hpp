#pragma once

#include <CLI/CLI.hpp>

/** Registriert Subcommands und führt Parse aus. Rückgabe: Exit-Code für main. */
int run_gtt_cli(CLI::App& app, int argc, char** argv);
