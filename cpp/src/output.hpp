#pragma once

#include <nlohmann/json.hpp>
#include <string>

class GttConfig;

std::string build_report_text(const GttConfig& cfg, const nlohmann::json& bundle, const std::string& kind);

void write_report_to_file(const GttConfig& cfg, const nlohmann::json& bundle, const std::string& kind,
                          const std::string& path);
