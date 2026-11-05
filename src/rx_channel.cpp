#include "qo100/rx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace qo100 {
namespace {
constexpr double pi = 3.14159265358979323846;
std::vector<float> fir(size_t n, double cutoff, double rate) {
    std::vector<float> h(n);
    double sum = 0;
    const double beta = 8.6, denom = std::cyl_bessel_i(0., beta);
    for (size_t i = 0; i < n; ++i) {
        const double t = double(i) - (n - 1) / 2.;
        const double v = 2 * cutoff / rate;
        const double sinc = t == 0 ? v : std::sin(pi * v * t) / (pi * t);
        const double x = 2 * double(i) / (n - 1) - 1;
        h[i] = float(sinc * std::cyl_bessel_i(0., beta * std::sqrt(std::max(0., 1 - x*x))) / denom);
        sum += h[i];
    }
    for (auto& x : h) x /= float(sum);
    return h;
}
}
RxChannel::RxChannel(int64_t offset, Zoom zoom, Audio audio)
    : lowpass_(fir(321, 31250, 600000)), wide_ring_(321),
      usb_taps_(513), usb_ring_(513), zoom_(std::move(zoom)), audio_(std::move(audio)) {
    const auto h = fir(513, 1950, 12000);
    // Complex bandpass: +100..+4000 Hz selects USB and rejects the lower sideband.
    for (size_t i = 0; i < h.size(); ++i)
        usb_taps_[i] = h[i] * std::polar(1.f, float(2 * pi * 2050 * (double(i) - 256) / 12000));
    int error = 0;
    src_ = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &error);
    if (!src_) throw std::runtime_error(src_strerror(error));
    input_.reserve(512); audio_packet_.reserve(240);
    tune(offset);
}
RxChannel::~RxChannel() { src_delete(src_); }
void RxChannel::tune(int64_t offset) {
    if (std::abs(offset) > 275000) throw std::runtime_error("Zoom must remain within the 600 kHz RX window");
    step_ = -2 * pi * double(offset) / 600000;
    phase_ = 0; wide_pos_ = usb_pos_ = decimation_ = 0;
    std::fill(wide_ring_.begin(), wide_ring_.end(), std::complex<float>{});
    std::fill(usb_ring_.begin(), usb_ring_.end(), std::complex<float>{});
    input_.clear(); audio_packet_.clear(); audio_index_ = 0;
    src_reset(src_);
}
void RxChannel::push(std::complex<float> x) {
    x *= std::complex<float>(float(std::cos(phase_)), float(std::sin(phase_)));
    phase_ = std::remainder(phase_ + step_, 2 * pi);
    wide_ring_[wide_pos_] = x;
    wide_pos_ = (wide_pos_ + 1) % wide_ring_.size();
    if (++decimation_ != 8) return;
    decimation_ = 0;
    std::complex<float> y{};
    size_t pos = wide_pos_;
    for (size_t i = 0; i < lowpass_.size(); ++i) {
        pos = pos ? pos - 1 : wide_ring_.size() - 1;
        y += wide_ring_[pos] * lowpass_[i];
    }
    zoom_(y);
    input_.push_back(y.real()); input_.push_back(y.imag());
    if (input_.size() == 512) resample();
}
void RxChannel::resample() {
    float output[512];
    long used = 0;
    while (used < 256) {
        SRC_DATA data{};
        data.data_in = input_.data() + 2 * used;
        data.input_frames = 256 - used;
        data.data_out = output; data.output_frames = 256;
        data.src_ratio = 12000. / 75000.;
        const auto error = src_process(src_, &data);
        if (error) throw std::runtime_error(src_strerror(error));
        if (!data.input_frames_used && !data.output_frames_gen) throw std::runtime_error("RX resampler stalled");
        used += data.input_frames_used;
        for (long i = 0; i < data.output_frames_gen; ++i) {
            usb_ring_[usb_pos_] = {output[2*i], output[2*i+1]};
            usb_pos_ = (usb_pos_ + 1) % usb_ring_.size();
            std::complex<float> filtered{};
            size_t pos = usb_pos_;
            for (size_t tap = 0; tap < usb_taps_.size(); ++tap) {
                pos = pos ? pos - 1 : usb_ring_.size() - 1;
                filtered += usb_ring_[pos] * usb_taps_[tap];
            }
            audio_packet_.push_back(static_cast<int16_t>(std::lround(std::clamp(filtered.real(), -1.f, 1.f) * 32767)));
            if (audio_packet_.size() == 240) {
                audio_(audio_packet_, audio_index_);
                audio_index_ += audio_packet_.size();
                audio_packet_.clear();
            }
        }
    }
    input_.clear();
}
}
