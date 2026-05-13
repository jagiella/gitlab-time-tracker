#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "throttle.hpp"

class GttConfig;

class GitlabClient {
 public:
  explicit GitlabClient(GttConfig& config);

  struct HttpResult {
    int status = 0;
    nlohmann::json body;
    std::map<std::string, std::string> headers;
  };

  HttpResult get(const std::string& path, int page = 1, int per_page = 100);
  HttpResult post(const std::string& path, const nlohmann::json& data);
  HttpResult graph_ql(const nlohmann::json& body);

  /** Collect all pages (expects JSON array body). */
  nlohmann::json all_pages(const std::string& path, int per_page = 100, int parallel = 10);

 private:
  std::string api_base() const;
  std::string graph_url() const;
  HttpResult perform(const std::string& method, const std::string& full_url, const std::string& body,
                     const std::vector<std::pair<std::string, std::string>>& extra_headers);

  GttConfig& config_;
  Throttle throttle_;
};
