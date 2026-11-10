#include "qo100/spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace qo100 {
namespace {
int16_t dbfs(float power) {
    return static_cast<int16_t>(std::lround(std::clamp(10.f * std::log10(std::max(power, 1e-16f)), -160.f, 20.f) * 100.f));
}
void check_powers(const std::vector<float>& powers) {
    if(powers.size()!=spectrum_fft_size) throw std::invalid_argument("Shared spectrum requires 24000 bins");
}
}
SpectrumView wide_spectrum(const std::vector<float>& powers, uint64_t center_hz) {
    check_powers(powers);
    SpectrumView view{center_hz,spectrum_fft_size*spectrum_bin_hz,std::vector<int16_t>(spectrum_wide_bins)};
    constexpr size_t group=spectrum_fft_size/spectrum_wide_bins;
    for(size_t i=0;i<view.bins.size();++i) {
        const auto first=powers.begin()+i*group;
        view.bins[i]=dbfs(*std::max_element(first,first+group));
    }
    return view;
}
SpectrumView zoom_spectrum(const std::vector<float>& powers, uint64_t center_hz, uint64_t receiver_hz) {
    check_powers(powers);
    const int64_t offset=int64_t(receiver_hz)-int64_t(center_hz);
    // Integer Hz and an odd 25-Hz step have no rounding ties. Preserve the actual
    // RF grid in the metadata; the receiver itself remains tunable in 1-Hz steps.
    const int64_t bin=offset>=0 ? (offset+12)/25 : -((-offset+12)/25);
    SpectrumView view{uint64_t(int64_t(center_hz)+bin*25),spectrum_zoom_span,std::vector<int16_t>(spectrum_zoom_bins,-16000)};
    const int64_t first=int64_t(powers.size()/2)+bin-spectrum_zoom_bins/2;
    for(size_t i=0;i<view.bins.size();++i) {
        const int64_t source=first+int64_t(i);
        if(source>=0 && source<int64_t(powers.size())) view.bins[i]=dbfs(powers[size_t(source)]);
    }
    return view;
}
Spectrum::Spectrum(uint32_t rate, size_t n, uint32_t fps, Callback callback)
    : n_(n), frame_interval_(fps ? rate / fps : 0), ring_(n), window_(n),
      powers_(n, 0), callback_(std::move(callback)) {
    if (n < 2 || n%2 || !rate || !fps || !frame_interval_)
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
    if(++write_==n_) write_=0;
    ++samples_;
    if (count_ < n_) ++count_;
    ++since_fft_;
    if (count_ == n_ && since_fft_ >= n_ / 2) {
        since_fft_ = 0;
        transform();
    }
}
void Spectrum::transform() {
    size_t source=write_;
    for (size_t i = 0; i < n_; ++i) {
        const auto x = ring_[source] * window_[i];
        input_[i][0] = x.real(); input_[i][1] = x.imag();
        if(++source==n_) source=0;
    }
    fftwf_execute(plan_);
    size_t k=n_/2;
    for (size_t i = 0; i < n_; ++i) {
        powers_[i] += (output_[k][0] * output_[k][0] + output_[k][1] * output_[k][1]) * normalization_;
        if(++k==n_) k=0;
    }
    ++averages_;
    if (samples_ - last_frame_ < frame_interval_) return;
    for(auto& power:powers_) power/=averages_;
    // Keep the publication clock on the sample timeline; don't accumulate hop rounding.
    last_frame_ += frame_interval_;
    callback_(powers_, samples_);
    std::fill(powers_.begin(),powers_.end(),0.f);
    averages_ = 0;
}
}
