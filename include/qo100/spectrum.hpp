#pragma once
#include <fftw3.h>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace qo100 {
constexpr uint32_t spectrum_fft_size = 24000, spectrum_bin_hz = 25;
constexpr uint32_t spectrum_wide_bins = 2000, spectrum_zoom_bins = 2400;
constexpr uint32_t spectrum_zoom_span = spectrum_zoom_bins * spectrum_bin_hz;

// Output geometry describes transmitted bins, independently of the analysis FFT size.
struct SpectrumView {
    uint64_t center_hz;
    uint32_t span_hz;
    std::vector<int16_t> bins;
};
SpectrumView wide_spectrum(const std::vector<float>& powers, uint64_t center_hz);
SpectrumView zoom_spectrum(const std::vector<float>& powers, uint64_t center_hz, uint64_t receiver_hz);

class Spectrum {
public:
    // Frequency-ordered, Hann-normalized mean linear powers. Valid during callback only.
    using Callback = std::function<void(const std::vector<float>&, uint64_t)>;
    Spectrum(uint32_t sample_rate, size_t fft_size, uint32_t fps, Callback callback);
    ~Spectrum();
    Spectrum(const Spectrum&) = delete;
    Spectrum& operator=(const Spectrum&) = delete;
    bool push_iq(const uint8_t* data, size_t size);
    void push(std::complex<float> sample);
    void reset(); // Discard spectral history after hardware retuning; retain sample clock.
    uint64_t samples() const { return samples_; }
private:
    void transform();
    size_t n_, write_ = 0, count_ = 0, since_fft_ = 0;
    uint64_t samples_ = 0, last_frame_ = 0, frame_interval_;
    unsigned averages_ = 0;
    float normalization_ = 1;
    std::vector<std::complex<float>> ring_;
    std::vector<float> window_, powers_;
    fftwf_complex* input_ = nullptr;
    fftwf_complex* output_ = nullptr;
    fftwf_plan plan_ = nullptr;
    Callback callback_;
};
}
