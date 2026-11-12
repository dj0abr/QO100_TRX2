#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace qo100::sstv {

inline constexpr unsigned sample_rate = 12000;
inline constexpr unsigned width = 320;
inline constexpr unsigned height = 256;
using RgbLine = std::array<std::uint8_t, width * 3>;
using Image = std::array<RgbLine, height>;

enum class Mode { martin_m1, scottie_1 };

struct Frame {
    Mode mode;
    bool partial; // Reception started after VIS. Line indices are relative to acquisition.
};

struct Line {
    unsigned index; // Row in the raster. Unrecoverable rows are omitted.
    RgbLine rgb;
    bool synchronized;
    double frequency_offset_hz; // Measured audio frequency minus nominal.
};

// Each instance belongs to one thread. Encoder and Decoder instances can run
// concurrently. Callbacks run synchronously on the thread that feeds the decoder.
class Encoder {
public:
    // Linear PCM amplitude relative to the nominal encoder output (0..100%).
    Encoder(Mode mode, const Image& image, unsigned level_percent = 100);
    ~Encoder();
    // Write up to capacity samples. A short read marks the end of the image.
    std::size_t read(std::int16_t* pcm, std::size_t capacity);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Decoder {
public:
    struct Callbacks {
        std::function<void(Frame)> begin;
        std::function<void(const Line&)> line;
        std::function<void(bool complete)> end;
    };

    explicit Decoder(Callbacks callbacks, std::optional<Mode> forced_mode = {});
    ~Decoder();

    void push(const std::int16_t* pcm, std::size_t count);
    // Call between consecutive PCM blocks when the RX2 tuning frequency changes.
    // Positive USB dial steps lower the audio tones by the same amount.
    void retune(double rx_delta_hz);
    // Close a finite recording, including any final line pending in the filter.
    void finish();
    // Drop signal history on a discontinuity such as a new RX2 audio epoch.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
