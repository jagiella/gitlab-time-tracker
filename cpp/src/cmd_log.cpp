#include "cmd_log.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "config.hpp"
#include "frame.hpp"
#include "tasks.hpp"
#include "time_format.hpp"

namespace {

bool tty_out() {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) != 0;
#else
  return ::isatty(STDOUT_FILENO) != 0;
#endif
}

std::string ansi(const char* code, const std::string& s, bool color) {
  if (!color) {
    return s;
  }
  static const char* rst = "\033[0m";
  return std::string(code) + s + rst;
}

std::string column_str(std::string s, std::size_t w) {
  if (s.size() <= w) {
    return s + std::string(w - s.size(), ' ');
  }
  if (w == 0) {
    return "";
  }
  if (w == 1) {
    return "…";
  }
  return s.substr(0, w - 1) + "…";
}

std::string ordinal_en(int d) {
  if (d % 10 == 1 && d != 11) {
    return "st";
  }
  if (d % 10 == 2 && d != 12) {
    return "nd";
  }
  if (d % 10 == 3 && d != 13) {
    return "rd";
  }
  return "th";
}

/** Entspricht moment(..., 'MMMM Do YYYY') (englisch). */
std::string format_day_title(const std::string& ymd) {
  int y = 0, m = 0, d = 0;
  if (std::sscanf(ymd.c_str(), "%d-%d-%d", &y, &m, &d) < 3) {
    return ymd;
  }
  static const char* mon[] = {"",    "January", "February", "March",     "April",   "May",      "June",
                              "July", "August",   "September", "October", "November", "December"};
  if (m < 1 || m > 12) {
    return ymd;
  }
  return std::string(mon[m]) + " " + std::to_string(d) + ordinal_en(d) + " " + std::to_string(y);
}

std::string resource_id_display(const Frame& fr) {
  const auto& idj = fr.resource.at("id");
  if (idj.is_string()) {
    return idj.get<std::string>();
  }
  return std::to_string(idj.get<int>());
}

bool is_new_resource_row(const Frame& fr) {
  if (fr.resource.contains("new") && fr.resource["new"].is_boolean() && fr.resource["new"].get<bool>()) {
    return true;
  }
  return fr.resource.at("id").is_string();
}

std::string csv_escape(const std::string& s) {
  if (s.find_first_of(",\"\r\n") == std::string::npos) {
    return s;
  }
  std::string o = "\"";
  for (char c : s) {
    if (c == '"') {
      o += "\"\"";
    } else {
      o += c;
    }
  }
  o += '"';
  return o;
}

}  // namespace

void run_cmd_log(GttConfig& cfg, bool csv, const std::optional<int>& hours_per_day,
                 const std::optional<std::string>& time_format, bool verbose) {
  (void)verbose;
  const int hpd = hours_per_day.value_or(cfg.get_int("hoursPerDay", 8));
  std::string tf = time_format.value_or(cfg.time_format_for("log"));

  Tasks t(cfg, nullptr);
  Tasks::LogCollected data = t.collect_log();

  if (csv) {
    std::cout << "frameId, project, issueid, date, starttime, endtime, duration (s), title, note\n";
    std::vector<std::string> keys;
    for (const auto& kv : data.frames) {
      keys.push_back(kv.first);
    }
    std::sort(keys.begin(), keys.end());
    for (const std::string& date : keys) {
      for (const Frame& fr : data.frames.at(date)) {
        const std::string rid = resource_id_display(fr);
        const std::string title = fr.title.value_or("");
        const std::string note = fr.note.value_or("");
        // Match Node gtt-log.js: comma + space between fields
        std::cout << csv_escape(fr.id) << ", " << csv_escape(fr.project) << ", " << csv_escape(rid) << ", "
                  << date << ", " << Frame::iso_hh_mm(fr.start_iso, fr.timezone, cfg.timezone_name()) << ", "
                  << Frame::iso_hh_mm(*fr.stop_iso, fr.timezone, cfg.timezone_name()) << ", "
                  << fr.duration_seconds(cfg) << ", " << csv_escape(title) << ", " << csv_escape(note) << "\n";
      }
    }
    return;
  }

  const bool color = tty_out();
  std::vector<std::string> keys;
  for (const auto& kv : data.frames) {
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());

  for (const std::string& date : keys) {
    const std::int64_t day_sec = data.times.at(date);
    std::string day_note;
    const double hpd_sec = static_cast<double>(hpd) * 3600.0;
    if (day_sec > static_cast<std::int64_t>(hpd_sec * 2.0 + 0.5)) {
      day_note = ansi("\033[31m", " - worked over " + std::to_string(hpd * 2) + " hours", color);
    } else if (day_sec > static_cast<std::int64_t>(hpd_sec * 1.5 + 0.5)) {
      day_note = ansi("\033[33m", " - worked over " + std::to_string(static_cast<int>(hpd * 1.5)) + " hours", color);
    } else if (day_sec > static_cast<std::int64_t>(hpd_sec * 1.1 + 0.5)) {
      day_note = ansi("\033[32m", " - worked over " + std::to_string(static_cast<int>(hpd * 1.1)) + " hours", color);
    }

    const std::string day_human =
        gtt::timefmt::to_human_readable(day_sec, hpd, tf.empty() ? "[%sign][%days>d ][%hours>h ][%minutes>m ][%seconds>s]" : tf);
    std::cout << ansi("\033[32m", format_day_title(date) + " (" + day_human + ")", color) << day_note << "\n";

    for (const Frame& fr : data.frames.at(date)) {
      const std::int64_t ceil_d = fr.ceil_duration_seconds(cfg);
      const int noted = fr.notes_time_total();
      const bool to_sync = (ceil_d - noted) != 0;
      std::string dur_txt =
          gtt::timefmt::to_human_readable(ceil_d, hpd, tf.empty() ? "[%sign][%days>d ][%hours>h ][%minutes>m ][%seconds>s]" : tf);
      dur_txt = column_str(dur_txt, 14);
      if (to_sync) {
        dur_txt = ansi("\033[33m", dur_txt, color);
      }

      const std::string typ = fr.resource.at("type").get<std::string>();
      const std::string rid = resource_id_display(fr);
      std::string issue_cell;
      if (is_new_resource_row(fr)) {
        const std::string raw = "(new " + typ + " \"" + rid + "\")";
        issue_cell = ansi("\033[44;37m", column_str(raw, 70), color);
      } else {
        const std::string left = typ + " #" + rid;
        issue_cell = ansi("\033[34m", column_str(left, 20), color) + column_str(fr.title.value_or(""), 50);
      }

      const std::string t0 = ansi("\033[32m", Frame::iso_hh_mm(fr.start_iso, fr.timezone, cfg.timezone_name()), color);
      const std::string t1 = ansi("\033[32m", Frame::iso_hh_mm(*fr.stop_iso, fr.timezone, cfg.timezone_name()), color);
      const std::string proj_col = ansi("\033[35m", column_str(fr.project, 50), color);
      const std::string note_tail = fr.note.value_or("");

      std::cout << "  " << fr.id << "  " << t0 << " to " << t1 << '\t' << dur_txt << proj_col << issue_cell << note_tail
                << "\n";
    }
  }
}
