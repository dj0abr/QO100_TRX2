#pragma once

#include "sstv.hpp"

namespace qo100::sstv::detail {

inline constexpr double pi = 3.14159265358979323846;
inline constexpr double sync_hz = 1200;
inline constexpr double black_hz = 1500;
inline constexpr double white_hz = 2300;
inline constexpr double leader_hz = 1900;
inline constexpr std::int64_t vis_bit_us = 30000;
inline constexpr std::int64_t vis_data_us = 10 * vis_bit_us;

struct Timing {
    unsigned vis;
    std::int64_t line_us;
    std::int64_t scan_us;
    std::int64_t sync_start_us;
    std::int64_t sync_us;
    std::array<std::int64_t, 3> color_start_us; // In RGB order.
    std::int64_t initial_sync_us;
};

// N7CXI, Proposal for SSTV Mode Specifications, Dayton 2000.
inline constexpr Timing timing(Mode mode) {
    if (mode == Mode::martin_m1) {
        constexpr std::int64_t sync = 4862;
        constexpr std::int64_t porch = 572;
        constexpr std::int64_t scan = 146432;
        constexpr auto green = sync + porch;
        constexpr auto blue = green + scan + porch;
        constexpr auto red = blue + scan + porch;
        return {44, red + scan + porch, scan, 0, sync, {red, green, blue}, 0};
    }
    constexpr std::int64_t sync = 9000;
    constexpr std::int64_t porch = 1500;
    constexpr std::int64_t scan = 138240;
    constexpr auto green = porch;
    constexpr auto blue = green + scan + porch;
    constexpr auto sync_start = blue + scan;
    constexpr auto red = sync_start + sync + porch;
    return {60, red + scan, scan, sync_start, sync, {red, green, blue}, sync};
}

inline constexpr double samples(double microseconds) {
    return microseconds * sample_rate / 1000000.0;
}

}
