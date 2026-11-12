#include "sstv/sstv.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using namespace qo100::sstv;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Image test_image() {
    Image result{};
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x)
            for (unsigned color = 0; color < 3; ++color)
                result[y][3 * x + color] = 25 + (x / 80 * 44 + y / 16 * 19 + color * 71) % 202;
    return result;
}

// Build an independent signal from the published line sequences. It can omit
// the header, shift USB tone frequencies, stretch timing and blank sync pulses.
struct Signal {
    std::vector<std::int16_t> pcm;
    double end_time = 0;
    double phase = 0;
    double clock_scale;
    double offset;
    unsigned noise_state = 23;

    void tone(double hz, double milliseconds) {
        end_time += milliseconds;
        const auto until = std::size_t(std::llround(end_time * 12 * clock_scale));
        const double step = 2 * std::acos(-1.0) * (hz + offset) / sample_rate;
        while (pcm.size() < until) {
            noise_state = 1664525u * noise_state + 1013904223u;
            const auto noise = int(noise_state >> 23) - 256;
            pcm.push_back(hz ? std::int16_t(22000 * std::sin(phase) + noise) : 0);
            phase = std::remainder(phase + step, 2 * std::acos(-1.0));
        }
    }
};

using RowRange = std::pair<unsigned, unsigned>;
enum class Variation { none, afc, timing, false_sync };

Signal reference(const Image& image, Mode mode, bool header, bool disturbances, std::size_t& retune_at,
                 const std::vector<RowRange>& outages = {}, Variation variation = Variation::none) {
    const bool scottie = mode == Mode::scottie_1;
    Signal signal{{}, 0, 0, disturbances ? 1.002 : 1.0, scottie ? -190.0 : 180.0};
    signal.tone(0, 43);
    if (header) {
        signal.tone(1900, 300);
        signal.tone(1200, 10);
        signal.tone(1900, 300);
        signal.tone(1200, 30);
        // M1 has an odd number of one bits and S1 an even
        // number. Send LSB first, then parity.
        const unsigned vis_and_parity = scottie ? 60 : 44 + 128;
        for (unsigned bit = 0; bit < 8; ++bit)
            signal.tone(vis_and_parity & (1u << bit) ? 1100 : 1300, 30);
        // Exercise a Martin sender whose first line starts early relative
        // to the nominal VIS stop-bit boundary.
        signal.tone(1200, disturbances && !scottie ? 24 : 30);
        if (scottie) signal.tone(1200, 9);
    }

    for (unsigned row = 0; row < height; ++row) {
        const auto outage = std::find_if(outages.begin(), outages.end(),
            [row](const auto& range) { return row >= range.first && row < range.second; });
        const bool blank_sync = (disturbances && (row == 90 || row == 91)) || outage != outages.end() ||
            (variation == Variation::false_sync && row >= 77 && row < 80);
        // Move the first sync after the longest gap outside the locked gate.
        if (!outages.empty() && row == outages.back().second) signal.tone(0, 12);
        if (variation == Variation::afc && row >= 150 && row < 171) signal.offset += 2;
        const double sync_jitter = variation == Variation::afc && row >= 15 && row < 100 ? (row % 2 ? 40 : -40) : 0;
        const double sync = blank_sync ? 0 : 1200 + sync_jitter;
        // Vary the sync edge while leaving the color scans at their nominal positions.
        const double edge_jitter = variation == Variation::timing && row >= 16 && row < 64
            ? (row % 2 ? -1.4 : 0.35) : 0;
        // Replace the real pulse with one at the wrong phase, once while
        // locked and once after three missing syncs. Keep the row length fixed.
        const double delay = variation == Variation::false_sync && row == 40 ? 4.5 :
            variation == Variation::false_sync && row == 80 ? 9 : 0;
        for (unsigned color : {1u, 2u, 0u}) {
            const bool after_sync = scottie ? color == 0 : color == 1;
            if (after_sync) {
                if (delay > 0) signal.tone(1500, delay);
                signal.tone(sync, (scottie ? 9 : 4.862) + edge_jitter);
            }
            const double porch = scottie ? 1.5 : 0.572;
            signal.tone(1500, porch - (after_sync ? edge_jitter : 0));
            double skip = after_sync ? delay : 0;
            const double pixel_ms = (scottie ? 138.240 : 146.432) / width;
            for (unsigned x = 0; x < width; ++x) {
                if (disturbances && row == 65 && color == 1 && x == 157) {
                    retune_at = signal.pcm.size();
                    signal.offset -= 110;
                }
                const double omitted = std::min(skip, pixel_ms);
                skip -= omitted;
                if (omitted < pixel_ms)
                    signal.tone(1500 + 800.0 * image[row][3 * x + color] / 255, pixel_ms - omitted);
            }
        }
        if (!scottie) signal.tone(1500, 0.572);
    }
    return signal;
}

