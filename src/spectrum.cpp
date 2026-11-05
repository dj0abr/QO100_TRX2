#include "qo100/spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace qo100 {
Spectrum::Spectrum(uint32_t rate, size_t n, uint32_t fps, Callback callback)
    : n_(n), frame_interval_(fps ? rate / fps : 0), ring_(n), window_(n),
      powers_(n, 0), db_(n), callback_(std::move(callback)) {
    if (n < 2 || (n & (n - 1)) || !rate || !fps || !frame_interval_)
        throw std::runtime_error("Invalid spectrum parameters");
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        window_[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * std::acos(-1.) * i / (n - 1)));
        sum += window_[i];
    }
    normalization_ = static_cast<float>(1. / (sum * sum));
    input_ = fftwf_alloc_complex(n);
    output_ = fftwf_alloc_complex(n);
    if (input_ && output_) plan_ = fftwf_plan_dft_1d(static_cast<int>(n), input_, output_, FFTW_FORWARD, FFTW_ESTIMATE);
    if (!plan_) {
        fftwf_free(input_); fftwf_free(output_);
        throw std::runtime_error("Cannot allocate FFTW plan");
    }
}
Spectrum::~Spectrum() {
    fftwf_destroy_plan(plan_);
    fftwf_free(input_); fftwf_free(output_);
}
void Spectrum::reset() {
    write_=count_=since_fft_=averages_=0;
    last_frame_=samples_;
    std::fill(powers_.begin(),powers_.end(),0.f);
}
bool Spectrum::push_iq(const uint8_t* data, size_t size) {
    if (!size || size % 4) return false;
    for (size_t i = 0; i < size; i += 4) {
        const auto re = static_cast<uint16_t>(data[i] | (uint16_t(data[i + 1]) << 8));
        const auto im = static_cast<uint16_t>(data[i + 2] | (uint16_t(data[i + 3]) << 8));
        const int ri = re >= 32768 ? int(re) - 65536 : int(re);
        const int qi = im >= 32768 ? int(im) - 65536 : int(im);
        push({ri / 32768.f, qi / 32768.f});
    }
    return true;
}
void Spectrum::push(std::complex<float> sample) {
    ring_[write_] = sample;
    write_ = (write_ + 1) % n_;
    ++samples_;
    if (count_ < n_) ++count_;
    ++since_fft_;
    if (count_ == n_ && since_fft_ >= n_ / 2) {
        since_fft_ = 0;
        transform();
    }
}
void Spectrum::transform() {
    for (size_t i = 0; i < n_; ++i) {
        const auto x = ring_[(write_ + i) % n_] * window_[i];
        input_[i][0] = x.real(); input_[i][1] = x.imag();
    }
    fftwf_execute(plan_);
    for (size_t i = 0; i < n_; ++i) {
        const auto k = (i + n_ / 2) % n_;
        powers_[i] += (output_[k][0] * output_[k][0] + output_[k][1] * output_[k][1]) * normalization_;
    }
    ++averages_;
    if (samples_ - last_frame_ < frame_interval_) return;
    for (size_t i = 0; i < n_; ++i) {
        const float p = powers_[i] / averages_;
        const float db = 10.f * std::log10(std::max(p, 1e-16f));
        db_[i] = static_cast<int16_t>(std::lround(std::clamp(db, -160.f, 20.f) * 100.f));
        powers_[i] = 0;
    }
    averages_ = 0;
    // Keep the publication clock on the sample timeline; don't accumulate hop rounding.
    last_frame_ += frame_interval_;
    callback_(db_, samples_);
}
}
