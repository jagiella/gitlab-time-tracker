#include "report.hpp"

#include <algorithm>
#include <curl/curl.h>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>

#include "config.hpp"
#include "gitlab_client.hpp"
#include "output.hpp"
#include "time_format.hpp"

namespace {

std::chrono::sys_seconds parse_iso_to_sys(const std::string& s) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) >= 6) {
  } else if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &mo, &d) >= 3) {
    h = mi = se = 0;
  } else {
    return {};
  }
  using namespace std::chrono;
  return sys_days{year{y} / month{unsigned(mo)} / day{unsigned(d)}} + hours{h} + minutes{mi} + seconds{se};
}

std::string enc_proj(const std::string& p) {
  CURL* c = curl_easy_init();
  if (!c) {
    return p;
  }
  char* e = curl_easy_escape(c, p.c_str(), 0);
  std::string o = e ? e : p;
  if (e) {
    curl_free(e);
  }
  curl_easy_cleanup(c);
  return o;
}

bool labels_exclude(const nlohmann::json& labels, const std::vector<std::string>& excludes) {
  if (excludes.empty()) {
    return false;
  }
  for (const auto& lab : labels) {
    if (!lab.is_string()) {
      continue;
    }
    const std::string L = lab.get<std::string>();
    for (const auto& ex : excludes) {
      if (L == ex) {
        return true;
      }
    }
  }
  return false;
}

std::vector<std::string> labels_filtered(const nlohmann::json& labels, const GttConfig& cfg) {
  std::vector<std::string> out;
  std::vector<std::string> exclude = cfg.get_string_vec("excludeLabels");
  std::vector<std::string> include_only = cfg.get_string_vec("includeLabels");
  for (const auto& lab : labels) {
    if (!lab.is_string()) {
      continue;
    }
    std::string L = lab.get<std::string>();
    bool skip = false;
    for (const auto& ex : exclude) {
      if (L == ex) {
        skip = true;
        break;
      }
    }
    if (!skip) {
      out.push_back(L);
    }
  }
  if (!include_only.empty()) {
    std::vector<std::string> filtered;
    for (const auto& L : out) {
      for (const auto& inc : include_only) {
        if (L == inc) {
          filtered.push_back(L);
        }
      }
    }
    return filtered;
  }
  return out;
}

std::string params_query(const GttConfig& cfg) {
  std::ostringstream oss;
  std::vector<std::string> parts;
  if (cfg.raw().contains("iids") && cfg.raw()["iids"].is_array() && !cfg.raw()["iids"].empty() &&
      cfg.get_string_vec("query").size() == 1) {
    std::ostringstream iids;
    bool first = true;
    for (const auto& id : cfg.raw()["iids"]) {
      if (!first) {
        iids << ',';
      }
      first = false;
      if (id.is_number()) {
        iids << id.get<int>();
      } else {
        iids << id.get<std::string>();
      }
    }
    parts.push_back("iids=" + iids.str());
  }
  if (!cfg.get_bool("closed", false)) {
    parts.push_back("state=opened");
  }
  auto inc = cfg.get_string_vec("includeByLabels");
  if (!inc.empty()) {
    std::ostringstream lb;
    for (size_t i = 0; i < inc.size(); ++i) {
      if (i) {
        lb << ',';
      }
      lb << inc[i];
    }
    parts.push_back("labels=" + lb.str());
  }
  if (cfg.raw().contains("milestone") && cfg.raw()["milestone"].is_string() &&
      !cfg.raw()["milestone"].get<std::string>().empty()) {
    parts.push_back("milestone=" + enc_proj(cfg.raw()["milestone"].get<std::string>()));
  }
  if (parts.empty()) {
    return "?";
  }
  oss << "?";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      oss << "&";
    }
    oss << parts[i];
  }
  return oss.str();
}

