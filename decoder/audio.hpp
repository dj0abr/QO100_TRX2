#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace qo100::audio {

inline constexpr unsigned sample_rate = 12000;
inline constexpr std::size_t block_size = 240;
using Pcm = std::array<std::int16_t, block_size>;

struct RxBlock {
    std::uint32_t epoch;
    std::uint64_t index;
    std::uint64_t frequency_hz;
    Pcm pcm;
};

struct TxBlock {
    Pcm pcm{};
    bool last = false;
};

}
