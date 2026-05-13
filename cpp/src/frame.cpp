#include "frame.hpp"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "config.hpp"
#include "hashids_encode.hpp"

namespace {

bool parse_ts(const std::string& s, int& y, int& mo, int& d, int& h, int& mi, int& se) {
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 6) {
    return true;
  }
  if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) >= 3) {
    h = mi = se = 0;
    return true;
  }
  return false;
}

bool parse_offset_minutes(const std::string& s, int& offset_minutes_east) {
  if (s.size() < 2) {
    return false;
  }
  const char sign = s[0];
  if (sign != '+' && sign != '-') {
    return false;
  }
  std::size_t colon = s.find(':');
  int oh = 0;
  int om = 0;
  try {
    if (colon != std::string::npos) {
      oh = std::stoi(s.substr(1, colon - 1));
      om = std::stoi(s.substr(colon + 1));
    } else if (s.size() >= 5) {
      oh = std::stoi(s.substr(1, 2));
      om = std::stoi(s.substr(3, 2));
    } else {
      return false;
    }
  } catch (...) {
    return false;
  }
  offset_minutes_east = (sign == '+' ? 1 : -1) * (oh * 60 + om);
  return true;
}

std::chrono::sys_seconds utc_midnight(int y, int mo, int d) {
  using namespace std::chrono;
  return time_point_cast<seconds>(sys_days(year{y} / month{unsigned(mo)} / day{unsigned(d)}));
}

std::chrono::sys_seconds utc_from_zulu_components(int y, int mo, int d, int h, int mi, int se) {
  using namespace std::chrono;
  return utc_midnight(y, mo, d) + hours{h} + minutes{mi} + seconds{se};
}

std::chrono::sys_seconds utc_from_offset_wall(int y, int mo, int d, int h, int mi, int se, int offset_minutes_east) {
  using namespace std::chrono;
  return utc_midnight(y, mo, d) + seconds(h * 3600 + mi * 60 + se - offset_minutes_east * 60);
}

/** Parse frame timestamps like JavaScript Date / moment: Zulu, explicit offset, or naive wall clock in tz_name. */
std::chrono::sys_seconds parse_iso_to_sys(const std::string& iso_in, const std::string& tz_name) {
  using namespace std::chrono;
  std::string s = iso_in;
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
    s.pop_back();
  }

  bool zulu = false;
  if (!s.empty() && (s.back() == 'Z' || s.back() == 'z')) {
    zulu = true;
    s.pop_back();
  }

  const auto dot = s.find('.');
  if (dot != std::string::npos) {
    s.resize(dot);
  }

  const size_t tpos = s.find('T');
  size_t signpos = std::string::npos;
  if (tpos != std::string::npos) {
    for (size_t i = tpos + 1; i < s.size(); ++i) {
      if (s[i] == '+' || s[i] == '-') {
        signpos = i;
      }
    }
  }

  const std::string main_part = signpos == std::string::npos ? s : s.substr(0, signpos);
  const std::string offset_part = signpos == std::string::npos ? std::string{} : std::string{s.substr(signpos)};

  int y = 0;
  int mo = 0;
  int d = 0;
  int h = 0;
  int mi = 0;
  int se = 0;
  if (std::sscanf(main_part.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 6) {
  } else if (std::sscanf(main_part.c_str(), "%d-%d-%d", &y, &mo, &d) >= 3) {
    h = mi = se = 0;
  } else {
    return sys_seconds{};
  }

  if (zulu) {
    return utc_from_zulu_components(y, mo, d, h, mi, se);
  }
  if (!offset_part.empty()) {
    int off_m = 0;
    if (parse_offset_minutes(offset_part, off_m)) {
      return utc_from_offset_wall(y, mo, d, h, mi, se, off_m);
    }
  }

  try {
    const std::chrono::time_zone* z = std::chrono::locate_zone(tz_name);
    const auto ld = std::chrono::local_days(year{y} / month{unsigned(mo)} / day{unsigned(d)});
    const local_seconds ls = floor<seconds>(ld + hours{h} + minutes{mi} + seconds{se});
    return z->to_sys(ls, choose::latest);
  } catch (...) {
    return utc_from_zulu_components(y, mo, d, h, mi, se);
  }
}

std::string format_zoned_hh_mm(std::chrono::sys_seconds tp, const std::string& tz_name) {
  using namespace std::chrono;
  try {
    const std::chrono::time_zone* z = std::chrono::locate_zone(tz_name);
    const zoned_time zt{z, tp};
    return std::format("{:%H:%M}", zt);
  } catch (...) {
    return std::format("{:%H:%M}", tp);
  }
}

std::string format_zoned_ymd(std::chrono::sys_seconds tp, const std::string& tz_name) {
  using namespace std::chrono;
  try {
    const std::chrono::time_zone* z = std::chrono::locate_zone(tz_name);
    const zoned_time zt{z, tp};
    return std::format("{:%Y-%m-%d}", zt);
  } catch (...) {
    return std::format("{:%Y-%m-%d}", tp);
  }
}

std::string now_iso_zoned(const GttConfig& cfg) {
  using namespace std::chrono;
  try {
    const std::chrono::time_zone* z = std::chrono::locate_zone(cfg.timezone_name());
    std::chrono::zoned_time zt{z, floor<seconds>(system_clock::now())};
    return std::format("{:%Y-%m-%dT%H:%M:%S}", zt);
  } catch (...) {
    auto now = floor<seconds>(system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%S}", now);
  }
}

}  // namespace

