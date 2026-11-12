#pragma once

#include "audio.hpp"

#include <memory>
#include <optional>

namespace qo100::audio {

inline constexpr std::size_t spectrum_fft_size = 1024;
inline constexpr std::size_t spectrum_bins = 1 + 4000 * spectrum_fft_size / sample_rate;

struct Spectrum {
    // 0 through 100 represents -100 through 0 dBFS. Bins start at DC.
    std::array<std::uint8_t, spectrum_bins> levels;
};

class AudioSpectrum {
public:
    AudioSpectrum();
    ~AudioSpectrum();
    void reset();
    std::optional<Spectrum> push(const Pcm& pcm);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
