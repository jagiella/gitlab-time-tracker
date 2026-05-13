#include "commands.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <cstdio>
#include <optional>
#include <vector>
#ifdef _WIN32
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "cmd_log.hpp"
#include "config.hpp"
#include "frame.hpp"
#include "gitlab_client.hpp"
#include "report.hpp"
#include "tasks.hpp"
#include "tty_interactive.hpp"

namespace {

using std::chrono::days;
using std::chrono::duration_cast;
using std::chrono::floor;
using std::chrono::hh_mm_ss;
using std::chrono::local_days;
using std::chrono::local_seconds;
using std::chrono::locate_zone;
using std::chrono::seconds;
using std::chrono::sys_seconds;
using std::chrono::time_zone;
using std::chrono::year_month_day;
using std::chrono::zoned_time;

std::string english_ordinal_day(unsigned d) {
  if (d >= 11 && d <= 13) {
    return std::to_string(d) + "th";
  }
  switch (d % 10) {
    case 1:
      return std::to_string(d) + "st";
    case 2:
      return std::to_string(d) + "nd";
    case 3:
      return std::to_string(d) + "rd";
    default:
      return std::to_string(d) + "th";
  }
}

const char* english_month_full(unsigned m) {
  static const char* k_names[] = {nullptr,     "January",  "February", "March",  "April",    "May",      "June",
                                  "July",     "August",   "September", "October", "November", "December"};
  if (m < 1 || m > 12) {
    return "?";
  }
  return k_names[m];
}

/** Wie Node `moment`: "May 11th 2026 09:51" in der Anzeige-Zeitzone. */
std::string format_moment_do_yyyy_hhmm(sys_seconds tp, const std::string& display_tz) {
  if (tp == sys_seconds{}) {
    return "?";
  }
  try {
    const time_zone* z = locate_zone(display_tz);
    const zoned_time zt{z, tp};
    const local_seconds lt = zt.get_local_time();
    const auto ld = floor<days>(lt);
    const year_month_day ymd{local_days{ld}};
    if (!ymd.ok()) {
      return "?";
    }
    const auto y = static_cast<int>(ymd.year());
    const unsigned mo = static_cast<unsigned>(ymd.month());
    const unsigned day = static_cast<unsigned>(ymd.day());
    const local_seconds midnight{local_days{ld}};
    const hh_mm_ss<seconds> hms{duration_cast<seconds>(lt - midnight)};
    const int H = static_cast<int>(hms.hours().count());
    const int M = static_cast<int>(hms.minutes().count());
    std::ostringstream os;
    os << english_month_full(mo) << ' ' << english_ordinal_day(day) << ' ' << y << ' ' << std::setfill('0')
       << std::setw(2) << H << ':' << std::setw(2) << M;
    return os.str();
  } catch (...) {
    return "?";
  }
}

bool tty_stdout() {
#ifdef _WIN32
  return _isatty(_fileno(stdout)) != 0;
#else
  return ::isatty(STDOUT_FILENO) != 0;
#endif
}

/** Wrap segment with ANSI color when stdout is a TTY (matches Node `colors` package). */
std::string ansi_colored(const char* code, const std::string& s, bool color) {
  if (!color) {
    return s;
  }
  return std::string(code) + s + "\033[0m";
}

GttConfig& global_cfg() {
  static GttConfig cfg{std::filesystem::current_path()};
  return cfg;
}

/** Wall-clock HH:mm in cfg timezone (matches Node resume success line). */
std::string now_hh_mm_display(const GttConfig& cfg) {
  using namespace std::chrono;
  const auto now = floor<seconds>(system_clock::now());
  try {
    const std::chrono::time_zone* z = std::chrono::locate_zone(cfg.timezone_name());
    const std::chrono::zoned_time zt{z, now};
    return std::format("{:%H:%M}", zt);
  } catch (...) {
    return std::format("{:%H:%M}", now);
  }
}

std::string resource_id_plain(const nlohmann::json& idj) {
  if (idj.is_number_integer()) {
    return std::to_string(idj.get<int>());
  }
  if (idj.is_string()) {
    return idj.get<std::string>();
  }
  return idj.dump();
}

std::string trim_ws(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && !not_space(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

bool parse_uint_strict(const std::string& s, std::size_t& out) {
  if (s.empty()) {
    return false;
  }
  std::size_t pos = 0;
  try {
    const unsigned long v = std::stoul(s, &pos);
    if (pos != s.size()) {
      return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

#ifndef _WIN32
std::string shell_single_quote_path(const std::filesystem::path& p) {
  const std::string s = p.string();
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

void launch_editor_path(const std::filesystem::path& path) {
  const char* ed = std::getenv("VISUAL");
  if (!ed || !*ed) {
    ed = std::getenv("EDITOR");
  }
  if (ed && *ed) {
    const std::string cmd = std::string(ed) + " " + shell_single_quote_path(path);
    const pid_t pid = fork();
    if (pid == 0) {
      execlp("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
      std::perror("execlp");
      _exit(127);
    }
    if (pid < 0) {
      std::perror("fork");
      std::exit(1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return;
  }
  const pid_t pid = fork();
  if (pid == 0) {
    execlp("xdg-open", "xdg-open", path.c_str(), static_cast<char*>(nullptr));
    execlp("open", "open", path.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid > 0) {
    waitpid(pid, nullptr, 0);
  }
}
#else
void launch_editor_path(const std::filesystem::path& path) {
  const char* ed = std::getenv("VISUAL");
  if (!ed || !*ed) {
    ed = std::getenv("EDITOR");
  }
  const std::string p = path.string();
  if (ed && *ed) {
    const std::string cmd = std::string("\"") + ed + "\" \"" + p + "\"";
    std::system(cmd.c_str());
  } else {
    std::system(("start \"\" \"" + p + "\"").c_str());
  }
}
#endif

/** Terminal display width (columns); requires UTF-8 locale for multibyte text. */
int utf8_display_width(const std::string& s) {
  std::mbstate_t st{};
  std::memset(&st, 0, sizeof(st));
  int total = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    wchar_t wc = 0;
    const std::size_t n = std::mbrtowc(&wc, s.c_str() + i, s.size() - i, &st);
    if (n == 0) {
      break;
    }
    if (n == static_cast<std::size_t>(-1)) {
      total += 1;
      i += 1;
      std::memset(&st, 0, sizeof(st));
      continue;
    }
    if (n == static_cast<std::size_t>(-2)) {
      break;
    }
    const int cw = wcwidth(wc);
    total += (cw < 0) ? 1 : cw;
    i += n;
  }
  return total;
}

std::string utf8_trunc_to_visual_width(const std::string& s, std::size_t max_cols) {
  if (utf8_display_width(s) <= static_cast<int>(max_cols)) {
    return s;
  }
  std::mbstate_t st{};
  std::memset(&st, 0, sizeof(st));
  std::string out;
  int total = 0;
  std::size_t i = 0;
  while (i < s.size()) {
    wchar_t wc = 0;
    const std::size_t n = std::mbrtowc(&wc, s.c_str() + i, s.size() - i, &st);
    if (n == 0) {
      break;
    }
    if (n == static_cast<std::size_t>(-1)) {
      if (total + 1 > static_cast<int>(max_cols)) {
        break;
      }
      out += s[i];
      total += 1;
      i += 1;
      std::memset(&st, 0, sizeof(st));
      continue;
    }
    if (n == static_cast<std::size_t>(-2)) {
      break;
    }
    const int cw = wcwidth(wc);
    const int w = (cw < 0) ? 1 : cw;
    if (total + w > static_cast<int>(max_cols)) {
      break;
    }
    out.append(s, i, n);
    total += w;
    i += n;
  }
  return out;
}

std::string utf8_fit_visual_width(const std::string& s, std::size_t cols) {
  std::string t = utf8_trunc_to_visual_width(s, cols);
  int w = utf8_display_width(t);
  while (w < static_cast<int>(cols)) {
    t += ' ';
    w += 1;
  }
  return t;
}

/**
 * Eine Zeile für `gtt edit` — wie Node/Inquirer (`gtt-edit.js`): Moment-Datum, keine Nummer im TTY.
 * @param fallback_index gesetzt → Zeilennummer wie bei Fallback-Eingabe (1–n)
 * @param inquirer_selected_row true → Cyan auf Frame-ID, „ to “ und Titelspalte (@inquirer/select)
 */
std::string format_edit_menu_line(const Frame& fr, const std::string& tz, bool use_color,
                                  std::optional<unsigned> fallback_index, bool inquirer_selected_row) {
  const bool cyan_row = inquirer_selected_row && !fallback_index.has_value();
  std::ostringstream row;
  if (fallback_index.has_value()) {
    row << std::setw(6) << std::right << *fallback_index << "  ";
  }
  if (use_color && cyan_row) {
    row << "\033[36m";
  }
  row << fr.id << "  ";
  if (use_color && cyan_row) {
    row << "\033[0m";
  }

  const std::string start_human = format_moment_do_yyyy_hhmm(fr.start_sys(), tz);
  if (use_color) {
    row << "\033[32m";
  }
  row << start_human;
  if (fr.stopped()) {
    const std::string stop_hh = Frame::iso_hh_mm(*fr.stop_iso, fr.timezone, tz);
    if (use_color && cyan_row) {
      // „ to “ wie Frame-ID / Titel (Cyan); Endzeit bleibt Grün
      row << "\033[0m\033[36m to \033[0m\033[32m" << stop_hh;
    } else if (use_color) {
      // „ to “ wie ID/Titel (Standardfarbe), nicht Grün
      row << "\033[0m to \033[32m" << stop_hh;
    } else {
      row << " to " << stop_hh;
    }
  } else {
    if (use_color) {
      row << "\033[0m";
    }
    row << " (running)";
  }
  row << '\t';
  if (use_color) {
    row << "\033[35m";
  }
  row << utf8_fit_visual_width(fr.project, 50);
  const std::string typ = fr.resource.at("type").get<std::string>();
  const std::string rid = resource_id_plain(fr.resource.at("id"));
  if (use_color) {
    row << "\033[34m";
  }
  row << utf8_fit_visual_width(typ + " #" + rid, 20);
  if (use_color) {
    row << "\033[0m";
  }
  if (use_color && cyan_row) {
    row << "\033[36m";
  }
  row << utf8_fit_visual_width(fr.title.value_or(""), 50);
  if (use_color && cyan_row) {
    row << "\033[0m";
  }
  if (fr.note.has_value() && !fr.note->empty()) {
    row << *fr.note;
  }
  return row.str();
}

/** UTF-8 box-drawing repeat (U+2500). */
std::string box_h_times(std::size_t n) {
  static const unsigned char k_h[] = {0xe2, 0x94, 0x80};
  const std::string one(reinterpret_cast<const char*>(k_h), sizeof k_h);
  std::string out;
  out.reserve(one.size() * n);
  for (std::size_t i = 0; i < n; ++i) {
    out += one;
  }
  return out;
}

/** Tabellenlayout wie Node `cli-table` bei `gtt list` (inkl. zweiter Zeile mit web_url). */
void print_issues_table_like_node(const nlohmann::json& issues) {
  struct Row {
    int iid = 0;
    std::string title;
    std::string url;
    std::string state;
  };
  std::vector<Row> rows;
  for (const auto& is : issues) {
    if (!is.is_object()) {
      continue;
    }
    Row r;
    r.iid = is.value("iid", 0);
    r.title = is.value("title", std::string());
    r.url = is.value("web_url", std::string());
    r.state = is.value("state", std::string());
    rows.push_back(std::move(r));
  }
  if (rows.empty()) {
    return;
  }

  std::size_t w1 = 3;
  std::size_t w3 = 6;
  for (const auto& r : rows) {
    const std::string iid_s = std::to_string(r.iid);
    w1 = std::max(w1, static_cast<std::size_t>(utf8_display_width(iid_s)));
    w3 = std::max(w3, static_cast<std::size_t>(utf8_display_width(r.state)));
  }
  std::size_t w2 = 10;
  for (const auto& r : rows) {
    w2 = std::max(w2, static_cast<std::size_t>(utf8_display_width(r.title)));
    w2 = std::max(w2, static_cast<std::size_t>(utf8_display_width(r.url)));
  }

  const bool use_color = tty_stdout();

  static const std::string k_tl("\xe2\x94\x8c", 3);
  static const std::string k_tm("\xe2\x94\xac", 3);
  static const std::string k_tr("\xe2\x94\x90", 3);
  static const std::string k_v("\xe2\x94\x82", 3);
  static const std::string k_bl("\xe2\x94\x94", 3);
  static const std::string k_bm("\xe2\x94\xb4", 3);
  static const std::string k_br("\xe2\x94\x98", 3);

  const std::size_t seg1 = w1 + 2;
  const std::size_t seg2 = w2 + 2;
  const std::size_t seg3 = w3 + 2;

  std::cout << k_tl << box_h_times(seg1) << k_tm << box_h_times(seg2) << k_tm << box_h_times(seg3) << k_tr << "\n";

  for (const auto& r : rows) {
    std::string c1 = utf8_fit_visual_width(std::to_string(r.iid), w1);
    std::string c2a = utf8_fit_visual_width(r.title, w2);
    std::string c3 = utf8_fit_visual_width(r.state, w3);
    if (use_color) {
      c1 = ansi_colored("\033[35m", c1, true);
      c2a = ansi_colored("\033[32m", c2a, true);
    }
    std::cout << k_v << " " << c1 << " " << k_v << " " << c2a << " " << k_v << " " << c3 << " " << k_v << "\n";

    std::string c2b = utf8_fit_visual_width(r.url, w2);
    if (use_color) {
      c2b = ansi_colored("\033[90m", c2b, true);
    }
    std::cout << k_v << " " << std::string(w1, ' ') << " " << k_v << " " << c2b << " " << k_v << " " << std::string(w3, ' ')
              << " " << k_v << "\n";
  }

  std::cout << k_bl << box_h_times(seg1) << k_bm << box_h_times(seg2) << k_bm << box_h_times(seg3) << k_br << "\n";
}

void apply_report_dates(GttConfig& cfg, bool today, bool this_week, bool this_month, bool last_month) {
  if (!today && !this_week && !this_month && !last_month) {
    return;
  }
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto d0 = floor<days>(now);
  if (today) {
    cfg.set_json("from", std::format("{:%Y-%m-%dT00:00:00}", d0));
    cfg.set_json("to", std::format("{:%Y-%m-%dT23:59:59}", d0));
    return;
  }
  if (this_week) {
    cfg.set_json("from", std::format("{:%Y-%m-%dT00:00:00}", d0 - days{6}));
    cfg.set_json("to", std::format("{:%Y-%m-%dT23:59:59}", d0));
    return;
  }
  if (!this_month && !last_month) {
    return;
  }
  std::time_t tt = system_clock::to_time_t(now);
  std::tm lt{};
#if defined(_WIN32)
  localtime_s(&lt, &tt);
#else
  localtime_r(&tt, &lt);
#endif
  int y = lt.tm_year + 1900;
  int m = lt.tm_mon + 1;
  if (last_month) {
    if (--m == 0) {
      m = 12;
      --y;
    }
  }
  static const int mdays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int dim = mdays[m];
  if (m == 2) {
    const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    if (leap) {
      dim = 29;
    }
  }
  char from_buf[40], to_buf[40];
  std::snprintf(from_buf, sizeof(from_buf), "%04d-%02d-01T00:00:00", y, m);
  std::snprintf(to_buf, sizeof(to_buf), "%04d-%02d-%02dT23:59:59", y, m, dim);
  cfg.set_json("from", std::string(from_buf));
  cfg.set_json("to", std::string(to_buf));
}

/** Eine Zeile pro Subcommand — Usage-Zeile der Haupt-Hilfe und Validierung von `gtt help <cmd>`. */
struct GttSubcommandHelp {
  const char* name;
  const char* usage_suffix;
};

constexpr GttSubcommandHelp k_gtt_subcommands[] = {
    {"start", "[options] [project] [id]"},
    {"stop", "[options]"},
    {"cancel", "[options]"},
    {"status", "[options]"},
    {"sync", "[options]"},
    {"list", "[options] [project]"},
    {"report", "[options] [project] [ids...]"},
    {"config", "[options]"},
    {"log", "[options]"},
    {"resume", "[options] [project]"},
    {"edit", "[id]"},
    {"create", "[options] [project] [title]"},
    {"delete", "[id]"},
};

/// Commander-style usage suffix for the top-level `gtt` help (see Node `gtt --help`).
std::string gtt_subcommand_usage_extra(const CLI::App* sub) {
  for (const auto& e : k_gtt_subcommands) {
    if (sub->get_name() == e.name) {
      return std::string{" "} + e.usage_suffix;
    }
  }
  return {};
}

class GttFormatter : public CLI::Formatter {
 public:
  std::string make_subcommand(const CLI::App* sub) const override {
    std::stringstream out;
    const std::string req = sub->get_required() ? (" " + get_label("REQUIRED")) : "";
    const std::string extra = gtt_subcommand_usage_extra(sub);
    CLI::detail::format_help(out, sub->get_display_name(true) + req + extra, sub->get_description(), column_width_);
    return out.str();
  }
};

}  // namespace

const std::vector<std::string>& gtt_cli_subcommand_names() {
  static const std::vector<std::string> k = [] {
    std::vector<std::string> out;
    out.reserve(sizeof(k_gtt_subcommands) / sizeof(k_gtt_subcommands[0]));
    for (const auto& e : k_gtt_subcommands) {
      out.emplace_back(e.name);
    }
    return out;
  }();
  return k;
}

int run_gtt_cli(CLI::App& app, int argc, char** argv) {
  auto fmt = std::make_shared<GttFormatter>();
  fmt->column_width(48);
  app.formatter(fmt);
  app.group("Commands");

  auto* start = app.add_subcommand("start", "start monitoring time for a project resource");
  std::string st_project, st_type = "issue", st_note;
  int st_id = 0;
  bool st_m = false, st_i = false;
  start->add_option("project", st_project);
  start->add_option("id", st_id);
  start->add_option("-t,--type", st_type);
  start->add_flag("-m", st_m, "merge_request");
  start->add_flag("-i", st_i, "issue");
  start->add_option("--note", st_note);
  start->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    if (st_m) {
      st_type = "merge_request";
    }
    if (st_i) {
      st_type = "issue";
    }
    std::string proj = st_project;
    if (proj.empty() && cfg.raw().contains("project") && cfg.raw()["project"].is_string()) {
      proj = cfg.raw()["project"].get<std::string>();
    }
    if (proj.empty()) {
      std::cerr << "No project set\n";
      std::exit(2);
    }
    if (!st_id) {
      std::cerr << "Wrong or missing issue/merge_request id\n";
      std::exit(2);
    }
    Tasks t(cfg, nullptr);
    nlohmann::json jid = st_id;
    t.start(proj, st_type, jid, st_note.empty() ? std::nullopt : std::optional<std::string>(st_note));
    std::cout << "Started tracking " << proj << " " << st_type << " #" << st_id << "\n";
  });

  auto* stop = app.add_subcommand("stop", "stop monitoring time");
  stop->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    Tasks t(cfg, nullptr);
    t.stop();
    std::cout << "Stopped.\n";
  });

  auto* cancel = app.add_subcommand("cancel", "cancel monitoring without saving");
  cancel->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    Tasks t(cfg, nullptr);
    t.cancel();
    std::cout << "Cancelled.\n";
  });

  auto* status = app.add_subcommand("status", "show if time monitoring is running");
  bool st_short = false;
  status->add_flag("-s", st_short, "short output");
  status->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    Tasks t(cfg, nullptr);
    auto frames = t.status();
    if (frames.empty()) {
      std::cout << (st_short ? "gtt idle \n" : "No projects are started right now.\n");
      return;
    }
    for (const auto& fr : frames) {
      std::cout << fr.project << " " << fr.resource.at("type").get<std::string>() << " #"
                << fr.resource.at("id").dump() << " (id " << fr.id << ")\n";
    }
  });

  auto* sync = app.add_subcommand("sync", "sync local time records to GitLab");
  std::string sync_url, sync_token;
  sync->add_option("--url", sync_url);
  sync->add_option("--token", sync_token);
  sync->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    if (!sync_url.empty()) {
      cfg.set_json("url", sync_url);
    }
    if (!sync_token.empty()) {
      cfg.set_json("token", sync_token);
    }
    GitlabClient cli(cfg);
    auto auth = cli.get("broadcast_messages", 1, 1);
    if (auth.status == 401) {
      std::cerr << "Invalid access token!\n";
      std::exit(1);
    }
    Tasks t(cfg, &cli);
    t.sync();
    std::cout << "Sync finished.\n";
  });

  auto* list = app.add_subcommand("list", "list open issues");
  std::string list_project, list_url, list_token;
  bool list_closed = false, list_my = false;
  list->add_option("project", list_project);
  list->add_flag("-c,--closed", list_closed);
  list->add_flag("--my", list_my);
  list->add_option("--url", list_url);
  list->add_option("--token", list_token);
  list->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    if (!list_url.empty()) {
      cfg.set_json("url", list_url);
    }
    if (!list_token.empty()) {
      cfg.set_json("token", list_token);
    }
    GitlabClient cli(cfg);
    Tasks t(cfg, &cli);
    std::string state = list_closed ? "closed" : "opened";
    nlohmann::json issues = t.list_issues(list_project, state, list_my);
    if (!issues.is_array() || issues.empty()) {
      std::cout << "No issues found.\n";
      return;
    }
    print_issues_table_like_node(issues);
  });

  auto* rep = app.add_subcommand("report", "generate a report");
  std::string rep_project, rep_url, rep_token, rep_from, rep_to, rep_output, rep_file, rep_type, rep_user;
  std::vector<std::string> rep_query, rep_report, rep_date_fmt, rep_record_cols, rep_issue_cols, rep_mr_cols;
  bool rep_today = false, rep_week = false, rep_month = false, rep_last_month = false, rep_closed = false,
       rep_subgroups = false, rep_quiet = false, rep_verbose = false, rep_no_head = false, rep_no_warn = false,
       rep_show_empty = false, rep_user_cols = false;
  rep->add_option("project", rep_project);
  rep->add_option("--url", rep_url);
  rep->add_option("--token", rep_token);
  rep->add_option("-f,--from", rep_from);
  rep->add_option("-t,--to", rep_to);
  rep->add_flag("--today", rep_today);
  rep->add_flag("--this_week", rep_week);
  rep->add_flag("--this_month", rep_month);
  rep->add_flag("--last_month", rep_last_month);
  rep->add_flag("-c,--closed", rep_closed);
  rep->add_option("-u,--user", rep_user);
  rep->add_option("-e,--type", rep_type);
  rep->add_flag("--subgroups", rep_subgroups);
  rep->add_option("-q,--query", rep_query)->expected(-1);
  rep->add_option("-r,--report", rep_report)->expected(-1);
  rep->add_option("-o,--output", rep_output);
  rep->add_option("-l,--file", rep_file);
  rep->add_option("--date_format", rep_date_fmt)->expected(-1);
  rep->add_flag("--no_headlines", rep_no_head);
  rep->add_flag("--no_warnings", rep_no_warn);
  rep->add_flag("--quiet", rep_quiet);
  rep->add_flag("--verbose", rep_verbose);
  rep->add_flag("--show_without_times", rep_show_empty);
  rep->add_flag("--user_columns", rep_user_cols);
  rep->add_option("--record_columns", rep_record_cols)->expected(-1);
  rep->add_option("--issue_columns", rep_issue_cols)->expected(-1);
  rep->add_option("--merge_request_columns", rep_mr_cols)->expected(-1);
  rep->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    if (!rep_url.empty()) {
      cfg.set_json("url", rep_url);
    }
    if (!rep_token.empty()) {
      cfg.set_json("token", rep_token);
    }
    if (!rep_from.empty()) {
      cfg.set_json("from", rep_from);
    }
    if (!rep_to.empty()) {
      cfg.set_json("to", rep_to);
    }
    if (!rep_type.empty()) {
      cfg.set_json("type", rep_type);
    }
    if (!rep_user.empty()) {
      cfg.set_json("user", rep_user);
    }
    cfg.set_json("closed", rep_closed, true);
    cfg.set_json("subgroups", rep_subgroups, true);
    cfg.set_json("quiet", rep_quiet, true);
    cfg.set_json("_verbose", rep_verbose, true);
    cfg.set_json("noHeadlines", rep_no_head, true);
    cfg.set_json("noWarnings", rep_no_warn, true);
    cfg.set_json("showWithoutTimes", rep_show_empty, true);
    cfg.set_json("userColumns", rep_user_cols, true);
    if (!rep_output.empty()) {
      cfg.set_json("output", rep_output);
    }
    if (!rep_file.empty()) {
      cfg.set_json("file", rep_file);
    }
    if (!rep_query.empty()) {
      cfg.set_json("query", nlohmann::json(rep_query));
    }
    if (!rep_report.empty()) {
      cfg.set_json("report", nlohmann::json(rep_report));
    }
    if (!rep_date_fmt.empty()) {
      cfg.set_json("dateFormat", rep_date_fmt[0]);
    }
    if (!rep_record_cols.empty()) {
      cfg.set_json("recordColumns", nlohmann::json(rep_record_cols));
    }
    if (!rep_issue_cols.empty()) {
      cfg.set_json("issueColumns", nlohmann::json(rep_issue_cols));
    }
    if (!rep_mr_cols.empty()) {
      cfg.set_json("mergeRequestColumns", nlohmann::json(rep_mr_cols));
    }
    if (!rep_project.empty()) {
      cfg.set_json("project", rep_project);
    }
    apply_report_dates(cfg, rep_today, rep_week, rep_month, rep_last_month);
    GitlabClient cli(cfg);
    run_report(cfg, cli);
  });

  auto* cfgcmd = app.add_subcommand("config", "print configuration file paths");
  bool cfg_local = false;
  cfgcmd->add_flag("-l,--local", cfg_local);
  cfgcmd->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    if (cfg_local) {
      std::cout << cfg.local_config_file().string() << "\n";
    } else {
      std::cout << cfg.global_config_file().string() << "\n";
    }
  });

  auto* log = app.add_subcommand("log", "log recorded time records");
  bool log_csv = false;
  bool log_verbose = false;
  std::optional<int> log_hpd;
  std::optional<std::string> log_time_format;
  log->add_flag("--csv", log_csv);
  log->add_flag("--verbose", log_verbose);
  log->add_option("--hours_per_day", log_hpd, "hours per day for human readable time formats");
  log->add_option("--time_format", log_time_format, "time format (see Node Time.toHumanReadable patterns)");
  log->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    run_cmd_log(cfg, log_csv, log_hpd, log_time_format, log_verbose);
  });

  auto* resume = app.add_subcommand("resume", "resume last stopped frame (new tracking, same resource)");
  std::optional<std::string> resume_project;
  resume->add_option("project", resume_project)
      ->description("GitLab project path (only consider frames for this project; matches Node gtt resume)");
  resume->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    namespace fs = std::filesystem;
    if (!fs::exists(cfg.frame_dir())) {
      std::cerr << "No frames.\n";
      return;
    }
    std::vector<Frame> candidates;
    for (const auto& ent : fs::directory_iterator(cfg.frame_dir())) {
      if (!ent.is_regular_file() || ent.path().extension() != ".json") {
        continue;
      }
      try {
        Frame fr = Frame::from_file(cfg, ent.path());
        if (!fr.stopped()) {
          continue;
        }
        if (resume_project.has_value() && !resume_project->empty()) {
          if (fr.project != *resume_project) {
            continue;
          }
        }
        candidates.push_back(std::move(fr));
      } catch (...) {
      }
    }
    if (candidates.empty()) {
      if (resume_project.has_value() && !resume_project->empty()) {
        std::cerr << "No stopped frame found for project \"" << *resume_project << "\".\n";
      } else {
        std::cerr << "No stopped frame found.\n";
      }
      return;
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Frame& a, const Frame& b) { return a.stop_sys() > b.stop_sys(); });
    Frame& fr = candidates.front();
    try {
      Tasks t(cfg, nullptr);
      t.start(fr.project, fr.resource.at("type").get<std::string>(), fr.resource.at("id"), fr.note);
      const std::string typ = fr.resource.at("type").get<std::string>();
      const std::string rid = resource_id_plain(fr.resource.at("id"));
      const bool use_color = tty_stdout();
      const std::string clock = now_hh_mm_display(cfg);
      std::cout << "Starting project " << ansi_colored("\033[35m", fr.project, use_color) << " "
                << ansi_colored("\033[34m", typ, use_color) << " "
                << ansi_colored("\033[34m", "#" + rid, use_color);
      if (fr.note.has_value() && !fr.note->empty()) {
        std::cout << " " << *fr.note;
      }
      std::cout << " at " << ansi_colored("\033[32m", clock, use_color) << "\n";
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      std::exit(1);
    }
  });

  auto* edit = app.add_subcommand("edit", "edit a frame JSON ($VISUAL / $EDITOR), optional id or pick from list");
  std::optional<std::string> edit_id_arg;
  edit->add_option("id", edit_id_arg)->description("frame id (optional .json); omit to choose interactively");
  edit->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    namespace fs = std::filesystem;
    const fs::path frame_dir = cfg.frame_dir();
    if (!fs::exists(frame_dir)) {
      std::cerr << "No records found.\n";
      std::exit(1);
    }

    fs::path target;
    if (edit_id_arg.has_value() && !edit_id_arg->empty()) {
      std::string id = trim_ws(*edit_id_arg);
      if (id.size() > 5 && id.compare(id.size() - 5, 5, ".json") == 0) {
        id.resize(id.size() - 5);
      }
      target = frame_dir / (id + ".json");
      if (!fs::exists(target)) {
        std::cerr << "No record found.\n";
        std::exit(1);
      }
      launch_editor_path(target);
      return;
    }

    constexpr std::size_t k_list_size = 30;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    for (const auto& ent : fs::directory_iterator(frame_dir)) {
      if (!ent.is_regular_file() || ent.path().extension() != ".json") {
        continue;
      }
      files.emplace_back(fs::last_write_time(ent), ent.path());
    }
    std::sort(files.begin(), files.end());
    const std::size_t start = files.size() > k_list_size ? files.size() - k_list_size : 0;
    std::vector<Frame> frames;
    frames.reserve(files.size() - start);
    for (std::size_t i = start; i < files.size(); ++i) {
      try {
        frames.push_back(Frame::from_file(cfg, files[i].second));
      } catch (...) {
      }
    }
    std::sort(frames.begin(), frames.end(),
              [](const Frame& a, const Frame& b) { return a.start_sys() < b.start_sys(); });
    if (frames.empty()) {
      std::cerr << "No records found.\n";
      std::exit(1);
    }

    const std::string tz = cfg.timezone_name();
    const bool use_color = tty_stdout();
    std::vector<std::string> menu_lines;
    menu_lines.reserve(frames.size());
    std::vector<std::string> menu_selected;
    menu_selected.reserve(frames.size());
    for (const Frame& fr : frames) {
      menu_lines.push_back(format_edit_menu_line(fr, tz, use_color, std::nullopt, false));
      menu_selected.push_back(format_edit_menu_line(fr, tz, use_color, std::nullopt, true));
    }

    const gtt::ArrowSelectResult pick = gtt::tty_arrow_select(
        menu_lines, menu_selected, frames.empty() ? 0 : frames.size() - 1, "Frame?");
    if (pick.kind == gtt::ArrowSelectKind::Cancelled) {
      std::cout << "Aborted.\n";
      return;
    }
    if (pick.kind == gtt::ArrowSelectKind::Selected) {
      target = frames[pick.index].file_path(cfg);
      launch_editor_path(target);
      return;
    }

    for (std::size_t i = 0; i < frames.size(); ++i) {
      std::cout << format_edit_menu_line(frames[i], tz, use_color, static_cast<unsigned>(i + 1), false)
                << "\n";
    }
    std::cout << "Enter number (1-" << frames.size() << ") or frame id: " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) {
      std::exit(1);
    }
    line = trim_ws(line);
    if (line.empty()) {
      std::cout << "Aborted.\n";
      return;
    }
    std::size_t n = 0;
    if (parse_uint_strict(line, n) && n >= 1 && n <= frames.size()) {
      target = frames[n - 1].file_path(cfg);
    } else {
      std::string id = line;
      if (id.size() > 5 && id.compare(id.size() - 5, 5, ".json") == 0) {
        id.resize(id.size() - 5);
      }
      target = frame_dir / (id + ".json");
      if (!fs::exists(target)) {
        std::cerr << "record not found.\n";
        std::exit(1);
      }
    }
    launch_editor_path(target);
  });

  auto* create =
      app.add_subcommand("create", "start tracking and create a new issue or merge_request with the given title");
  std::vector<std::string> create_args;
  std::string create_type = "issue";
  create->add_option("args", create_args,
                     "[project] [title], or [title] if project is set in config");
  create->add_option("-t,--type", create_type, "issue or merge_request")->capture_default_str();
  create->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    if (create_type != "issue" && create_type != "merge_request") {
      std::cerr << "Error: --type must be issue or merge_request\n";
      std::exit(2);
    }
    if (create_args.size() > 2) {
      std::cerr << "Error: too many arguments (use: project title | title)\n";
      std::exit(2);
    }
    std::string project;
    std::string title;
    if (create_args.size() == 2) {
      project = create_args[0];
      title = create_args[1];
    } else if (create_args.size() == 1) {
      title = std::move(create_args[0]);
      project = cfg.get_string("project", "");
      if (project.empty()) {
        std::cerr << "Error: No project set\n";
        std::exit(2);
      }
    } else {
      std::cerr << "Error: Wrong or missing title\n";
      std::exit(2);
    }
    if (title.empty()) {
      std::cerr << "Error: Wrong or missing title\n";
      std::exit(2);
    }
    try {
      Tasks t(cfg, nullptr);
      const nlohmann::json id_json = title;
      t.start(project, create_type, id_json, std::nullopt);
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      std::exit(1);
    }
    const bool use_color = tty_stdout();
    std::cout << "Starting project " << ansi_colored("\033[35m", project, use_color) << " and create " << create_type
              << " " << ansi_colored("\033[34m", "\"" + title + "\"", use_color) << " at "
              << ansi_colored("\033[32m", now_hh_mm_display(cfg), use_color) << "\n";
  });

  auto* del = app.add_subcommand("delete", "delete a frame file by id");
  std::string del_id;
  del->add_option("id", del_id)->required();
  del->callback([&] {
    GttConfig& cfg = global_cfg();
    cfg.reload_from_disk();
    auto p = cfg.frame_dir() / (del_id + ".json");
    if (!std::filesystem::exists(p)) {
      std::cerr << "Frame not found.\n";
      std::exit(1);
    }
    std::filesystem::remove(p);
    std::cout << "Deleted " << del_id << "\n";
  });

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }
  // Match Node `gtt` with no subcommand: print usage/help and exit 1.
  if (app.get_subcommands().empty()) {
    std::cout << app.help();
    return 1;
  }
  return 0;
}
