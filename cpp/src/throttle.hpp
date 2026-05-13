#pragma once

#include <chrono>
#include <mutex>

class Throttle {
 public:
  Throttle(int max_per_interval, int interval_ms);

  void acquire();

 private:
  int max_;
  int interval_ms_;
  std::mutex mtx_;
  std::chrono::steady_clock::time_point window_start_;
  int count_{0};
};
