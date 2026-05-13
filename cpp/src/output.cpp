#include "output.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_map>

#include "config.hpp"

namespace {

std::string esc_csv_cell(const std::string& s) {
  if (s.find_first_of(",\"\n\r") != std::string::npos) {
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
  return s;
}

std::string row_csv(const std::vector<std::string>& cells) {
  std::ostringstream o;
  for (size_t i = 0; i < cells.size(); ++i) {
    if (i) {
      o << ',';
    }
    o << esc_csv_cell(cells[i]);
  }
  return o.str() + "\n";
}

nlohmann::json prepare_row(const GttConfig& cfg, const nlohmann::json& obj, const std::vector<std::string>& cols) {
  nlohmann::json row = nlohmann::json::array();
  for (const auto& c : cols) {
    if (!obj.contains(c)) {
      row.push_back("");
      continue;
    }
    const auto& v = obj.at(c);
    if (v.is_string()) {
      row.push_back(v.get<std::string>());
    } else if (v.is_number_integer()) {
      row.push_back(std::to_string(v.get<int>()));
    } else if (v.is_null()) {
      row.push_back("");
    } else {
      row.push_back(v.dump());
    }
  }
  (void)cfg;
  return row;
}

void collect_stats(const GttConfig& cfg, const nlohmann::json& bundle, std::int64_t& total_est,
                   std::int64_t& total_api_spent, std::int64_t& spent_notes, std::unordered_map<std::string, std::int64_t>& users,
                   std::unordered_map<std::string, std::int64_t>& projects) {
  auto proc = [&](const nlohmann::json& arr) {
    for (const auto& it : arr) {
      if (it.contains("_stats")) {
        total_est += it["_stats"].value("time_estimate", 0);
        total_api_spent += it["_stats"].value("total_time_spent", 0);
      }
      if (it.contains("_times") && it["_times"].is_array()) {
        for (const auto& t : it["_times"]) {
          std::int64_t s = t.value("_seconds", 0LL);
          spent_notes += s;
          users[t["_user"].get<std::string>()] += s;
          projects[t["_project_ns"].get<std::string>()] += s;
        }
      }
    }
  };
  proc(bundle["issues"]);
  proc(bundle["merge_requests"]);
}

std::string markdown_table_from_rows(const nlohmann::json& rows) {
  if (!rows.is_array() || rows.empty()) {
    return "";
  }
  std::ostringstream o;
  const auto& head = rows[0];
  o << "| ";
  for (size_t i = 0; i < head.size(); ++i) {
    if (i) {
      o << " | ";
    }
    o << head[i].get<std::string>();
  }
  o << " |\n| ";
  for (size_t i = 0; i < head.size(); ++i) {
    if (i) {
      o << " | ";
    }
    o << "---";
  }
  o << " |\n";
  for (size_t r = 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    o << "| ";
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) {
        o << " | ";
      }
      o << row[i].get<std::string>();
    }
    o << " |\n";
  }
  return o.str();
}

std::string ascii_table(const nlohmann::json& rows) {
  if (!rows.is_array() || rows.empty()) {
    return "";
  }
  std::vector<size_t> widths;
  const auto& head = rows[0];
  widths.resize(head.size(), 0);
  for (size_t c = 0; c < head.size(); ++c) {
    widths[c] = std::max(widths[c], head[c].get<std::string>().size());
  }
  for (size_t r = 1; r < rows.size(); ++r) {
    const auto& row = rows[r];
    for (size_t c = 0; c < row.size(); ++c) {
      widths[c] = std::max(widths[c], row[c].get<std::string>().size());
    }
  }
  auto line = [&](const nlohmann::json& row) {
    std::ostringstream o;
    o << "| ";
    for (size_t c = 0; c < row.size(); ++c) {
      if (c) {
        o << " | ";
      }
      std::string cell = row[c].get<std::string>();
      o << cell << std::string(widths[c] - cell.size(), ' ');
    }
    o << " |\n";
    return o.str();
  };
  std::ostringstream out;
  out << line(head);
  out << "|";
  for (size_t c = 0; c < widths.size(); ++c) {
    out << std::string(widths[c] + 2, '-');
    if (c + 1 < widths.size()) {
      out << "+";
    }
  }
  out << "|\n";
  for (size_t r = 1; r < rows.size(); ++r) {
    out << line(rows[r]);
  }
  return out.str();
}