void fetch_timelogs_for_project(GitlabClient& client, const GttConfig& cfg, const nlohmann::json& project,
                                nlohmann::json& out_logs) {
  std::string path_ns = project.at("path_with_namespace").get<std::string>();
  std::string from_day = std::format("{:%Y-%m-%d}", cfg.get_from());
  std::string to_day = std::format("{:%Y-%m-%d}", cfg.get_to());
  const char* query = R"(
        query ($project: ID!, $after: String, $entryPerPage: Int,
            $startTime:Time, $endTime:Time){
              project(fullPath: $project) {
                name
                timelogs(startTime: $startTime, endTime: $endTime,
                  first:$entryPerPage, after: $after) {
                  pageInfo {
                    hasNextPage
                    endCursor
                  }
                  nodes {
                    user {
                      username
                    }
                    spentAt
                    timeSpent
                    summary
                    note {
                      body
                      url
                    }
                    mergeRequests:mergeRequest {
                      iid
                      projectId
                    }
                    issues:issue {
                      iid
                      projectId
                    }
                  }
                }
              }
            }
            )";
  std::string cursor;
  nlohmann::json all_nodes = nlohmann::json::array();
  while (true) {
    nlohmann::json body = {{"query", query},
                           {"variables",
                            {{"project", path_ns},
                             {"after", cursor},
                             {"entryPerPage", 30},
                             {"startTime", from_day},
                             {"endTime", to_day}}}};
    auto res = client.graph_ql(body);
    if (res.status < 200 || res.status >= 300 || !res.body.is_object()) {
      break;
    }
    if (res.body.contains("errors")) {
      break;
    }
    auto& pr = res.body["data"]["project"];
    if (!pr.contains("timelogs")) {
      break;
    }
    auto& data = pr["timelogs"];
    if (!data.contains("nodes") || !data["nodes"].is_array()) {
      break;
    }
    for (const auto& n : data["nodes"]) {
      all_nodes.push_back(n);
    }
    if (!data["pageInfo"]["hasNextPage"].get<bool>()) {
      break;
    }
    cursor = data["pageInfo"]["endCursor"].get<std::string>();
  }
  out_logs = std::move(all_nodes);
}

void get_notes_for(GitlabClient& client, const GttConfig& cfg, const nlohmann::json& issue, bool is_mr,
                   nlohmann::json& out_notes) {
  int pid = issue.at("project_id").get<int>();
  int iid = issue.at("iid").get<int>();
  std::string typ = is_mr ? "merge_requests" : "issues";
  std::string path = "projects/" + std::to_string(pid) + "/" + typ + "/" + std::to_string(iid) + "/notes";
  out_notes = client.all_pages(path, cfg.get_int("_perPage", 100), cfg.get_int("_parallel", 10));
}

nlohmann::json get_time_stats(GitlabClient& client, const nlohmann::json& issue, bool is_mr) {
  int pid = issue.at("project_id").get<int>();
  int iid = issue.at("iid").get<int>();
  std::string typ = is_mr ? "merge_requests" : "issues";
  std::string path = "projects/" + std::to_string(pid) + "/" + typ + "/" + std::to_string(iid) + "/time_stats";
  auto r = client.get(path, 1, 1);
  return r.body;
}

void attach_timelogs(nlohmann::json& issue, const nlohmann::json& all_logs, bool is_mr) {
  nlohmann::json mine = nlohmann::json::array();
  int iid = issue.at("iid").get<int>();
  int pid = issue.at("project_id").get<int>();
  for (const auto& lg : all_logs) {
    const char* key = is_mr ? "mergeRequests" : "issues";
    if (!lg.contains(key) || lg[key].is_null()) {
      continue;
    }
    if (lg[key]["iid"].get<int>() != iid) {
      continue;
    }
    if (lg[key]["projectId"].get<int>() != pid) {
      continue;
    }
    mine.push_back(lg);
  }
  issue["_timelog_subset"] = mine;
}

void parse_times_from_notes(nlohmann::json& issue, const GttConfig& cfg, const std::string& project_ns,
                            bool is_mr, const std::string& type_label) {
  static const std::regex re_added(R"(added (.*) of time spent(?: at ([0-9-]*))?)", std::regex::icase);
  static const std::regex re_sub(R"(subtracted (.*) of time spent(?: at ([0-9-]*))?)", std::regex::icase);
  static const std::regex re_del(R"(deleted (.*) of spent time(?: from ([0-9-]*))?)", std::regex::icase);
  static const std::regex re_rem("Removed time spent", std::regex::icase);

  if (!issue.contains("_notes")) {
    return;
  }
  auto notes = issue["_notes"];
  if (!notes.is_array()) {
    return;
  }
  std::sort(notes.begin(), notes.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
    return a.at("created_at").get<std::string>() < b.at("created_at").get<std::string>();
  });

  std::int64_t time_spent = 0;
  nlohmann::json times = nlohmann::json::array();
  int hpd = cfg.get_int("hoursPerDay", 8);
  const std::string user_f = cfg.get_string("user", "");

  for (const auto& note : notes) {
    if (!note.value("system", false)) {
      continue;
    }
    const std::string body = note.value("body", std::string());
    std::smatch m;
    std::string time_str;
    int mult = 1;
    std::string created_at = note.at("created_at").get<std::string>();
    if (std::regex_search(body, m, re_added)) {
      time_str = m[1].str();
      if (m[2].matched && !m[2].str().empty()) {
        created_at = m[2].str();
      }
    } else if (std::regex_search(body, m, re_sub)) {
      time_str = m[1].str();
      mult = -1;
      if (m[2].matched && !m[2].str().empty()) {
        created_at = m[2].str();
      }
    } else if (std::regex_search(body, m, re_del)) {
      time_str = m[1].str();
      mult = -1;
      if (m[2].matched && !m[2].str().empty()) {
        created_at = m[2].str();
      }
    } else if (std::regex_search(body, re_rem)) {
      time_str = gtt::timefmt::to_human_readable(time_spent, hpd);
      mult = -1;
    } else {
      continue;
    }
    std::string author = note["author"].at("username").get<std::string>();
    if (!user_f.empty() && author != user_f) {
      continue;
    }
    std::int64_t secs = gtt::timefmt::parse_human_duration(time_str, hpd, cfg.get_int("daysPerWeek", 5),
                                                            cfg.get_int("weeksPerMonth", 4)) *
                      mult;
    auto tp = parse_iso_to_sys(created_at);
    if (tp < cfg.get_from() || tp > cfg.get_to()) {
      continue;
    }
    time_spent += secs;
    nlohmann::json row = {{"_user", author},
                          {"_date", created_at},
                          {"_type", type_label},
                          {"_iid", issue.at("iid").get<int>()},
                          {"_time", cfg.to_human_readable(secs, "records")},
                          {"_seconds", secs},
                          {"_project_ns", project_ns},
                          {"_title", issue.value("title", std::string())}};
    times.push_back(std::move(row));
  }

  issue["_times"] = times;
  issue["_time_spent_seconds"] = time_spent;
  issue.erase("_notes");
}

