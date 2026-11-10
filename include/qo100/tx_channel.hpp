#pragma once
#include "qo100/dsp.hpp"
#include "qo100/speech_compressor.hpp"
#include <complex>
#include <cstdint>
#include <map>
#include <samplerate.h>
#include <vector>

namespace qo100 {
// One 20 ms block is clocked by the Pluto FIFO feedback, never by GUI arrival.
class TxChannel {
public:
    enum class Profile { voice, digital };
    static constexpr size_t audio_block = 240, iq_block = 24000, target = 1440, capacity = 2880;
    explicit TxChannel(int64_t offset = 150000, CompressorSettings compressor = {}, Profile profile = Profile::voice);
    ~TxChannel();
    TxChannel(const TxChannel&) = delete;
    TxChannel& operator=(const TxChannel&) = delete;
    void reset(int64_t offset);
    bool add(uint64_t index, const std::vector<int16_t>& samples);
    std::vector<std::complex<float>> render(bool audio_recent);
    // Empty means known-silent IQ; caller still advances the hardware block clock.
    std::vector<std::complex<float>> render_active(bool audio_recent);
    std::vector<float> pull_audio(bool audio_recent);
    size_t level() const;
    double ppm() const { return correction_ * 1e6; }
    void set_compressor(CompressorSettings settings) { compressor_.set(settings); }
    CompressorSettings compressor_settings() const { return compressor_.settings(); }
    float compression_db() const { return compressor_.reduction_db(); }
    float limiting_db() const { return limiter_.reduction_db(); }
    uint64_t underruns = 0, overruns = 0, losses = 0;
private:
    Profile profile_;
    void clear_dsp();
    std::map<uint64_t, std::vector<int16_t>> packets_;
    std::vector<float> pending_;
    SrcPtr clock_{nullptr, src_delete};
    LiquidFir usb_filter_;
    LiquidInterpolator interpolator_;
    FrequencyShifter mixer_{1200000};
    SpeechCompressor compressor_;
    TxPeakLimiter limiter_;
    uint64_t next_ = 0;
    bool have_next_ = false, started_ = false;
    double correction_ = 0, integral_ = 0, smoothed_ = 0, clock_buffered_ = 0;
};
}
