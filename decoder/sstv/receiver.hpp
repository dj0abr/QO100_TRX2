#pragma once

#include "sstv.hpp"

#include <variant>
#include <vector>

namespace qo100::sstv {

inline constexpr std::size_t pcm_block_size = 240;
inline constexpr std::size_t spectrum_fft_size = 1024;
inline constexpr std::size_t spectrum_bins = 1 + 4000 * spectrum_fft_size / sample_rate;

struct AudioBlock {
    std::uint32_t epoch;
    std::uint64_t index;
    std::uint64_t frequency_hz;
    std::array<std::int16_t, pcm_block_size> pcm;
};

struct Spectrum {
    // 0 through 100 represents -100 through 0 dBFS. Bins start at DC.
    std::array<std::uint8_t, spectrum_bins> levels;
};

class Receiver {
public:
    struct Discontinuity {};
    struct End { bool complete; };
    using Event = std::variant<Discontinuity, Frame, Line, End>;
    struct Output {
        std::vector<Event> events;
        std::optional<Spectrum> spectrum;
    };

    Receiver();
    ~Receiver();
    // Call from one consumer thread. The receiver owns a separate DSP thread.
    bool push(AudioBlock block);
    void reset(std::optional<Mode> forced_mode = {});
    Output take_output();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
