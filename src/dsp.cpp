#include "qo100/dsp.hpp"
#include <cmath>
#include <stdexcept>
#include <vector>

namespace qo100 {
namespace {
constexpr double pi = 3.14159265358979323846;
// Keep the established Kaiser design and default passbands; RX1 can select
// another audio bandwidth and center without changing the other channels.
std::vector<float> lowpass(size_t n, double cutoff, double rate) {
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
void check_liquid(int result) {
    if (result != LIQUID_OK) throw std::runtime_error("liquid-dsp processing error " + std::to_string(result));
}
LiquidDecimator make_rx_decimator() {
    auto h = lowpass(321, 31250, 600000);
    LiquidDecimator result(firdecim_crcf_create(8, h.data(), h.size()));
    if (!result) throw std::runtime_error("Cannot create liquid RX decimator");
    return result;
}
LiquidFir make_usb_filter(float gain, RxFilterSettings settings) {
    if (!settings.valid()) throw std::runtime_error("Invalid USB filter bandwidth/center");
    const auto h = lowpass(513, settings.bandwidth_hz/2., 12000);
    std::vector<std::complex<float>> taps(h.size());
    for (size_t i = 0; i < h.size(); ++i)
        taps[i] = gain * h[i] * std::polar(1.f, float(2*pi*settings.center_hz*(double(i)-256)/12000));
    LiquidFir result(firfilt_cccf_create(taps.data(), taps.size()));
    if (!result) throw std::runtime_error("Cannot create liquid USB filter");
    return result;
}
SrcPtr make_audio_resampler(int quality, int channels) {
    int error = 0;
    SrcPtr result(src_new(quality, channels, &error), src_delete);
    if (!result) throw std::runtime_error(src_strerror(error));
    return result;
}
FrequencyShifter::FrequencyShifter(unsigned sample_rate)
    : nco_(nco_crcf_create(LIQUID_VCO_INTERP)), rate_(sample_rate) {
    if (!nco_ || !rate_) throw std::runtime_error("Cannot create liquid oscillator");
}
void FrequencyShifter::tune(int64_t offset, bool reset_phase) {
    if (offset <= -int64_t(rate_)/2 || offset >= int64_t(rate_)/2)
        throw std::runtime_error("Mixer offset exceeds Nyquist range");
    const bool up = offset >= 0;
    if (reset_phase) check_liquid(nco_crcf_reset(nco_.get()));
    else if (up != up_) check_liquid(nco_crcf_set_phase(nco_.get(), -nco_crcf_get_phase(nco_.get())));
    up_ = up;
    check_liquid(nco_crcf_set_frequency(nco_.get(), float(2*pi*std::abs(double(offset))/rate_)));
}
std::complex<float> FrequencyShifter::execute(std::complex<float> sample) {
    std::complex<float> result;
    check_liquid(up_ ? nco_crcf_mix_up(nco_.get(), sample, &result)
                     : nco_crcf_mix_down(nco_.get(), sample, &result));
    check_liquid(nco_crcf_step(nco_.get()));
    return result;
}
void FrequencyShifter::execute_block(std::complex<float>* samples, unsigned count) {
    check_liquid(up_ ? nco_crcf_mix_block_up(nco_.get(), samples, samples, count)
                     : nco_crcf_mix_block_down(nco_.get(), samples, samples, count));
}
}
