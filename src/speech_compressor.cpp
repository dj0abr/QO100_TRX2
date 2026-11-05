#include "qo100/speech_compressor.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qo100 {
namespace {
const double rms_alpha = std::exp(-1./(12000*.005));
const double attack_alpha = std::exp(-1./(12000*.003));
const double release_alpha = std::exp(-1./(12000*.150));
const double limiter_release = std::exp(-1./(1200000*.080));
}
SpeechCompressor::SpeechCompressor(CompressorSettings settings) {
    set(settings); reset();
}
void SpeechCompressor::set(CompressorSettings settings) {
    if (!settings.valid()) throw std::runtime_error("TX1 compressor amount must be 0..1400");
    settings_ = settings;
}
void SpeechCompressor::reset() {
    power_ = reduction_ = 0;
    amount_ = settings_.amount/1000.;
}
float SpeechCompressor::execute(float sample) {
    // Keep detector history even in bypass so enabling does not boost an
    // already-loud signal while the detector is still starting from silence.
    power_ = rms_alpha*power_ + (1-rms_alpha)*double(sample)*sample;
    if (power_<1e-12) power_=0; // Avoid denormals during long silent microphone input.
    const double over = 10*std::log10(std::max(power_, 1e-12)) + 24; // threshold -24 dBFS
    const double desired = over<=-3 ? 0 : over>=3 ? .75*over : .75*(over+3)*(over+3)/12;
    const double alpha = desired>reduction_ ? attack_alpha : release_alpha;
    reduction_ = alpha*reduction_ + (1-alpha)*desired;
    if (reduction_<1e-12) reduction_=0;
    const double target = settings_.amount/1000.;
    amount_ += std::clamp(target-amount_, -1./240, 1./240); // 100 percentage points per 20 ms
    if (amount_ == 0) return sample;
    return float(sample*std::pow(10., (18*amount_-reduction_*reduction_scale())/20));
}
double SpeechCompressor::reduction_scale() const {
    if(amount_<=1) return amount_; // Preserve the proven curve through 100%.
    // Extend ratio with matching value/slope at 100%, always below infinite
    // compression. Extending the old dB weight linearly would invert dynamics
    // above 133.3%. R=4+12*(u-1) reaches 8.8:1 at 140%.
    const double ratio=4+12*(amount_-1);
    return (1-1/ratio)/.75;
}
float SpeechCompressor::reduction_db() const { return float(reduction_*reduction_scale()); }
void TxPeakLimiter::reset() {
    delay_.fill({}); delay_pos_=head_=count_=0;index_=0;gain_=1;
}
std::complex<float> TxPeakLimiter::execute(std::complex<float> sample) {
    const int64_t output_index=index_-int64_t(lookahead);
    while (count_ && queue_[head_].index<output_index) {
        head_=(head_+1)%queue_.size();--count_;
    }
    const float amplitude=std::abs(sample);
    const float required=amplitude>ceiling ? ceiling/amplitude : 1.f;
    // Store the minimum of gain[k] + (k-output_index)/lookahead. A new peak
    // enters at <= unity and ramps down over the delay, reaching its exact
    // gain bound when that sample leaves the delay. No I/Q samples are clipped.
    while(count_) {
        const auto& last=queue_[(head_+count_-1)%queue_.size()];
        if(last.gain+double(last.index-index_)/lookahead<required) break;
        --count_;
    }
    queue_[(head_+count_)%queue_.size()]={index_,required};++count_;
    const auto& first=queue_[head_];
    const double bound=std::min(1., first.gain+double(first.index-output_index)/lookahead);
    gain_=std::min(bound, limiter_release*gain_+(1-limiter_release));
    const auto result=delay_[delay_pos_]*float(gain_);
    delay_[delay_pos_]=sample;
    delay_pos_=(delay_pos_+1)%delay_.size();++index_;
    return result;
}
float TxPeakLimiter::reduction_db() const { return float(-20*std::log10(std::max(gain_, 1e-12))); }
}
