#pragma once
#include "qo100/compressor_settings.hpp"
#include <array>
#include <complex>

namespace qo100 {
// 12-kHz mono RMS compressor. The amount scales both soft-knee gain reduction
// and make-up gain in dB: normal 100% = 4:1 / +18 dB; 140% = 8.8:1 / +25.2 dB.
class SpeechCompressor {
public:
    explicit SpeechCompressor(CompressorSettings settings = {});
    void set(CompressorSettings settings);
    CompressorSettings settings() const { return settings_; }
    void reset();
    float execute(float sample);
    float reduction_db() const;
private:
    double reduction_scale() const;
    CompressorSettings settings_;
    double power_ = 0, reduction_ = 0, amount_ = 0;
};

// Limit the actual oversampled SSB envelope, including FIR/interpolator
// overshoots. A 5-ms delay permits a continuous linear attack before each peak;
// release is 80 ms. The monotonic queue holds future gain constraints.
class TxPeakLimiter {
public:
    static constexpr size_t lookahead = 6000; // 5 ms at 1.2 MS/s
    static constexpr float ceiling = .5f;    // Preserve the existing 6-dB reserve.
    void reset();
    std::complex<float> execute(std::complex<float> sample);
    float reduction_db() const;
private:
    struct Constraint { int64_t index = 0; float gain = 1; };
    std::array<std::complex<float>, lookahead> delay_{};
    std::array<Constraint, lookahead+2> queue_{};
    size_t delay_pos_ = 0, head_ = 0, count_ = 0;
    int64_t index_ = 0;
    double gain_ = 1;
};
}