void emit_table_section(std::ostringstream& out, const GttConfig& cfg, const std::string& kind,
                        const std::string& title, const nlohmann::json& items, const std::vector<std::string>& cols) {
  if (!cfg.get_bool("noHeadlines", false)) {
    out << (kind == "markdown" ? "\n### " : "\n") << title << (kind == "markdown" ? "\n\n" : "\n\n");
  }
  nlohmann::json rows = nlohmann::json::array();
  nlohmann::json head = nlohmann::json::array();
  for (const auto& c : cols) {
    std::string h = c;
    for (auto& ch : h) {
      if (ch == '_') {
        ch = ' ';
      }
    }
    head.push_back(h);
  }
  rows.push_back(head);
  for (const auto& it : items) {
    rows.push_back(prepare_row(cfg, it, cols));
  }
  if (kind == "markdown") {
    out << markdown_table_from_rows(rows);
  } else if (kind == "csv") {
    for (size_t i = 0; i < rows.size(); ++i) {
      std::vector<std::string> cells;
      for (const auto& c : rows[i]) {
        cells.push_back(c.get<std::string>());
      }
      out << row_csv(cells);
    }
  } else {
    out << ascii_table(rows) << "\n";
  }
}

}  // namespace

std::string build_report_text(const GttConfig& cfg, const nlohmann::json& bundle, const std::string& kind) {
  using json = nlohmann::json;
  std::ostringstream out;
  auto report_parts = cfg.get_string_vec("report");

  std::int64_t total_est = 0, total_api = 0, spent_notes = 0;
  std::unordered_map<std::string, std::int64_t> users, projects;
  collect_stats(cfg, bundle, total_est, total_api, spent_notes, users, projects);

  auto want = [&](const char* p) {
    return std::find(report_parts.begin(), report_parts.end(), std::string(p)) != report_parts.end();
  };

  if (want("stats")) {
    if (!cfg.get_bool("noHeadlines", false)) {
      out << (kind == "markdown" ? "\n### TIME STATS\n\n" : "\nTIME STATS\n\n");
    }
    out << "* total estimate: " << cfg.to_human_readable(total_est, "stats") << "\n";
    out << "* total spent (API): " << cfg.to_human_readable(total_api, "stats") << "\n";
    out << "* spent (parsed notes): " << cfg.to_human_readable(spent_notes, "stats") << "\n";
    for (const auto& [u, s] : users) {
      out << "* " << u << ": " << cfg.to_human_readable(s, "stats") << "\n";
    }
    for (const auto& [p, s] : projects) {
      out << "* " << p << ": " << cfg.to_human_readable(s, "stats") << "\n";
    }
    out << "\n";
  }

  std::vector<std::string> icols = cfg.get_string_vec("issueColumns");
  if (icols.empty()) {
    icols = {"iid", "title", "spent", "total_estimate"};
  }
  std::vector<std::string> mcols = cfg.get_string_vec("mergeRequestColumns");
  if (mcols.empty()) {
    mcols = {"iid", "title", "spent", "total_estimate"};
  }

  if (want("issues")) {
    emit_table_section(out, cfg, kind, "ISSUES", bundle["issues"], icols);
  }
  if (want("merge_requests")) {
    emit_table_section(out, cfg, kind, "MERGE REQUESTS", bundle["merge_requests"], mcols);
  }

  if (want("records")) {
    if (!cfg.get_bool("noHeadlines", false)) {
      out << (kind == "markdown" ? "\n### TIME RECORDS\n\n" : "\nTIME RECORDS\n\n");
    }
    json rows = json::array();
    std::vector<std::string> rcols = cfg.get_string_vec("recordColumns");
    if (rcols.empty()) {
      rcols = {"user", "date", "type", "iid", "time"};
    }
    json head = json::array();
    for (const auto& c : rcols) {
      std::string h = c;
      for (auto& ch : h) {
        if (ch == '_') {
          ch = ' ';
        }
      }
      head.push_back(h);
    }
    rows.push_back(head);
    auto add_times = [&](const json& arr) {
      for (const auto& it : arr) {
        if (!it.contains("_times")) {
          continue;
        }
        for (const auto& t : it["_times"]) {
          json disp = json::object();
          disp["user"] = t["_user"];
          disp["date"] = t["_date"];
          disp["type"] = t["_type"];
          disp["iid"] = t["_iid"];
          disp["time"] = t["_time"];
          rows.push_back(prepare_row(cfg, disp, rcols));
        }
      }
    };
    add_times(bundle["issues"]);
    add_times(bundle["merge_requests"]);
    if (kind == "markdown") {
      out << markdown_table_from_rows(rows);
    } else if (kind == "csv") {
      for (size_t i = 0; i < rows.size(); ++i) {
        std::vector<std::string> cells;
        for (const auto& c : rows[i]) {
          cells.push_back(c.get<std::string>());
        }
        out << row_csv(cells);
      }
    } else {
      out << ascii_table(rows) << "\n";
    }
  }

  return out.str();
}

void write_report_to_file(const GttConfig& cfg, const nlohmann::json& bundle, const std::string& kind,
                          const std::string& path) {
  std::ofstream f(path);
  f << build_report_text(cfg, bundle, kind);
}
