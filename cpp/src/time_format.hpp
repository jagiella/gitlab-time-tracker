#pragma once

#include <cstdint>
#include <string>

namespace gtt::timefmt {

std::int64_t parse_human_duration(const std::string& string, int hours_per_day = 8, int days_per_week = 5,
                                    int weeks_per_month = 4);

std::string to_human_readable(std::int64_t input_seconds, int hours_per_day = 8,
                              const std::string& format = "[%sign][%days>d ][%hours>h ][%minutes>m ][%seconds>s]");

}  // namespace gtt::timefmt
