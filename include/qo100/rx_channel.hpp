#pragma once
#include "qo100/dsp.hpp"
#include "qo100/rx_filter.hpp"
#include "qo100/noise_reduction.hpp"
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <samplerate.h>
#include <vector>

namespace qo100 {
// One reusable, independently tuned channel. All rates refer to complex samples.
class RxChannel {
public:
    enum class Profile { normal, multimedia };
    using Zoom = std::function<void(std::complex<float>)>;
    using Audio = std::function<void(const std::vector<int16_t>&, uint64_t)>;
    RxChannel(int64_t offset_hz, Zoom zoom, Audio audio, RxFilterSettings filter = {},
              std::optional<uint32_t> nr_percent = std::nullopt, Profile profile = Profile::normal);
    ~RxChannel();
    RxChannel(const RxChannel&) = delete;
    RxChannel& operator=(const RxChannel&) = delete;
    void tune(int64_t offset_hz);
    // Change frequency while keeping oscillator phase, DSP history and sample clock.
    void retune(int64_t offset_hz);
    void push(std::complex<float> sample);
    // Read-only input shared by receivers. Boundaries do not reset DSP state.
    void push_block(const std::complex<float>* samples, size_t count);
    void set_filter(RxFilterSettings settings) { usb_filter_.set(settings); }
    RxFilterSettings filter_settings() const { return usb_filter_.target(); }
    void set_nr(uint32_t percent) { if(!nr_) throw std::logic_error("NR unavailable on this receiver"); nr_->set(percent); }
    uint32_t nr_percent() const { return nr_ ? nr_->percent() : 0; }
private:
    void push_mixed(std::complex<float> sample);
    void resample();
    LiquidDecimator decimator_;
    RxAudioFilter usb_filter_;
    LiquidFir mm_filter_;
    std::unique_ptr<NoiseReduction> nr_;
    FrequencyShifter mixer_{600000};
    // Reused scratch limits temporary storage to 32 KiB; no allocation per block.
    std::array<std::complex<float>, 4096> block_;
    std::array<std::complex<float>, 8> mixed_{};
    unsigned decimation_ = 0;
    SrcPtr src_{nullptr, src_delete};
    std::vector<float> input_;
    std::vector<int16_t> audio_packet_;
    uint64_t audio_index_ = 0;
    Zoom zoom_;
    Audio audio_;
};
}