std::string frame_generate_new_id() {
  return gtt_encode_frame_id(gtt_now_millis_id_seed());
}

void Frame::validate() const {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (!parse_ts(start_iso, y, mo, d, h, mi, se)) {
    throw std::runtime_error("Error: Start date is not in a valid ISO date format!");
  }
  if (stop_iso && !stop_iso->empty()) {
    if (!parse_ts(*stop_iso, y, mo, d, h, mi, se)) {
      throw std::runtime_error("Error: Stop date is not in a valid ISO date format!");
    }
  }
}

Frame Frame::from_json(GttConfig& cfg, const nlohmann::json& j) {
  Frame f;
  f.id = j.at("id").get<std::string>();
  f.project = j.at("project").get<std::string>();
  f.resource = j.at("resource");
  f.notes = j.value("notes", nlohmann::json::array());
  f.start_iso = j.at("start").get<std::string>();
  if (j.contains("stop") && !j["stop"].is_null() && j["stop"].is_boolean() && j["stop"].get<bool>() == false) {
    f.stop_iso = std::nullopt;
  } else if (j.contains("stop") && !j["stop"].is_null() && j["stop"].is_string()) {
    f.stop_iso = j["stop"].get<std::string>();
  } else {
    f.stop_iso = std::nullopt;
  }
  f.timezone = j.value("timezone", cfg.timezone_name());
  if (j.contains("modified") && j["modified"].is_string()) {
    f.modified_iso = j["modified"].get<std::string>();
  }
  if (j.contains("title") && !j["title"].is_null()) {
    f.title = j["title"].get<std::string>();
  }
  if (j.contains("note") && !j["note"].is_null()) {
    f.note = j["note"].get<std::string>();
  }
  f.validate();
  return f;
}

Frame Frame::from_file(GttConfig& cfg, const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read frame");
  }
  nlohmann::json j = nlohmann::json::parse(in);
  return from_json(cfg, j);
}

std::filesystem::path Frame::file_path(const GttConfig& cfg) const {
  return cfg.frame_dir() / (id + ".json");
}

void Frame::write(GttConfig& cfg, bool skip_modified) const {
  nlohmann::json j;
  j["id"] = id;
  j["project"] = project;
  j["resource"] = resource;
  j["notes"] = notes;
  j["start"] = start_iso;
  j["stop"] = stop_iso ? nlohmann::json(*stop_iso) : nlohmann::json(nullptr);
  j["timezone"] = timezone;
  j["modified"] = skip_modified && modified_iso ? *modified_iso : now_iso_zoned(cfg);
  j["title"] = title ? nlohmann::json(*title) : nlohmann::json(nullptr);
  j["note"] = note ? nlohmann::json(*note) : nlohmann::json(nullptr);
  auto p = file_path(cfg);
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::ofstream out(p);
  out << j.dump(2);
}

std::int64_t Frame::duration_seconds(const GttConfig& cfg) const {
  (void)cfg;
  if (!stopped()) {
    return 0;
  }
  const auto t0 = start_sys();
  const auto t1 = parse_iso_to_sys(*stop_iso, timezone);
  return (t1 - t0).count();
}

Frame Frame::create_started(GttConfig& cfg, const std::string& project_path, const std::string& resource_type,
                            const nlohmann::json& resource_id, const std::optional<std::string>& note_text) {
  Frame f;
  f.id = frame_generate_new_id();
  f.project = project_path;
  f.resource = {{"id", resource_id}, {"type", resource_type}};
  if (resource_id.is_string()) {
    f.resource["new"] = true;
  }
  f.notes = nlohmann::json::array();
  f.start_iso = now_iso_zoned(cfg);
  f.stop_iso = std::nullopt;
  f.timezone = cfg.timezone_name();
  f.note = note_text;
  f.write(cfg, false);
  return f;
}

void Frame::stop_now(GttConfig& cfg) {
  stop_iso = now_iso_zoned(cfg);
  write(cfg, false);
}

int Frame::notes_time_total() const {
  int s = 0;
  if (!notes.is_array()) {
    return 0;
  }
  for (const auto& n : notes) {
    if (n.contains("time") && n["time"].is_number_integer()) {
      s += n.at("time").get<int>();
    }
  }
  return s;
}

std::string Frame::log_date_key(const GttConfig& cfg) const {
  const auto tp = parse_iso_to_sys(start_iso, timezone);
  return format_zoned_ymd(tp, cfg.timezone_name());
}

std::chrono::sys_seconds Frame::start_sys() const {
  return parse_iso_to_sys(start_iso, timezone);
}

std::chrono::sys_seconds Frame::stop_sys() const {
  if (!stopped()) {
    return {};
  }
  return parse_iso_to_sys(*stop_iso, timezone);
}

std::string Frame::iso_hh_mm(const std::string& iso, const std::string& stored_tz, const std::string& display_tz) {
  const auto tp = parse_iso_to_sys(iso, stored_tz);
  return format_zoned_hh_mm(tp, display_tz);
}

std::int64_t Frame::ceil_duration_seconds(const GttConfig& cfg) const {
  const std::int64_t d = duration_seconds(cfg);
  return static_cast<std::int64_t>(std::ceil(static_cast<double>(d)));
}
