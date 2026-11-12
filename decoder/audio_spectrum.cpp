#include "audio_spectrum.hpp"

#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace qo100::audio {
namespace {
constexpr double pi = 3.14159265358979323846;
}

struct AudioSpectrum::Impl {
    Impl() {
        for (std::size_t i = 0; i < window_.size(); ++i) {
            window_[i] = 0.5f - 0.5f * std::cos(2 * pi * i / window_.size());
        }
        plan_ = fftwf_plan_dft_r2c_1d(spectrum_fft_size, input_.data(), output_, FFTW_ESTIMATE);
        if (!plan_) throw std::runtime_error("Cannot create audio spectrum FFT");
    }

    ~Impl() { fftwf_destroy_plan(plan_); }

    void reset() { pos_ = count_ = 0; }

    std::optional<Spectrum> push(const Pcm& pcm) {
        for (const auto sample : pcm) {
            samples_[pos_] = sample / 32768.f;
            pos_ = (pos_ + 1) % samples_.size();
        }
        count_ += pcm.size();
        if (count_ < sample_rate / 10) return {};
        count_ = 0;

        for (std::size_t i = 0; i < input_.size(); ++i)
            input_[i] = samples_[(pos_ + i) % samples_.size()] * window_[i];
        fftwf_execute(plan_);

        Spectrum spectrum{};
        for (std::size_t i = 0; i < spectrum.levels.size(); ++i) {
            const auto amplitude = std::hypot(output_[i][0], output_[i][1]) * (4.f / spectrum_fft_size);
            const auto dbfs = 20.f * std::log10(std::max(amplitude, 0.00001f));
            spectrum.levels[i] = std::uint8_t(std::lround(std::clamp(dbfs + 100.f, 0.f, 100.f)));
        }
        return spectrum;
    }

    std::array<float, spectrum_fft_size> samples_{}, window_{}, input_{};
    fftwf_complex output_[spectrum_fft_size / 2 + 1]{};
    fftwf_plan plan_;
    std::size_t pos_ = 0, count_ = 0;
};

AudioSpectrum::AudioSpectrum() : impl_(std::make_unique<Impl>()) {}
AudioSpectrum::~AudioSpectrum() = default;
void AudioSpectrum::reset() { impl_->reset(); }
std::optional<Spectrum> AudioSpectrum::push(const Pcm& pcm) { return impl_->push(pcm); }

}
