#include "timing.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace qo100::sstv {

using namespace detail;

struct Encoder::Impl {
    struct Tone {
        double hz;
        std::int64_t end_sample;
    };

    Timing format;
    Image image;
    const double amplitude;
    std::vector<Tone> tones;
    std::size_t tone = 0;
    unsigned row = 0;
    std::int64_t line_start_us = 0;
    std::int64_t sample = 0;
    double phase = 0;

    Impl(Mode mode, const Image& pixels, unsigned level_percent)
        : format(timing(mode)), image(pixels), amplitude(24576.0 * level_percent / 100) {
        if (level_percent > 100) throw std::invalid_argument("SSTV TX level must be between 0 and 100");
        tones.reserve(3 * width + 5);
        append(leader_hz, 300000);
        append(sync_hz, 310000);
        append(leader_hz, 610000);
        append(sync_hz, 640000);
        unsigned parity = 0;
        for (unsigned bit = 0; bit < 7; ++bit) {
            const auto value = (format.vis >> bit) & 1;
            parity ^= value;
            append(value ? 1100 : 1300, 670000 + bit * vis_bit_us);
        }
        append(parity ? 1100 : 1300, 880000);
        line_start_us = 910000 + format.initial_sync_us;
        append(sync_hz, line_start_us);
    }

    void append(double hz, double end_us) {
        tones.push_back({hz, std::llround(samples(end_us))});
    }

    void encode_line() {
        tones.clear();
        tone = 0;
        for (unsigned color : {1u, 2u, 0u}) {
            if ((color == 1 && format.sync_start_us == 0) ||
                (color == 0 && format.sync_start_us != 0)) {
                append(sync_hz, line_start_us + format.sync_start_us + format.sync_us);
            }
            const auto start = line_start_us + format.color_start_us[color];
            append(black_hz, start);
            for (unsigned x = 0; x < width; ++x) {
                const auto hz = black_hz + (white_hz - black_hz) * image[row][3 * x + color] / 255.0;
                append(hz, start + double(format.scan_us) * (x + 1) / width);
            }
        }
        line_start_us += format.line_us;
        append(black_hz, line_start_us);
        ++row;
    }

    std::size_t read(std::int16_t* pcm, std::size_t capacity) {
        std::size_t written = 0;
        while (written < capacity) {
            if (tone == tones.size()) {
                if (row == height) break;
                encode_line();
            }
            const auto current = tones[tone];
            const double step = 2 * pi * current.hz / sample_rate;
            while (sample < current.end_sample && written < capacity) {
                pcm[written++] = std::int16_t(std::lround(amplitude * std::sin(phase)));
                phase += step;
                if (phase >= 2 * pi) phase -= 2 * pi;
                ++sample;
            }
            if (sample == current.end_sample) ++tone;
        }
        return written;
    }
};

Encoder::Encoder(Mode mode, const Image& image, unsigned level_percent)
    : impl_(std::make_unique<Impl>(mode, image, level_percent)) {}
Encoder::~Encoder() = default;
std::size_t Encoder::read(std::int16_t* pcm, std::size_t capacity) { return impl_->read(pcm, capacity); }

}
