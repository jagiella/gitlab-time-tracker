#pragma once

#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

class GttConfig;

struct Frame {
  static Frame from_file(GttConfig& cfg, const std::filesystem::path& path);
  static Frame from_json(GttConfig& cfg, const nlohmann::json& j);

  void validate() const;
  void write(GttConfig& cfg, bool skip_modified = false) const;

  std::filesystem::path file_path(const GttConfig& cfg) const;

  std::string id;
  std::string project;
  nlohmann::json resource;
  nlohmann::json notes = nlohmann::json::array();
  std::string start_iso;
  std::optional<std::string> stop_iso;
  std::string timezone;
  std::optional<std::string> modified_iso;
  std::optional<std::string> title;
  std::optional<std::string> note;

  bool stopped() const { return stop_iso.has_value() && !stop_iso->empty(); }

  /** Duration in seconds (0 if still running). */
  std::int64_t duration_seconds(const GttConfig& cfg) const;

  /** Sum of `time` in frame notes (synced GitLab segments). */
  int notes_time_total() const;

  /** Grouping key YYYY-MM-DD in the configured display timezone. */
  std::string log_date_key(const GttConfig& cfg) const;

  /** Absolute instants for ordering and duration (same rules as Node moment parsing). */
  std::chrono::sys_seconds start_sys() const;
  std::chrono::sys_seconds stop_sys() const;

  /** HH:mm for log: parse instant using stored_tz, format wall clock in display_tz. */
  static std::string iso_hh_mm(const std::string& iso, const std::string& stored_tz, const std::string& display_tz);

  std::int64_t ceil_duration_seconds(const GttConfig& cfg) const;

  void stop_now(GttConfig& cfg);

  static Frame create_started(GttConfig& cfg, const std::string& project_path, const std::string& resource_type,
                              const nlohmann::json& resource_id, const std::optional<std::string>& note_text);
};

std::string frame_generate_new_id();
