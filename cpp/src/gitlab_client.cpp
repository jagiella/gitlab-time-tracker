#include "gitlab_client.hpp"

#include <algorithm>
#include <curl/curl.h>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

#include "config.hpp"
#include "throttle.hpp"

namespace {

size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
  auto* out = static_cast<std::map<std::string, std::string>*>(userdata);
  std::string line(buffer, size * nitems);
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon);
    std::string val = line.substr(colon + 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
      val.erase(val.begin());
    }
    while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) {
      val.pop_back();
    }
    for (auto& c : key) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    (*out)[key] = val;
  }
  return size * nitems;
}

struct CurlInit {
  CurlInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
  ~CurlInit() { curl_global_cleanup(); }
};

CurlInit g_curl_init;

}  // namespace

GitlabClient::GitlabClient(GttConfig& config)
    : config_(config),
      throttle_(config.get_int("throttleMaxRequestsPerInterval", 10), config.get_int("throttleInterval", 1000)) {}

std::string GitlabClient::api_base() const {
  std::string u = config_.get_string("url", "https://gitlab.com/api/v4/");
  if (!u.empty() && u.back() != '/') {
    u.push_back('/');
  }
  return u;
}

std::string GitlabClient::graph_url() const {
  std::string u = api_base();
  if (u.size() >= 3 && u.substr(u.size() - 3) == "v4/") {
    u.resize(u.size() - 3);
  }
  return u + "graphql";
}

GitlabClient::HttpResult GitlabClient::perform(const std::string& method, const std::string& full_url,
                                               const std::string& body,
                                               const std::vector<std::pair<std::string, std::string>>& extra_headers) {
  throttle_.acquire();
  CURL* curl = curl_easy_init();
  HttpResult res;
  if (!curl) {
    return res;
  }
  std::string response;
  std::map<std::string, std::string> hdrs;
  struct curl_slist* slist = nullptr;
  std::string token = config_.get_string("token", "");
  for (const auto& [k, v] : extra_headers) {
    std::string line = k + ": " + v;
    slist = curl_slist_append(slist, line.c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdrs);
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  if (slist) {
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
  }
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "gtt-cpp/1.0");
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  CURLcode cc = curl_easy_perform(curl);
  if (cc == CURLE_OK) {
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    res.status = static_cast<int>(http);
  }
  curl_slist_free_all(slist);
  curl_easy_cleanup(curl);
  res.headers = std::move(hdrs);
  try {
    if (!response.empty() && (response.front() == '{' || response.front() == '[')) {
      res.body = nlohmann::json::parse(response);
    }
  } catch (...) {
    res.body = nlohmann::json::object({{"raw", response}});
  }
  return res;
}

GitlabClient::HttpResult GitlabClient::get(const std::string& path, int page, int per_page) {
  std::string token = config_.get_string("token", "");
  CURL* tmp = curl_easy_init();
  char* esc = tmp ? curl_easy_escape(tmp, token.c_str(), 0) : nullptr;
  std::string tok_q = esc ? esc : token;
  if (esc) {
    curl_free(esc);
  }
  if (tmp) {
    curl_easy_cleanup(tmp);
  }
  std::ostringstream oss;
  oss << api_base() << path;
  if (path.find('?') == std::string::npos) {
    oss << '?';
  } else {
    oss << '&';
  }
  oss << "private_token=" << tok_q;
  oss << "&page=" << page << "&per_page=" << per_page;
  return perform("GET", oss.str(), "",
                 {{"PRIVATE-TOKEN", token}, {"Content-Type", "application/json"}});
}

GitlabClient::HttpResult GitlabClient::post(const std::string& path, const nlohmann::json& data) {
  nlohmann::json payload = data;
  payload["private_token"] = config_.get_string("token", "");
  std::string token = config_.get_string("token", "");
  std::string url = api_base() + path;
  return perform("POST", url, payload.dump(),
                 {{"PRIVATE-TOKEN", token}, {"Content-Type", "application/json"}});
}

GitlabClient::HttpResult GitlabClient::graph_ql(const nlohmann::json& body) {
  std::string token = config_.get_string("token", "");
  return perform("POST", graph_url(), body.dump(),
                 {{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}});
}

nlohmann::json GitlabClient::all_pages(const std::string& path, int per_page, int parallel) {
  HttpResult first = get(path, 1, per_page);
  if (first.status < 200 || first.status >= 300) {
    return nlohmann::json::array();
  }
  nlohmann::json collect = nlohmann::json::array();
  if (!first.body.is_array()) {
    return collect;
  }
  for (const auto& el : first.body) {
    collect.push_back(el);
  }
  auto it = first.headers.find("x-total-pages");
  if (it == first.headers.end()) {
    return collect;
  }
  int pages = std::stoi(it->second);
  if (pages <= 1) {
    return collect;
  }
  int runners = std::max(1, parallel);
  std::mutex mtx;
  for (int page = 2; page <= pages; page += runners) {
    std::vector<std::future<HttpResult>> futs;
    for (int p = page; p < page + runners && p <= pages; ++p) {
      futs.push_back(std::async(std::launch::async, [this, path, p, per_page]() { return get(path, p, per_page); }));
    }
    for (auto& f : futs) {
      HttpResult r = f.get();
      if (r.body.is_array()) {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& el : r.body) {
          collect.push_back(el);
        }
      }
    }
  }
  return collect;
}
