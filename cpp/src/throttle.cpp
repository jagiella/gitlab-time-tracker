#include "throttle.hpp"

#include <thread>

Throttle::Throttle(int max_per_interval, int interval_ms)
    : max_(max_per_interval > 0 ? max_per_interval : 10),
      interval_ms_(interval_ms > 0 ? interval_ms : 1000),
      window_start_(std::chrono::steady_clock::now()) {}

void Throttle::acquire() {
  using clock = std::chrono::steady_clock;
  std::unique_lock<std::mutex> lock(mtx_);
  while (true) {
    auto now = clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - window_start_).count();
    if (elapsed >= interval_ms_) {
      window_start_ = now;
      count_ = 0;
    }
    if (count_ < max_) {
      ++count_;
      return;
    }
    int wait_ms = interval_ms_ - static_cast<int>(elapsed);
    if (wait_ms < 1) {
      wait_ms = 1;
    }
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
    lock.lock();
    window_start_ = clock::now();
    count_ = 0;
  }
}