struct Recording {
    std::vector<Frame> frames;
    std::vector<Line> lines;
    std::vector<bool> endings;

    Decoder::Callbacks callbacks() {
        return {[this](Frame frame) { frames.push_back(frame); },
            [this](const Line& line) {
                require(lines.empty() || line.index > lines.back().index, "line number restarted");
                lines.push_back(line);
            },
            [this](bool complete) { endings.push_back(complete); }};
    }
};

void check_colors(const Image& image, const Recording& recording, unsigned first_row, unsigned count,
                  double maximum_error = 5) {
    int total_error = 0;
    unsigned pixels = 0;
    for (unsigned row = 0; row < count; ++row) {
        for (unsigned x : {35u, 115u, 195u, 275u}) {
            for (unsigned color = 0; color < 3; ++color) {
                const auto index = 3 * x + color;
                total_error += std::abs(int(recording.lines[row].rgb[index]) - image[first_row + row][index]);
                ++pixels;
            }
        }
    }
    require(double(total_error) / pixels < maximum_error, "average RGB error exceeded the tolerance");
}

void test_reference(Mode mode, bool header) {
    const auto image = test_image();
    std::size_t retune_at = 0;
    const auto audio = reference(image, mode, header, header, retune_at);
    Recording recording;
    Decoder decoder(recording.callbacks());
    for (std::size_t i = 0; i < audio.pcm.size();) {
        if (retune_at && i == retune_at) decoder.retune(110);
        const auto boundary = retune_at > i ? retune_at : audio.pcm.size();
        const auto count = std::min<std::size_t>(240, boundary - i);
        decoder.push(audio.pcm.data() + i, count);
        i += count;
        if (header && i > sample_rate * 10 && i < sample_rate * 11)
            require(recording.lines.size() > 12, "rows were not delivered during reception");
    }
    decoder.finish();
    require(recording.frames.size() == 1 && recording.frames[0].mode == mode, "wrong mode or frame count");
    require(recording.frames[0].partial == !header, "incorrect partial image flag");
    if (header) {
        require(recording.lines.size() == height, "did not decode 256 lines");
        require(recording.endings == std::vector<bool>{true}, "frame did not complete");
        require(!recording.lines[90].synchronized && !recording.lines[91].synchronized &&
            recording.lines[92].synchronized, "sync did not recover after two absent pulses");
        require(std::abs(recording.lines[50].frequency_offset_hz - audio.offset - 110) < 8,
            "bad AFC before retune");
        require(std::abs(recording.lines[75].frequency_offset_hz - audio.offset) < 8,
            "AFC did not account for RX retune");
        check_colors(image, recording, 0, height);
    } else {
        require(recording.lines.size() >= height - 8 && recording.lines.size() <= height,
            "did not acquire the signal after missing VIS");
        // Absolute row is unknown to the decoder. Align the captured suffix
        // to the end of this reference image to check its color content.
        check_colors(image, recording, height - recording.lines.size(), recording.lines.size());
    }
}

void test_encoder(Mode mode) {
    const auto image = test_image();
    Encoder encoder(mode, image);
    Recording recording;
    Decoder decoder(recording.callbacks());
    std::array<std::int16_t, 240> block{};
    std::size_t total = 0;
    for (std::size_t count; (count = encoder.read(block.data(), block.size())) != 0;) {
        decoder.push(block.data(), count);
        total += count;
    }
    decoder.finish();
    const auto duration_us = mode == Mode::martin_m1 ? 910000 + 256 * 446446 : 919000 + 256 * 428220;
    require(total == std::size_t(std::llround(duration_us * 0.012)), "wrong encoded image duration");
    require(recording.frames.size() == 1 && recording.frames[0].mode == mode &&
        recording.lines.size() == height, "encoder/decoder round trip failed");
    check_colors(image, recording, 0, height);
}

