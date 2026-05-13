#include "time_format.hpp"

#include <cmath>
#include <regex>
#include <sstream>
#include <string>

namespace gtt::timefmt {

namespace {

std::int64_t apply_sign(bool neg, std::int64_t v) {
  return neg ? -v : v;
}

}  // namespace

std::int64_t parse_human_duration(const std::string& string, int hours_per_day, int days_per_week,
                                  int weeks_per_month) {
  static const std::regex re(
      R"(^(?:([-])\s*)?(?:(\d+)mo\s*)?(?:(\d+)w\s*)?(?:(\d+)d\s*)?(?:(\d+)h\s*)?(?:(\d+)m\s*)?(?:(\d+)s\s*)?$)");
  std::smatch m;
  if (!std::regex_match(string, m, re)) {
    return 0;
  }
  bool neg = m[1].matched && m[1].str() == "-";
  auto val = [&](int idx) -> std::int64_t {
    if (!m[idx].matched || m[idx].str().empty()) {
      return 0;
    }
    return std::stoll(m[idx].str());
  };
  std::int64_t seconds = val(7);
  seconds += val(6) * 60;
  seconds += val(5) * 3600;
  seconds += val(4) * static_cast<std::int64_t>(hours_per_day) * 3600;
  seconds += val(3) * static_cast<std::int64_t>(days_per_week) * hours_per_day * 3600;
  seconds += val(2) * static_cast<std::int64_t>(weeks_per_month) * days_per_week * hours_per_day * 3600;
  return apply_sign(neg, seconds);
}

std::string to_human_readable(std::int64_t input_seconds, int hours_per_day, const std::string& format) {
  std::string sign = input_seconds < 0 ? "-" : "";
  std::int64_t input = std::llabs(input_seconds);

  const std::int64_t seconds_in_day = 3600LL * hours_per_day;
  const std::int64_t seconds_in_hour = 3600;
  const std::int64_t seconds_in_minute = 60;

  struct Ins {
    std::string sign;
    double days_overall{};
    std::string days_overall_comma;
    std::int64_t days{};
    std::string Days;
    double hours_overall{};
    std::string hours_overall_comma;
    std::int64_t hours{};
    std::string Hours;
    double minutes_overall{};
    std::string minutes_overall_comma;
    std::int64_t minutes{};
    std::string Minutes;
    double seconds_overall{};
    std::int64_t seconds{};
    std::string Seconds;
  } ins;
  ins.sign = sign;
  ins.days_overall = static_cast<double>(input) / static_cast<double>(seconds_in_day);
  {
    std::ostringstream o;
    o << ins.days_overall;
    ins.days_overall_comma = o.str();
  }
  ins.days = input / seconds_in_day;
  {
    std::ostringstream o;
    o.width(2);
    o.fill('0');
    o << ins.days;
    ins.Days = o.str();
  }
  ins.hours_overall = static_cast<double>(input) / static_cast<double>(seconds_in_hour);
  {
    std::ostringstream o;
    o << ins.hours_overall;
    ins.hours_overall_comma = o.str();
  }
  ins.hours = (input % seconds_in_day) / seconds_in_hour;
  {
    std::ostringstream o;
    o.width(2);
    o.fill('0');
    o << ins.hours;
    ins.Hours = o.str();
  }
  ins.minutes_overall = static_cast<double>(input) / static_cast<double>(seconds_in_minute);
  {
    std::ostringstream o;
    o << ins.minutes_overall;
    ins.minutes_overall_comma = o.str();
  }
  ins.minutes = ((input % seconds_in_day) % seconds_in_hour) / seconds_in_minute;
  {
    std::ostringstream o;
    o.width(2);
    o.fill('0');
    o << ins.minutes;
    ins.Minutes = o.str();
  }
  ins.seconds_overall = static_cast<double>(input);
  ins.seconds = ((input % seconds_in_day) % seconds_in_hour) % seconds_in_minute;
  {
    std::ostringstream o;
    o.width(2);
    o.fill('0');
    o << ins.seconds;
    ins.Seconds = o.str();
  }

  auto get_insert = [&](const std::string& key) -> std::string {
    if (key == "sign") {
      return ins.sign;
    }
    if (key == "days") {
      return std::to_string(ins.days);
    }
    if (key == "Days") {
      return ins.Days;
    }
    if (key == "hours") {
      return std::to_string(ins.hours);
    }
    if (key == "Hours") {
      return ins.Hours;
    }
    if (key == "minutes") {
      return std::to_string(ins.minutes);
    }
    if (key == "Minutes") {
      return ins.Minutes;
    }
    if (key == "seconds") {
      return std::to_string(ins.seconds);
    }
    if (key == "Seconds") {
      return ins.Seconds;
    }
    if (key == "days_overall") {
      return std::to_string(ins.days_overall);
    }
    if (key == "days_overall_comma") {
      return ins.days_overall_comma;
    }
    if (key == "hours_overall") {
      return std::to_string(ins.hours_overall);
    }
    if (key == "hours_overall_comma") {
      return ins.hours_overall_comma;
    }
    if (key == "minutes_overall") {
      return std::to_string(ins.minutes_overall);
    }
    if (key == "minutes_overall_comma") {
      return ins.minutes_overall_comma;
    }
    if (key == "seconds_overall") {
      return std::to_string(ins.seconds_overall);
    }
    return "";
  };

  std::string output = format;

  {
    static const std::regex rounded(R"(\[\%([^\>\]]*)\:([^\]]*)\])");
    std::smatch m;
    std::string s = output;
    while (std::regex_search(s, m, rounded)) {
      std::string key = m[1].str();
      std::string decimals_part = m[2].str();
      int decimals = 0;
      static const std::regex dec_simple(R"(([0-9]*)\>(.*))");
      std::smatch dm;
      std::string suffix;
      if (std::regex_match(decimals_part, dm, dec_simple)) {
        if (!dm[1].str().empty()) {
          decimals = std::stoi(dm[1].str());
        }
        suffix = dm[2].str();
      } else {
        decimals = std::stoi(decimals_part);
      }
      double raw = std::stod(get_insert(key));
      double rounded_val = std::ceil(raw * std::pow(10.0, decimals)) / std::pow(10.0, decimals);
      std::string repl;
      if (rounded_val != 0.0 && !suffix.empty()) {
        repl = std::to_string(rounded_val) + suffix;
      } else {
        repl = std::to_string(rounded_val);
      }
      s.replace(m.position(), m.length(), repl);
    }
    output = s;
  }

  {
    static const std::regex cond(R"(\[\%([^\>\]]*)\>([^\]]*)\])");
    std::string s = output;
    std::smatch m;
    while (std::regex_search(s, m, cond)) {
      std::string key = m[1].str();
      std::string tail = m[2].str();
      double v = std::stod(get_insert(key));
      std::string repl = v > 0 ? (get_insert(key) + tail) : "";
      s.replace(m.position(), m.length(), repl);
    }
    output = s;
  }

  {
    static const std::regex def(R"(\[\%([^\]]*)\])");
    std::string s = output;
    std::smatch m;
    while (std::regex_search(s, m, def)) {
      std::string repl = get_insert(m[1].str());
      s.replace(m.position(), m.length(), repl);
    }
    output = s;
  }

  while (!output.empty() && output.front() == ' ') {
    output.erase(output.begin());
  }
  while (!output.empty() && output.back() == ' ') {
    output.pop_back();
  }
  return output;
}

}  // namespace gtt::timefmt
