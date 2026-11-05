#pragma once
#include <algorithm>
#include <complex>
#include <cstdint>
#include <liquid/liquid.h>
#include <memory>
#include <samplerate.h>
#include <type_traits>

static_assert(LIQUID_VERSION_MAJOR == 1 && LIQUID_VERSION_MINOR == 8 && LIQUID_VERSION_PATCH == 3, "QO100 requires the bundled liquid-dsp 1.8.3 header");

namespace qo100 {
template<typename T, int (*Destroy)(T)> struct LiquidDeleter {
    void operator()(T object) const noexcept { if (object) (void)Destroy(object); }
};
template<typename T, int (*Destroy)(T)>
using LiquidPtr = std::unique_ptr<std::remove_pointer_t<T>, LiquidDeleter<T, Destroy>>;
using LiquidFir = LiquidPtr<firfilt_cccf, firfilt_cccf_destroy>;
using LiquidDecimator = LiquidPtr<firdecim_crcf, firdecim_crcf_destroy>;
using LiquidInterpolator = LiquidPtr<firinterp_crcf, firinterp_crcf_destroy>;
using LiquidNco = LiquidPtr<nco_crcf, nco_crcf_destroy>;
using SrcPtr = std::unique_ptr<SRC_STATE, decltype(&src_delete)>;

void check_liquid(int result);
LiquidDecimator make_rx_decimator();
LiquidFir make_usb_filter(float gain);
SrcPtr make_audio_resampler(int quality, int channels);

// Interpolated oscillator: avoid the coarse LIQUID_NCO lookup table. Use a
// positive phase step and select the mixing direction to retain precision
// for small negative frequency offsets as well.
class FrequencyShifter {
public:
    explicit FrequencyShifter(unsigned sample_rate);
    void tune(int64_t offset_hz);
    std::complex<float> execute(std::complex<float> sample);
    void execute_block(std::complex<float>* samples, unsigned count);
private:
    LiquidNco nco_;
    unsigned rate_;
    bool up_ = true;
};
}