void test_weak_signal(Mode mode, bool header) {
    const auto image = test_image();
    std::size_t unused = 0;
    auto signal = reference(image, mode, header, false, unused);
    // Low-level audio with broadband noise. The phase estimate
    // jitters even when the 1200 Hz sync is clear in the audio spectrum.
    unsigned random = 0x8aef37u;
    for (auto& sample : signal.pcm) {
        random = 1664525u * random + 1013904223u;
        const int noise = int(random >> 16) % 641 - 320;
        sample = std::int16_t(sample / 36 + noise);
    }

    Recording recording;
    Decoder decoder(recording.callbacks());
    for (std::size_t i = 0; i < signal.pcm.size(); i += 240)
        decoder.push(signal.pcm.data() + i, std::min<std::size_t>(240, signal.pcm.size() - i));
    decoder.finish();
    require(recording.frames.size() == 1 && recording.frames[0].mode == mode &&
        recording.frames[0].partial == !header, "did not correctly acquire the weak SSTV signal");
    require(recording.lines.size() >= (header ? height : height - 20), "lost weak SSTV signal before the last lines");
    require(std::count_if(recording.lines.begin(), recording.lines.end(),
        [](const Line& line) { return line.synchronized; }) >= height * 9 / 10,
        "too many missed sync pulses in weak SSTV signal");
    if (header) {
        require(recording.endings == std::vector<bool>{true}, "weak image did not complete");
        check_colors(image, recording, 0, height, 9);
    }
}

void test_reacquisition(Mode mode, bool header) {
    const auto image = test_image();
    std::size_t unused = 0;
    const std::vector<RowRange> outages{{28, 34}, {62, 69}, {111, 117}, {153, 161}, {201, header ? 224u : 212u}};
    const auto signal = reference(image, mode, header, false, unused, outages);
    Recording recording;
    Decoder decoder(recording.callbacks());
    for (std::size_t i = 0; i < signal.pcm.size(); i += 240)
        decoder.push(signal.pcm.data() + i, std::min<std::size_t>(240, signal.pcm.size() - i));
    decoder.finish();

    require(recording.frames.size() == 1 && recording.frames.front().partial == !header,
        "created a new image after an interruption");
    require(recording.endings == std::vector<bool>{false}, "reported an image with missing rows as complete");
    require(!recording.lines.empty() && recording.lines.back().index >= (header ? height - 1 : height - 8),
        "did not resume to the final row");
    const unsigned row_offset = height - 1 - recording.lines.back().index;

    for (const auto& [first, end] : outages) {
        // After the final gap the whole signal is 12 ms late. A second
        // off-phase pulse must confirm that this really is a timing shift.
        const unsigned expected_row = end - row_offset + (end == outages.back().second ? 1 : 0);
        const auto resumed = std::find_if(recording.lines.begin(), recording.lines.end(),
            [expected_row](const Line& line) { return line.index >= expected_row; });
        require(resumed != recording.lines.end() && resumed->index == expected_row && resumed->synchronized,
            "did not regain sync at the correct row");
        require(std::none_of(recording.lines.begin(), recording.lines.end(),
            [&](const Line& line) { return line.index + row_offset >= first + 3 && line.index < expected_row; }),
            "emitted estimated rows throughout the interruption");
        for (unsigned x : {35u, 115u, 195u, 275u})
            for (unsigned color = 0; color < 3; ++color) {
                const auto index = 3 * x + color;
                require(std::abs(int(resumed->rgb[index]) - image[resumed->index + row_offset][index]) < 8,
                    "recovered row is misaligned");
            }
    }
}

void test_afc(Mode mode) {
    const auto image = test_image();
    std::size_t unused = 0;
    const auto signal = reference(image, mode, true, false, unused, {}, Variation::afc);
    Recording recording;
    Decoder decoder(recording.callbacks());
    for (std::size_t i = 0; i < signal.pcm.size(); i += 240)
        decoder.push(signal.pcm.data() + i, std::min<std::size_t>(240, signal.pcm.size() - i));
    decoder.finish();

    require(recording.lines.size() == height && recording.endings == std::vector<bool>{true},
        "AFC test image did not complete");
    for (unsigned row = 20; row < 100; ++row) {
        require(recording.lines[row].synchronized, "frequency jitter caused a loss of line sync");
        require(std::abs(recording.lines[row].frequency_offset_hz - recording.lines[row - 1].frequency_offset_hz) < 3,
            "AFC followed an individual sync pulse too closely");
    }
    require(std::abs(recording.lines.back().frequency_offset_hz - signal.offset) < 8,
        "AFC did not follow a sustained frequency drift");
}

