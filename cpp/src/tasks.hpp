#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "frame.hpp"

class GttConfig;
class GitlabClient;

class Tasks {
 public:
  Tasks(GttConfig& cfg, GitlabClient* client = nullptr);

  Frame start(const std::string& project, const std::string& type, const nlohmann::json& id,
              const std::optional<std::string>& note);
  std::vector<Frame> stop();
  std::vector<Frame> cancel();
  std::vector<Frame> status();
  void sync();

  /** List issues (REST). */
  nlohmann::json list_issues(const std::string& project, const std::string& state, bool mine);

  /** Wie `tasks.log()` in Node: Frames nach Kalendertag, Summen in Sekunden (ceil-Dauer). */
  struct LogCollected {
    std::map<std::string, std::vector<Frame>> frames;
    std::map<std::string, std::int64_t> times;
  };
  LogCollected collect_log() const;

 private:
  std::vector<std::filesystem::path> running_frame_paths() const;
  static bool file_looks_running(const std::filesystem::path& p);

  GttConfig& cfg_;
  GitlabClient* client_ = nullptr;
};
