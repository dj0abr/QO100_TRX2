#pragma once
#include "qo100/dsp.hpp"
#include <array>

namespace qo100 {
// Same-length FIRs have equal delay. Warm the replacement with the preceding
// raw IQ history, then crossfade over 20 ms without resetting audio counters.
class RxAudioFilter {
public:
    explicit RxAudioFilter(RxFilterSettings settings = {});
    void set(RxFilterSettings settings);
    void reset();
    std::complex<float> execute(std::complex<float> sample);
    RxFilterSettings target() const { return target_; }
    RxFilterSettings active() const { return active_; }
    static constexpr unsigned fade_samples = 240;
private:
    LiquidFir filter_, replacement_;
    RxFilterSettings active_, target_, fading_to_;
    std::array<std::complex<float>, 512> history_{};
    size_t history_pos_ = 0, history_count_ = 0;
    unsigned fade_pos_ = 0;
};
}
