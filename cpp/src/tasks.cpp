#include "tasks.hpp"

#include <algorithm>
#include <curl/curl.h>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include "config.hpp"
#include "gitlab_client.hpp"
#include "time_format.hpp"

namespace {

std::string url_encode_project(const std::string& project) {
  CURL* c = curl_easy_init();
  if (!c) {
    return project;
  }
  char* esc = curl_easy_escape(c, project.c_str(), 0);
  std::string out = esc ? esc : project;
  if (esc) {
    curl_free(esc);
  }
  curl_easy_cleanup(c);
  return out;
}

std::string spend_body(int seconds, const std::string& spent_ymd, const std::optional<std::string>& note,
                       int hours_per_day) {
  std::string human =
      gtt::timefmt::to_human_readable(seconds, hours_per_day, "[%sign][%days>d ][%hours>h ][%minutes>m ][%seconds>s]");
  std::string n = note.value_or("");
  if (!n.empty()) {
    n = "\n\n" + n;
  }
  return std::string("/spend ") + human + " " + spent_ymd + n;
}

struct RemoteResource {
  GitlabClient* client = nullptr;
  GttConfig* cfg = nullptr;
  nlohmann::json data;
  nlohmann::json notes = nlohmann::json::array();

  std::string type_path() const {
    if (!data.is_object()) {
      return "issues";
    }
    const std::string t = data.value("type", std::string("issue"));
    return t == "merge_request" ? "merge_requests" : "issues";
  }

  int iid() const { return data.at("iid").get<int>(); }
  int project_id() const { return data.at("project_id").get<int>(); }
  std::string title() const {
    if (!data.is_object()) {
      return {};
    }
    return data.value("title", std::string());
  }

  void make(const std::string& project, const nlohmann::json& id, bool is_new) {
    std::string enc = url_encode_project(project);
    if (is_new) {
      nlohmann::json body = {{"title", id.get<std::string>()}};
      auto r = client->post("projects/" + enc + "/" + type_path(), body);
      if (r.status < 200 || r.status >= 300) {
        throw std::runtime_error("create failed");
      }
      data = r.body;
    } else {
      std::string path =
          "projects/" + enc + "/" + type_path() + "/" + std::to_string(id.get<int>());
      auto r = client->get(path, 1, 1);
      if (r.status < 200 || r.status >= 300) {
        throw std::runtime_error("get issue/mr failed");
      }
      data = r.body;
    }
  }

  void load_notes() {
    std::string path = "projects/" + std::to_string(project_id()) + "/" + type_path() + "/" +
                       std::to_string(iid()) + "/notes";
    notes = client->all_pages(path, cfg->get_int("_perPage", 100), cfg->get_int("_parallel", 10));
  }

  void create_time(int seconds, const std::string& stop_iso, const std::optional<std::string>& note) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    std::sscanf(stop_iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se);
    char spent[32];
    std::snprintf(spent, sizeof(spent), "%d-%d-%d", y, mo, d);
    std::string body_txt =
        spend_body(seconds, spent, note, cfg->get_int("hoursPerDay", 8));
    nlohmann::json body = {{"body", body_txt}};
    std::string path = "projects/" + std::to_string(project_id()) + "/" + type_path() + "/" +
                       std::to_string(iid()) + "/notes";
    auto r = client->post(path, body);
    if (r.status < 200 || r.status >= 300) {
      throw std::runtime_error("post time note failed");
    }
  }

  nlohmann::json newest_note() const {
    if (!notes.is_array() || notes.empty()) {
      return {};
    }
    auto note_id = [](const nlohmann::json& j) -> int {
      if (!j.is_object()) {
        return 0;
      }
      return j.value("id", 0);
    };
    auto it = std::max_element(notes.begin(), notes.end(),
                               [&](const nlohmann::json& a, const nlohmann::json& b) { return note_id(a) < note_id(b); });
    return *it;
  }
};

}  // namespace

Tasks::Tasks(GttConfig& cfg, GitlabClient* client) : cfg_(cfg), client_(client) {}

bool Tasks::file_looks_running(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in) {
    return false;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();
  const std::string s = buffer.str();
  static const std::regex re_false(R"("stop"\s*:\s*false)", std::regex::icase);
  static const std::regex re_null(R"("stop"\s*:\s*null)", std::regex::icase);
  return std::regex_search(s, re_false) || std::regex_search(s, re_null);
}

std::vector<std::filesystem::path> Tasks::running_frame_paths() const {
  std::vector<std::filesystem::path> out;
  namespace fs = std::filesystem;
  if (!fs::exists(cfg_.frame_dir())) {
    return out;
  }
  for (const auto& ent : fs::directory_iterator(cfg_.frame_dir())) {
    if (!ent.is_regular_file()) {
      continue;
    }
    if (ent.path().extension() != ".json") {
      continue;
    }
    if (file_looks_running(ent.path())) {
      out.push_back(ent.path());
    }
  }
  return out;
}

