#pragma once
#include <complex>
#include <cstdint>
#include <map>
#include <samplerate.h>
#include <vector>

namespace qo100 {
// One 20 ms block is clocked by the Pluto FIFO feedback, never by GUI arrival.
class TxChannel {
public:
    static constexpr size_t audio_block = 240, iq_block = 24000, target = 1440, capacity = 2880;
    explicit TxChannel(int64_t offset = 150000);
    ~TxChannel();
    TxChannel(const TxChannel&) = delete;
    TxChannel& operator=(const TxChannel&) = delete;
    void reset(int64_t offset);
    bool add(uint64_t index, const std::vector<int16_t>& samples);
    std::vector<std::complex<float>> render(bool audio_recent);
    std::vector<float> pull_audio(bool audio_recent);
    size_t level() const;
    double ppm() const { return correction_ * 1e6; }
    uint64_t underruns = 0, overruns = 0, losses = 0;
private:
    void clear_dsp();
    std::map<uint64_t, std::vector<int16_t>> packets_;
    std::vector<float> pending_, ring_;
    std::vector<std::complex<float>> taps_;
    SRC_STATE *clock_ = nullptr, *interpolator_ = nullptr;
    uint64_t next_ = 0;
    bool have_next_ = false, started_ = false;
    size_t pos_ = 0;
    double correction_ = 0, integral_ = 0, smoothed_ = 0, clock_buffered_ = 0;
    std::complex<double> oscillator_{1,0}, step_{1,0};
};
}
