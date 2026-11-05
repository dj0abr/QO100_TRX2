#pragma once
#include "qo100/dsp.hpp"
#include "qo100/rx_filter.hpp"
#include <array>
#include <complex>
#include <cstdint>
#include <functional>
#include <samplerate.h>
#include <vector>

namespace qo100 {
// One reusable, independently tuned channel. All rates refer to complex samples.
class RxChannel {
public:
    using Zoom = std::function<void(std::complex<float>)>;
    using Audio = std::function<void(const std::vector<int16_t>&, uint64_t)>;
    RxChannel(int64_t offset_hz, Zoom zoom, Audio audio, RxFilterSettings filter = {});
    ~RxChannel();
    RxChannel(const RxChannel&) = delete;
    RxChannel& operator=(const RxChannel&) = delete;
    void tune(int64_t offset_hz);
    void push(std::complex<float> sample);
    void set_filter(RxFilterSettings settings) { usb_filter_.set(settings); }
    RxFilterSettings filter_settings() const { return usb_filter_.target(); }
private:
    void resample();
    LiquidDecimator decimator_;
    RxAudioFilter usb_filter_;
    FrequencyShifter mixer_{600000};
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
