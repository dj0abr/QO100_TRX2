#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include <speex/speex_preprocess.h>

namespace qo100 {
// RX voice audio, 240 PCM16 samples at 12 kHz. Each receiver owns its own state.
class NoiseReduction {
public:
    explicit NoiseReduction(uint32_t percent = 0);
    void set(uint32_t percent);
    uint32_t percent() const { return percent_; }
    void reset();
    void process(std::vector<int16_t>& samples);
private:
    std::unique_ptr<SpeexPreprocessState, decltype(&speex_preprocess_state_destroy)>
        state_{nullptr, speex_preprocess_state_destroy};
    uint32_t percent_ = 0;
    float mix_ = 0;
};
}
