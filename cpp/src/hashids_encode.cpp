#include "hashids_encode.hpp"

extern "C" {
#include "../vendor/hashids_c/hashids.h"
}

#include <chrono>
#include <cstdint>

std::string gtt_encode_frame_id(std::uint64_t n) {
  hashids_t* h = hashids_init(nullptr);
  if (!h) {
    return std::to_string(n);
  }
  unsigned long long num = static_cast<unsigned long long>(n);
  std::size_t est = hashids_estimate_encoded_size(h, 1, &num) + 8;
  std::string buf(est, '\0');
  std::size_t len = hashids_encode_one(h, buf.data(), num);
  hashids_free(h);
  if (len == 0) {
    return std::to_string(n);
  }
  buf.resize(len);
  return buf;
}

std::uint64_t gtt_now_millis_id_seed() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
