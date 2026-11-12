#pragma once

#include "sstv.hpp"
#include "../audio_spectrum.hpp"

#include <variant>
#include <vector>

namespace qo100::sstv {

inline constexpr auto pcm_block_size = audio::block_size;
inline constexpr auto spectrum_fft_size = audio::spectrum_fft_size;
inline constexpr auto spectrum_bins = audio::spectrum_bins;
using AudioBlock = audio::RxBlock;
using Spectrum = audio::Spectrum;

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
