#include "config.hpp"

#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <sstream>

#include <yaml-cpp/yaml.h>

#include "time_format.hpp"

namespace {

std::filesystem::path home_dir() {
#ifdef _WIN32
  const char* h = std::getenv("USERPROFILE");
#else
  const char* h = std::getenv("HOME");
#endif
  if (h) {
    return std::filesystem::path(h);
  }
  return {};
}

std::filesystem::path xdg_data_home() {
  const char* xdg = std::getenv("XDG_DATA_HOME");
  if (xdg && *xdg) {
    return std::filesystem::path(xdg);
  }
  return home_dir() / ".local" / "share";
}

nlohmann::json yaml_node_to_json(const YAML::Node& node) {
  using json = nlohmann::json;
  if (!node.IsDefined() || node.IsNull()) {
    return json();
  }
  if (node.IsScalar()) {
    const std::string s = node.as<std::string>();
    if (s == "true") {
      return true;
    }
    if (s == "false") {
      return false;
    }
    try {
      return node.as<int>();
    } catch (...) {
    }
    try {
      return node.as<double>();
    } catch (...) {
    }
    return s;
  }
  if (node.IsSequence()) {
    json arr = json::array();
    for (const auto& it : node) {
      arr.push_back(yaml_node_to_json(it));
    }
    return arr;
  }
  if (node.IsMap()) {
    json obj = json::object();
    for (const auto& it : node) {
      obj[it.first.as<std::string>()] = yaml_node_to_json(it.second);
    }
    return obj;
  }
  return json();
}

}  // namespace

nlohmann::json GttConfig::default_data() {
  using json = nlohmann::json;
  return json::object({
      {"type", "project"},
      {"subgroups", false},
      {"url", "https://gitlab.com/api/v4/"},
      {"token", false},
      {"project", false},
      {"from", "1970-01-01"},
      {"to", ""},
      {"iids", json::array()},
      {"closed", false},
      {"milestone", false},
      {"hoursPerDay", 8},
      {"daysPerWeek", 5},
      {"weeksPerMonth", 4},
      {"issueColumns", json::array({"iid", "title", "spent", "total_estimate"})},
      {"mergeRequestColumns", json::array({"iid", "title", "spent", "total_estimate"})},
      {"recordColumns", json::array({"user", "date", "type", "iid", "time"})},
      {"userColumns", false},
      {"dateFormat", "DD.MM.YYYY HH:mm:ss"},
      {"dateFormatGroupReport", "YYYY-MM-DD"},
      {"timeFormat", "[%sign][%days>d ][%hours>h ][%minutes>m ][%seconds>s]"},
      {"output", "table"},
      {"excludeByLabels", false},
      {"includeByLabels", false},
      {"includeLabels", false},
      {"excludeLabels", false},
      {"query", json::array({"issues", "merge_requests"})},
      {"report", json::array({"stats", "issues", "merge_requests", "records"})},
      {"noHeadlines", false},
      {"noWarnings", false},
      {"quiet", false},
      {"showWithoutTimes", false},
      {"timezone", ""},
      {"_perPage", 100},
      {"_parallel", 10},
      {"_verbose", false},
      {"_checkToken", true},
      {"_skipDescriptionParsing", false},
      {"throttleMaxRequestsPerInterval", 10},
      {"throttleInterval", 1000},
  });
}

nlohmann::json GttConfig::yaml_file_to_json(const std::filesystem::path& path) {
  YAML::Node root = YAML::LoadFile(path.string());
  return yaml_node_to_json(root);
}

GttConfig::GttConfig(std::filesystem::path work_dir) : work_dir_(std::filesystem::absolute(work_dir)) {
  global_dir_ = xdg_data_home() / ".gtt";
  ensure_global_layout();
  reload_from_disk();
}

void GttConfig::ensure_global_layout() {
  namespace fs = std::filesystem;
  const fs::path old_home = home_dir() / ".gtt";
  if (!fs::exists(global_dir_) && fs::exists(old_home)) {
    std::error_code ec;
    fs::rename(old_home, global_dir_, ec);
  }
  std::error_code ec;
  fs::create_directories(global_dir_, ec);
  fs::create_directories(global_dir_ / "cache", ec);
  const auto cfg = global_dir_ / "config.yml";
  if (!fs::exists(cfg)) {
    std::ofstream(cfg.string()).close();
  }
}

bool GttConfig::local_exists_walk_up() {
  namespace fs = std::filesystem;
  if (fs::exists(local_config_file())) {
    return true;
  }
  fs::path root = work_dir_.root_path();
  fs::path dir = work_dir_;
  while (true) {
    dir = dir.parent_path();
    if (dir.empty() || dir == root) {
      break;
    }
    if (fs::exists(dir / ".gtt.yml")) {
      work_dir_ = dir;
      return true;
    }
  }
  return false;
}

