#pragma once
#include <fftw3.h>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace qo100 {
class Spectrum {
public:
    using Callback = std::function<void(const std::vector<int16_t>&, uint64_t)>;
    Spectrum(uint32_t sample_rate, size_t fft_size, uint32_t fps, Callback callback);
    ~Spectrum();
    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;
    bool push_iq(const uint8_t* data, size_t size);
    void push(std::complex<float> sample);
    uint64_t samples() const { return samples_; }
private:
    void transform();
    size_t n_, write_ = 0, count_ = 0, since_fft_ = 0;
    uint64_t samples_ = 0, last_frame_ = 0, frame_interval_;
    unsigned averages_ = 0;
    float normalization_ = 1;
    std::vector<std::complex<float>> ring_;
    std::vector<float> window_, powers_;
    std::vector<int16_t> db_;
    fftwf_complex* input_ = nullptr;
    fftwf_complex* output_ = nullptr;
    fftwf_plan plan_ = nullptr;
    Callback callback_;
};
}
