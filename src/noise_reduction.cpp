#include "qo100/noise_reduction.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace qo100 {
NoiseReduction::NoiseReduction(uint32_t percent) {
    if(percent>100) throw std::runtime_error("RX1 NR percent must be 0..100");
    percent_=percent;
    reset();
}
void NoiseReduction::reset() {
    state_.reset(speex_preprocess_state_init(240,12000));
    if(!state_) throw std::runtime_error("Cannot create SpeexDSP denoiser");
    int enabled=1, disabled=0;
    speex_preprocess_ctl(state_.get(),SPEEX_PREPROCESS_SET_DENOISE,&enabled);
    speex_preprocess_ctl(state_.get(),SPEEX_PREPROCESS_SET_AGC,&disabled);
    // VAD and dereverb stay at their disabled defaults. Calling SET_VAD even
    // with zero prints an upstream deprecation warning on every retune.
    mix_=0;
    set(percent_);
}
void NoiseReduction::set(uint32_t percent) {
    if(percent>100) throw std::runtime_error("RX1 NR percent must be 0..100");
    percent_=percent;
    int attenuation=-std::max(1,int(std::lround(.3*percent)));
    speex_preprocess_ctl(state_.get(),SPEEX_PREPROCESS_SET_NOISE_SUPPRESS,&attenuation);
}
void NoiseReduction::process(std::vector<int16_t>& samples) {
    if(samples.size()!=240) throw std::runtime_error("NR requires 240 samples at 12 kHz");
    std::array<int16_t,240> wet;
    std::copy(samples.begin(),samples.end(),wet.begin());
    // Keep learning during bypass, so switching on needs no noise-only calibration.
    speex_preprocess_run(state_.get(),wet.data());
    const float target=percent_ ? 1.f : 0.f;
    for(size_t i=0;i<samples.size();++i) {
        mix_+=std::clamp(target-mix_,-1.f/240,1.f/240);
        if(mix_<1e-5f) { mix_=0; continue; } // Exact PCM bypass after the short crossfade.
        if(mix_>1-1e-5f) mix_=1;
        samples[i]=int16_t(std::lround(samples[i]+mix_*(float(wet[i])-samples[i])));
    }
}
}
