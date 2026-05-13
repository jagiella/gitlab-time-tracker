#pragma once

#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class GttConfig {
 public:
  explicit GttConfig(std::filesystem::path work_dir = std::filesystem::current_path());

  /** Reload YAML from disk (global + optional local). */
  void reload_from_disk();

  nlohmann::json& raw() { return data_; }
  const nlohmann::json& raw() const { return data_; }

  void set_json(const std::string& key, const nlohmann::json& value, bool force = false);

  std::string get_string(const std::string& key, const std::string& def = {}) const;
  int get_int(const std::string& key, int def = 0) const;
  bool get_bool(const std::string& key, bool def = false) const;
  std::vector<std::string> get_string_vec(const std::string& key) const;

  std::filesystem::path global_dir() const { return global_dir_; }
  std::filesystem::path frame_dir() const;
  std::filesystem::path cache_dir() const { return global_dir_ / "cache"; }
  std::filesystem::path global_config_file() const { return global_dir_ / "config.yml"; }
  std::filesystem::path local_config_file() const { return work_dir_ / ".gtt.yml"; }

  std::chrono::sys_seconds get_from() const;
  std::chrono::sys_seconds get_to() const;
  /** Empty config → system zone (see implementation). Explicit "UTC" stays UTC. */
  std::string timezone_name() const;

  std::string time_format_for(const std::string& sub_key) const;
  std::string to_human_readable(std::int64_t seconds, const std::string& time_format_sub = "stats") const;

  std::filesystem::path work_dir() const { return work_dir_; }

 private:
  void ensure_global_layout();
  static nlohmann::json default_data();
  static nlohmann::json yaml_file_to_json(const std::filesystem::path& path);
  bool local_exists_walk_up();

  std::filesystem::path work_dir_;
  std::filesystem::path global_dir_;
  nlohmann::json data_;
};