void test_line_clock(Mode mode) {
    const auto image = test_image();
    std::size_t unused = 0;
    const auto signal = reference(image, mode, true, false, unused, {}, Variation::timing);
    Recording recording;
    Decoder decoder(recording.callbacks());
    for (std::size_t i = 0; i < signal.pcm.size(); i += 240)
        decoder.push(signal.pcm.data() + i, std::min<std::size_t>(240, signal.pcm.size() - i));
    decoder.finish();
    require(recording.lines.size() == height && recording.endings == std::vector<bool>{true},
        "timing jitter interrupted the image");

    // These rows have an identical transition in the green scan at x=80.
    // Its position should remain stable when the received sync edge varies.
    constexpr unsigned color = 1;
    const double midpoint = (image[32][3 * 79 + color] + image[32][3 * 80 + color]) / 2.0;
    double first = width;
    double last = 0;
    for (unsigned row = 32; row < 48; ++row) {
        const auto& line = recording.lines[row];
        require(line.synchronized, "timing jitter caused a loss of line sync");
        bool crossed = false;
        for (unsigned x = 73; x < 87; ++x) {
            const double a = line.rgb[3 * x + color], b = line.rgb[3 * (x + 1) + color];
            if (a < midpoint && b >= midpoint) {
                const double edge = x + (midpoint - a) / (b - a);
                first = std::min(first, edge);
                last = std::max(last, edge);
                crossed = true;
                break;
            }
        }
        require(crossed, "test image lost its color transition");
    }
    require(last - first < 2.5, "color edge followed sync jitter");
}

void test_false_sync(Mode mode) {
    const auto image = test_image();
    std::size_t unused = 0;
    const auto signal = reference(image, mode, true, false, unused, {}, Variation::false_sync);
    Recording recording;
    Decoder decoder(recording.callbacks());
    for (std::size_t i = 0; i < signal.pcm.size(); i += 240)
        decoder.push(signal.pcm.data() + i, std::min<std::size_t>(240, signal.pcm.size() - i));
    decoder.finish();

    require(recording.frames.size() == 1 && recording.lines.size() == height - 1,
        "false sync interrupted the image");
    require(recording.lines[40].index == 40 && !recording.lines[40].synchronized,
        "accepted a false pulse while locked");
    // The nearby green edge belongs to the same color bars on both rows.
    int edge_error = 0;
    for (unsigned x = 74; x < 86; ++x) {
        const auto index = 3 * x + 1;
        edge_error += std::abs(int(recording.lines[40].rgb[index]) - recording.lines[39].rgb[index]);
    }
    require(edge_error < 48, "false pulse shifted the affected line");
    require(recording.lines[41].index == 41 && recording.lines[41].synchronized,
        "false pulse displaced the next line");
    require(recording.lines[80].index == 81 && recording.lines[80].synchronized,
        "relocked to a single false pulse after a gap");
}

void test_tx_level(Mode mode) {
    const Image image{};
    Encoder nominal(mode, image), half(mode, image, 50), muted(mode, image, 0);
    std::array<std::int16_t, 240> full{}, scaled{}, silence{};
    bool audible = false;
    for (int block = 0; block < 100; ++block) {
        require(nominal.read(full.data(), full.size()) == full.size() &&
            half.read(scaled.data(), scaled.size()) == scaled.size() &&
            muted.read(silence.data(), silence.size()) == silence.size(),
            "TX level changed the number of generated samples");
        for (std::size_t i = 0; i < full.size(); ++i) {
            audible |= full[i] != 0;
            require(std::abs(2 * int(scaled[i]) - full[i]) <= 1, "TX level did not scale PCM linearly");
            require(silence[i] == 0, "zero TX level produced audio");
        }
    }
    require(audible, "nominal TX level did not produce audio");
}

void test_modes(Mode mode) {
    test_encoder(mode);
    test_tx_level(mode);
    test_reference(mode, true);
    test_reference(mode, false);
    test_weak_signal(mode, false);
    test_weak_signal(mode, true);
    test_reacquisition(mode, true);
    test_reacquisition(mode, false);
    test_afc(mode);
    test_line_clock(mode);
    test_false_sync(mode);
}

}

int main() {
    try {
        auto martin = std::async(std::launch::async, test_modes, Mode::martin_m1);
        test_modes(Mode::scottie_1);
        martin.get();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
