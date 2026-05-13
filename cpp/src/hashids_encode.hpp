#pragma once

#include <cstdint>
#include <string>

/** Frame id compatible with Node hashids (default salt/alphabet). */
std::string gtt_encode_frame_id(std::uint64_t millis_or_any_number);

std::uint64_t gtt_now_millis_id_seed();
