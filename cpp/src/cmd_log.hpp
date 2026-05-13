#pragma once

#include <optional>
#include <string>

class GttConfig;

/** Entspricht `gtt-log.js`: gruppiert nach Tag, CLI- oder CSV-Ausgabe. */
void run_cmd_log(GttConfig& cfg, bool csv, const std::optional<int>& hours_per_day,
                 const std::optional<std::string>& time_format, bool verbose);