void GttConfig::reload_from_disk() {
  data_ = default_data();
  try {
    if (std::filesystem::exists(global_config_file())) {
      nlohmann::json g = yaml_file_to_json(global_config_file());
      if (g.is_object()) {
        data_.update(g);
      }
    }
  } catch (const std::exception&) {
  }

  if (local_exists_walk_up()) {
    try {
      nlohmann::json local = yaml_file_to_json(local_config_file());
      if (!local.is_object()) {
        local = nlohmann::json::object();
      }
      bool extend = true;
      if (local.contains("extend")) {
        if (local["extend"].is_boolean()) {
          extend = local["extend"].get<bool>();
        } else if (local["extend"].is_string()) {
          extend = true;
          try {
            nlohmann::json ext = yaml_file_to_json(local["extend"].get<std::string>());
            if (ext.is_object()) {
              data_.update(ext);
            }
          } catch (...) {
          }
        }
        local.erase("extend");
      }
      if (extend) {
        data_.update(local);
      } else {
        data_ = default_data();
        data_.update(local);
      }
    } catch (const std::exception&) {
    }
  }

  if (!data_["to"].is_string() || data_["to"].get<std::string>().empty()) {
    auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::ostringstream oss;
    oss << std::format("{:%Y-%m-%dT%H:%M:%S}", now);
    data_["to"] = oss.str();
  }

  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(frame_dir(), ec);
}

void GttConfig::set_json(const std::string& key, const nlohmann::json& value, bool force) {
  if (!force && (value.is_null())) {
    return;
  }
  data_[key] = value;
}

std::string GttConfig::get_string(const std::string& key, const std::string& def) const {
  if (!data_.contains(key)) {
    return def;
  }
  const auto& v = data_.at(key);
  if (v.is_null() || (v.is_boolean() && !v.get<bool>())) {
    return def;
  }
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_boolean()) {
    return v.get<bool>() ? "true" : "false";
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<int>());
  }
  return v.dump();
}

int GttConfig::get_int(const std::string& key, int def) const {
  if (!data_.contains(key)) {
    return def;
  }
  const auto& v = data_.at(key);
  if (v.is_number_integer()) {
    return v.get<int>();
  }
  if (v.is_string()) {
    try {
      return std::stoi(v.get<std::string>());
    } catch (...) {
    }
  }
  return def;
}

bool GttConfig::get_bool(const std::string& key, bool def) const {
  if (!data_.contains(key)) {
    return def;
  }
  const auto& v = data_.at(key);
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_number()) {
    return v.get<int>() != 0;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    return s == "true" || s == "1";
  }
  return def;
}

std::vector<std::string> GttConfig::get_string_vec(const std::string& key) const {
  std::vector<std::string> out;
  if (!data_.contains(key)) {
    return out;
  }
  const auto& v = data_.at(key);
  if (v.is_array()) {
    for (const auto& el : v) {
      if (el.is_string()) {
        out.push_back(el.get<std::string>());
      }
    }
    return out;
  }
  if (v.is_string()) {
    out.push_back(v.get<std::string>());
  }
  return out;
}

std::filesystem::path GttConfig::frame_dir() const {
  if (data_.contains("frameDir") && data_["frameDir"].is_string()) {
    const std::string p = data_["frameDir"].get<std::string>();
    if (!p.empty()) {
      return std::filesystem::path(p);
    }
  }
  return global_dir_ / "frames";
}

namespace {

std::chrono::sys_seconds parse_date_string(const std::string& s) {
  using namespace std::chrono;
  int y = 1970, mo = 1, d = 1, h = 0, mi = 0, se = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 6) {
  } else if (std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 6) {
  } else if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) >= 3) {
    h = mi = se = 0;
  } else {
    return sys_seconds{};
  }
  if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) {
    return sys_seconds{};
  }
  const auto sd = sys_days{year{y} / month{unsigned(mo)} / std::chrono::day{unsigned(d)}};
  return sd + hours{h} + minutes{mi} + seconds{se};
}

}  // namespace

std::chrono::sys_seconds GttConfig::get_from() const {
  return parse_date_string(get_string("from", "1970-01-01"));
}

std::chrono::sys_seconds GttConfig::get_to() const {
  std::string t = get_string("to", "");
  if (t.empty()) {
    return std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
  }
  return parse_date_string(t);
}

std::string GttConfig::timezone_name() const {
  std::string t = get_string("timezone", "");
  if (!t.empty()) {
    return t;
  }
  try {
    return std::string(std::chrono::current_zone()->name());
  } catch (...) {
    return "UTC";
  }
}

std::string GttConfig::time_format_for(const std::string& sub_key) const {
  const auto& tf = data_["timeFormat"];
  if (tf.is_object() && tf.contains(sub_key) && tf[sub_key].is_string()) {
    return tf[sub_key].get<std::string>();
  }
  if (tf.is_string()) {
    return tf.get<std::string>();
  }
  return "[%sign][%days>d ][%hours>h ][%minutes>m ][%seconds>s]";
}

std::string GttConfig::to_human_readable(std::int64_t seconds, const std::string& time_format_sub) const {
  int hpd = get_int("hoursPerDay", 8);
  return gtt::timefmt::to_human_readable(seconds, hpd, time_format_for(time_format_sub));
}
