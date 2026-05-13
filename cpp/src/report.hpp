#pragma once

#include <nlohmann/json.hpp>

class GttConfig;
class GitlabClient;

/** Full `gtt report` pipeline (table / csv / markdown only). */
void run_report(GttConfig& cfg, GitlabClient& client);
