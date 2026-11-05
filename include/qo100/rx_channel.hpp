#pragma once
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
    RxChannel(int64_t offset_hz, Zoom zoom, Audio audio);
    ~RxChannel();
    RxChannel(const RxChannel&) = delete;
    RxChannel& operator=(const RxChannel&) = delete;
    void tune(int64_t offset_hz);
    void push(std::complex<float> sample);
private:
    void resample();
    std::vector<float> lowpass_;
    std::vector<std::complex<float>> wide_ring_, usb_taps_, usb_ring_;
    size_t wide_pos_ = 0, usb_pos_ = 0;
    unsigned decimation_ = 0;
    double phase_ = 0, step_ = 0;
    SRC_STATE* src_ = nullptr;
    std::vector<float> input_;
    std::vector<int16_t> audio_packet_;
    uint64_t audio_index_ = 0;
    Zoom zoom_;
    Audio audio_;
};
}