Frame Tasks::start(const std::string& project, const std::string& type, const nlohmann::json& id,
                   const std::optional<std::string>& note) {
  if (!running_frame_paths().empty()) {
    throw std::runtime_error("Already running. Please stop it first with 'gtt stop'.");
  }
  return Frame::create_started(cfg_, project, type, id, note);
}

std::vector<Frame> Tasks::stop() {
  auto paths = running_frame_paths();
  if (paths.empty()) {
    throw std::runtime_error("No projects started.");
  }
  std::vector<Frame> out;
  for (const auto& p : paths) {
    Frame f = Frame::from_file(cfg_, p);
    f.stop_now(cfg_);
    out.push_back(std::move(f));
  }
  return out;
}

std::vector<Frame> Tasks::cancel() {
  auto paths = running_frame_paths();
  if (paths.empty()) {
    throw std::runtime_error("No projects started.");
  }
  std::vector<Frame> out;
  for (const auto& p : paths) {
    Frame f = Frame::from_file(cfg_, p);
    std::filesystem::remove(p);
    out.push_back(std::move(f));
  }
  return out;
}

std::vector<Frame> Tasks::status() {
  std::vector<Frame> out;
  for (const auto& p : running_frame_paths()) {
    out.push_back(Frame::from_file(cfg_, p));
  }
  return out;
}

void Tasks::sync() {
  if (!client_) {
    throw std::runtime_error("sync requires API client");
  }
  std::vector<Frame> pending;
  namespace fs = std::filesystem;
  if (!fs::exists(cfg_.frame_dir())) {
    return;
  }
  for (const auto& ent : fs::directory_iterator(cfg_.frame_dir())) {
    if (!ent.is_regular_file() || ent.path().extension() != ".json") {
      continue;
    }
    Frame fr = Frame::from_file(cfg_, ent.path());
    if (!fr.stopped()) {
      continue;
    }
    int need = static_cast<int>(fr.ceil_duration_seconds(cfg_));
    int have = fr.notes_time_total();
    if (need == have) {
      continue;
    }
    pending.push_back(std::move(fr));
  }

  for (auto& fr : pending) {
    RemoteResource res;
    res.client = client_;
    res.cfg = &cfg_;
    bool is_new = fr.resource.at("id").is_string();
    res.make(fr.project, fr.resource.at("id"), is_new);
    fr.title = res.title();
    int have = fr.notes_time_total();
    int time = static_cast<int>(fr.ceil_duration_seconds(cfg_)) - have;
    if (time <= 0) {
      continue;
    }
    std::string stop_iso = fr.stop_iso.value_or(fr.start_iso);
    res.create_time(time, stop_iso, fr.note);
    res.load_notes();
    auto newest = res.newest_note();
    if (is_new) {
      fr.resource.erase("new");
      fr.resource["title"] = fr.resource.at("id");
      fr.resource["id"] = res.iid();
    }
    nlohmann::json note_entry = nlohmann::json::object();
    if (!newest.is_null() && newest.contains("id")) {
      note_entry["id"] = newest.at("id");
    } else {
      note_entry["id"] = 0;
    }
    note_entry["time"] = time;
    fr.notes.push_back(note_entry);
    fr.write(cfg_, true);
  }
}

nlohmann::json Tasks::list_issues(const std::string& project, const std::string& state, bool mine) {
  std::ostringstream q;
  q << "scope=" << (mine ? "assigned-to-me" : "all") << "&state=" << state;
  std::string path;
  if (!project.empty()) {
    path = "projects/" + url_encode_project(project) + "/issues?" + q.str();
  } else {
    path = "issues/?" + q.str();
  }
  auto r = client_->get(path, 1, 100);
  if (r.status < 200 || r.status >= 300) {
    return nlohmann::json::array();
  }
  return r.body.is_array() ? r.body : nlohmann::json::array();
}

Tasks::LogCollected Tasks::collect_log() const {
  LogCollected out;
  namespace fs = std::filesystem;
  if (!fs::exists(cfg_.frame_dir())) {
    return out;
  }
  for (const auto& ent : fs::directory_iterator(cfg_.frame_dir())) {
    if (!ent.is_regular_file() || ent.path().extension() != ".json") {
      continue;
    }
    try {
      Frame fr = Frame::from_file(cfg_, ent.path());
      if (!fr.stopped()) {
        continue;
      }
      const std::string dk = fr.log_date_key(cfg_);
      out.frames[dk].push_back(std::move(fr));
    } catch (...) {
    }
  }
  for (auto& kv : out.frames) {
    std::int64_t sum = 0;
    for (const auto& fr : kv.second) {
      sum += fr.ceil_duration_seconds(cfg_);
    }
    out.times[kv.first] = sum;
    std::sort(kv.second.begin(), kv.second.end(),
               [](const Frame& a, const Frame& b) { return a.start_sys() < b.start_sys(); });
  }
  return out;
}
