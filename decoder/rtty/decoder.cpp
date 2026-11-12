#include "rtty.hpp"
#include "ita2.hpp"

#include <array>
#include <cmath>
#include <complex>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace qo100::rtty {
using namespace detail;

namespace {
using Complex = std::complex<double>;

// A half-bit sliding correlation resolves the two tones while leaving
// room to sample near the middle of a bit after detecting the start edge.
constexpr unsigned window = bit_samples / 2;
constexpr unsigned oscillator_period = sample_rate / std::gcd(sample_rate, std::gcd(mark_hz, space_hz));

struct ToneDetector {
    struct Oscillators {
        Complex mark, space;
    };
    struct Sample {
        Complex mark{}, space{};
        double power = 0;
    };
    enum class Tone { unknown, mark, space };

    static const std::array<Oscillators, oscillator_period> oscillators;
    std::array<Sample, window> history{};
    Sample sum{};
    unsigned phase = 0, index = 0;

    Tone push(double sample) {
        const auto& oscillator = oscillators[phase];
        const Sample current{sample * oscillator.mark, sample * oscillator.space, sample * sample};
        auto& old = history[index];
        sum.mark += current.mark - old.mark;
        sum.space += current.space - old.space;
        sum.power += current.power - old.power;
        old = current;
        phase = (phase + 1) % oscillator_period;
        index = (index + 1) % window;

        // Require both an absolute signal floor and coherent energy above the
        // broadband noise floor. Leave ambiguous transitions undecided.
        if (sum.power < window * 1e-8) return Tone::unknown;
        const double mark = std::norm(sum.mark);
        const double space = std::norm(sum.space);
        const double threshold = sum.power * window * 0.07;
        if (mark > threshold && mark > 1.4 * space) return Tone::mark;
        if (space > threshold && space > 1.4 * mark) return Tone::space;
        return Tone::unknown;
    }
};

const std::array<ToneDetector::Oscillators, oscillator_period> ToneDetector::oscillators = [] {
    std::array<ToneDetector::Oscillators, oscillator_period> result{};
    for (unsigned i = 0; i < result.size(); ++i) {
        result[i] = {std::polar(1.0, 2 * pi * mark_hz * i / sample_rate),
                     std::polar(1.0, 2 * pi * space_hz * i / sample_rate)};
    }
    return result;
}();
}

struct Decoder::Impl {
    using Tone = ToneDetector::Tone;

    struct State {
        ToneDetector detector;
        unsigned mark_run = 0;
        unsigned since_mark = 0;
        unsigned until_sample = 0;
        unsigned bit = 0;
        unsigned code = 0;
        bool shifted = false;
    };

    Character character;
    Options options;
    State state{};

    Impl(Character callback, Options settings) : character(std::move(callback)), options(settings) {
        if (!character) throw std::invalid_argument("RTTY RX requires a character callback");
    }

    void emit(unsigned code) {
        if (code == letters_shift || code == figures_shift) {
            state.shifted = code == figures_shift;
            return;
        }
        if (code == space && options.unshift_on_space) state.shifted = false;
        const char c = (state.shifted ? figures : letters)[code];
        if (c) character(c);
    }

    void push(std::int16_t sample) {
        auto tone = state.detector.push(sample / 32768.0);
        if (options.reverse && tone != Tone::unknown)
            tone = tone == Tone::mark ? Tone::space : Tone::mark;

        if (!state.until_sample) {
            if (tone == Tone::mark) {
                if (state.mark_run < bit_samples / 4) ++state.mark_run;
                state.since_mark = 0;
            } else if (tone == Tone::space) {
                if (state.mark_run >= bit_samples / 4) {
                    state.until_sample = bit_samples / 2;
                    state.bit = 0;
                    state.code = 0;
                }
                state.mark_run = 0;
            } else if (state.mark_run && ++state.since_mark > window) {
                state.mark_run = 0;
            }
            return;
        }

        if (--state.until_sample) return;
        if (state.bit == 0) {
            if (tone != Tone::space) return; // False start.
        } else if (state.bit <= data_bits) {
            if (tone == Tone::unknown) return;
            if (tone == Tone::mark) state.code |= 1u << (state.bit - 1);
        } else {
            if (tone == Tone::mark) emit(state.code);
            return;
        }
        ++state.bit;
        state.until_sample = bit_samples;
    }
};

Decoder::Decoder(Character character) : Decoder(std::move(character), Options{}) {}
Decoder::Decoder(Character character, Options options)
    : impl_(std::make_unique<Impl>(std::move(character), options)) {}
Decoder::~Decoder() = default;
void Decoder::push(const std::int16_t* pcm, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) impl_->push(pcm[i]);
}
void Decoder::reset() {
    impl_->state = {};
}

}