void decorate_issue_display(nlohmann::json& issue, const GttConfig& cfg, bool is_mr) {
  std::int64_t ts = issue.value("_time_spent_seconds", 0LL);
  const char* tf = is_mr ? "merge_requests" : "issues";
  issue["spent"] = cfg.to_human_readable(ts, tf);
  int est = issue.value("_stats", nlohmann::json::object()).value("time_estimate", 0);
  issue["total_estimate"] = cfg.to_human_readable(est, tf);
}

void process_issue_like(GitlabClient& client, const GttConfig& cfg, nlohmann::json& issue,
                        const nlohmann::json& project_timelogs, const std::string& project_ns, bool is_mr) {
  get_notes_for(client, cfg, issue, is_mr, issue["_notes"]);
  attach_timelogs(issue, project_timelogs, is_mr);
  parse_times_from_notes(issue, cfg, project_ns, is_mr, is_mr ? "MergeRequest" : "Issue");
  issue["_stats"] = get_time_stats(client, issue, is_mr);
  decorate_issue_display(issue, cfg, is_mr);
}

}  // namespace

void run_report(GttConfig& cfg, GitlabClient& client) {
  std::string out_kind = cfg.get_string("output", "table");
  if (out_kind != "table" && out_kind != "csv" && out_kind != "markdown") {
    std::cerr << "Unknown output \"" << out_kind << "\". Use: table, csv, markdown\n";
    std::exit(2);
  }

  std::vector<std::string> projects;
  if (cfg.raw().contains("project")) {
    if (cfg.raw()["project"].is_string()) {
      projects.push_back(cfg.raw()["project"].get<std::string>());
    } else if (cfg.raw()["project"].is_array()) {
      for (const auto& p : cfg.raw()["project"]) {
        projects.push_back(p.get<std::string>());
      }
    }
  }
  if (projects.empty()) {
    std::cerr << "Missing project(s) or group(s) namespace.\n";
    std::exit(2);
  }

  auto r_check = client.get("broadcast_messages", 1, 1);
  if (r_check.status == 401) {
    std::cerr << "Invalid access token!\n";
    std::exit(1);
  }

  std::vector<nlohmann::json> project_jsons;
  std::string type = cfg.get_string("type", "project");

  if (type == "project") {
    for (const auto& proj : projects) {
      auto pr = client.get("projects/" + enc_proj(proj), 1, 1);
      if (pr.status < 200 || pr.status >= 300) {
        std::cerr << "Project not found or no access: " << proj << "\n";
        std::exit(1);
      }
      project_jsons.push_back(pr.body);
    }
  } else if (type == "group") {
    auto gr = client.get("groups", 1, 100);
    nlohmann::json groups = gr.body.is_array() ? gr.body : nlohmann::json::array();
    nlohmann::json matched = nlohmann::json::array();
    for (const auto& g : groups) {
      if (g.value("full_path", std::string()) == projects[0]) {
        matched.push_back(g);
      }
    }
    if (matched.empty()) {
      std::cerr << "Group not found.\n";
      std::exit(1);
    }
    nlohmann::json all_groups = matched;
    if (cfg.get_bool("subgroups", false)) {
      for (const auto& g : groups) {
        for (const auto& root : matched) {
          if (g.value("parent_id", 0) == root.value("id", -1)) {
            all_groups.push_back(g);
          }
        }
      }
    }
    for (const auto& g : all_groups) {
      int gid = g.at("id").get<int>();
      std::string path = "groups/" + std::to_string(gid) + "/projects";
      nlohmann::json plist = client.all_pages(path, cfg.get_int("_perPage", 100), cfg.get_int("_parallel", 10));
      if (!plist.is_array()) {
        continue;
      }
      for (const auto& p : plist) {
        project_jsons.push_back(p);
      }
    }
  }

  nlohmann::json master_issues = nlohmann::json::array();
  nlohmann::json master_mrs = nlohmann::json::array();
  nlohmann::json master_logs = nlohmann::json::array();

  std::string pq = params_query(cfg);
  auto query_list = cfg.get_string_vec("query");

  for (const auto& project : project_jsons) {
    std::string pid = std::to_string(project.at("id").get<int>());
    std::string path_ns = project.at("path_with_namespace").get<std::string>();

    nlohmann::json project_logs = nlohmann::json::array();
    fetch_timelogs_for_project(client, cfg, project, project_logs);
    for (const auto& lg : project_logs) {
      master_logs.push_back(lg);
    }

    if (std::find(query_list.begin(), query_list.end(), "issues") != query_list.end()) {
      nlohmann::json iss = client.all_pages("projects/" + pid + "/issues" + pq, cfg.get_int("_perPage", 100),
                                            cfg.get_int("_parallel", 10));
      if (iss.is_array()) {
        std::vector<std::string> ex = cfg.get_string_vec("excludeByLabels");
        for (auto& it : iss) {
          if (it.contains("moved_to_id") && !it["moved_to_id"].is_null() && it["moved_to_id"].is_number() &&
              it["moved_to_id"].get<int>() != 0) {
            continue;
          }
          if (labels_exclude(it.value("labels", nlohmann::json::array()), ex)) {
            continue;
          }
          it["labels"] = labels_filtered(it.value("labels", nlohmann::json::array()), cfg);
          it["_project_ns"] = path_ns;
          process_issue_like(client, cfg, it, project_logs, path_ns, false);
          master_issues.push_back(std::move(it));
        }
      }
    }
    if (std::find(query_list.begin(), query_list.end(), "merge_requests") != query_list.end()) {
      nlohmann::json mrs = client.all_pages("projects/" + pid + "/merge_requests" + pq, cfg.get_int("_perPage", 100),
                                             cfg.get_int("_parallel", 10));
      if (mrs.is_array()) {
        std::vector<std::string> ex = cfg.get_string_vec("excludeByLabels");
        for (auto& mr : mrs) {
          if (labels_exclude(mr.value("labels", nlohmann::json::array()), ex)) {
            continue;
          }
          mr["labels"] = labels_filtered(mr.value("labels", nlohmann::json::array()), cfg);
          mr["_project_ns"] = path_ns;
          process_issue_like(client, cfg, mr, project_logs, path_ns, true);
          master_mrs.push_back(std::move(mr));
        }
      }
    }
  }

  if (!cfg.get_bool("showWithoutTimes", false)) {
    nlohmann::json fi = nlohmann::json::array();
    for (auto& it : master_issues) {
      if (it.contains("_times") && it["_times"].is_array() && !it["_times"].empty()) {
        fi.push_back(std::move(it));
      }
    }
    master_issues = std::move(fi);
    nlohmann::json fm = nlohmann::json::array();
    for (auto& it : master_mrs) {
      if (it.contains("_times") && it["_times"].is_array() && !it["_times"].empty()) {
        fm.push_back(std::move(it));
      }
    }
    master_mrs = std::move(fm);
  }

  if (master_issues.empty() && master_mrs.empty()) {
    std::cerr << "No issues or merge requests matched your criteria.\n";
    std::exit(1);
  }

  auto sort_desc_iid = [](nlohmann::json& arr) {
    if (!arr.is_array() || arr.size() < 2) {
      return;
    }
    std::vector<nlohmann::json> v;
    for (auto& x : arr) {
      v.push_back(std::move(x));
    }
    std::sort(v.begin(), v.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
      return a.at("iid").get<int>() > b.at("iid").get<int>();
    });
    arr = nlohmann::json::array();
    for (auto& x : v) {
      arr.push_back(std::move(x));
    }
  };
  sort_desc_iid(master_issues);
  sort_desc_iid(master_mrs);

  nlohmann::json bundle;
  bundle["issues"] = master_issues;
  bundle["merge_requests"] = master_mrs;
  bundle["timelogs"] = master_logs;
  std::string file = cfg.get_string("file", "");
  if (!file.empty()) {
    write_report_to_file(cfg, bundle, out_kind, file);
  } else {
    std::cout << build_report_text(cfg, bundle, out_kind);
  }
}
