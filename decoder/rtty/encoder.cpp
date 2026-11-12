#include "rtty.hpp"
#include "ita2.hpp"

#include <cmath>
#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

namespace qo100::rtty {
using namespace detail;

std::string normalize_text(std::string_view text) {
    constexpr std::pair<std::string_view, std::string_view> replacements[]{
        {u8"Ä", "AE"}, {u8"ä", "AE"}, {u8"Ö", "OE"}, {u8"ö", "OE"},
        {u8"Ü", "UE"}, {u8"ü", "UE"}, {u8"ß", "SS"}, {u8"ẞ", "SS"}
    };

    std::string result;
    while (!text.empty()) {
        char c = text.front();
        text.remove_prefix(1);
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (c == '\\' && !text.empty() && (text.front() == '\r' || text.front() == '\n'))
            continue; // Accept a pasted Markdown hard line break.
        if (c == '\r') {
            if (!text.empty() && text.front() == '\n') text.remove_prefix(1);
            c = '\n';
        }
        if (c != '\0' && c != '\a' &&
            (letters_text.find(c) != std::string_view::npos || figures_text.find(c) != std::string_view::npos)) {
            result += c;
            continue;
        }
        bool replaced = false;
        for (const auto& [from, to] : replacements) {
            if (c == from.front() && text.substr(0, from.size() - 1) == from.substr(1)) {
                result += to;
                text.remove_prefix(from.size() - 1);
                replaced = true;
                break;
            }
        }
        if (!replaced) throw std::invalid_argument("Unsupported RTTY character");
    }
    return result;
}

namespace {
std::vector<std::uint8_t> encode_text(std::string_view text, bool& shifted) {
    std::vector<std::uint8_t> codes;
    for (const char c : text) {
        if (c == '\n') {
            codes.push_back(8); // CR
            codes.push_back(2); // LF
        } else if (c == ' ') {
            codes.push_back(space);
            // Keep receivers with either unshift-on-space setting in sync.
            if (shifted) codes.push_back(letters_shift);
            shifted = false;
        } else {
            const auto letter = letters_text.find(c);
            const bool use_figures = letter == std::string_view::npos;
            if (shifted != use_figures) {
                codes.push_back(use_figures ? figures_shift : letters_shift);
                shifted = use_figures;
            }
            codes.push_back(static_cast<std::uint8_t>(use_figures ? figures_text.find(c) : letter));
        }
    }
    return codes;
}
}

struct Encoder::Impl {
    std::deque<std::uint8_t> codes;
    const double amplitude;
    const bool continuous;
    bool shifted = false, started = false, idle_frame = false;
    unsigned position = 0;
    double phase = 0;
    unsigned leader = 12 * bit_samples;
    unsigned trailer = 3 * bit_samples;

    Impl(unsigned level_percent, bool live)
        : amplitude(24576.0 * level_percent / 100), continuous(live) {
        if (level_percent > 100) throw std::invalid_argument("RTTY TX level must be between 0 and 100");
    }

    void load(std::string_view text) {
        const auto normalized = normalize_text(text);
        if (normalized.find_first_not_of(" \n") == std::string::npos)
            throw std::invalid_argument("RTTY TX text is empty");
        const auto frames = encode_text(normalized, shifted);
        codes = {letters_shift, letters_shift};
        codes.insert(codes.end(), frames.begin(), frames.end());
    }

    std::optional<std::string> enqueue(std::string_view text, std::size_t frame_limit) {
        if (!continuous) throw std::logic_error("Only continuous RTTY TX accepts queued text");
        auto normalized = normalize_text(text);
        if (normalized.empty()) return normalized;
        bool next_shift = shifted;
        const auto frames = encode_text(normalized, next_shift);
        if (codes.size() + frames.size() + (started ? 0 : 1) > frame_limit) return std::nullopt;
        if (!started) codes.push_back(letters_shift);
        codes.insert(codes.end(), frames.begin(), frames.end());
        shifted = next_shift;
        started = true;
        return normalized;
    }

    std::size_t read(std::int16_t* pcm, std::size_t capacity) {
        constexpr double mark_step = 2 * pi * mark_hz / sample_rate;
        constexpr double space_step = 2 * pi * space_hz / sample_rate;
        std::size_t written = 0;
        while (written < capacity) {
            bool mark = true;
            if (leader) {
                --leader;
            } else if (!codes.empty() || continuous) {
                // Once a diddle starts, finish that frame before taking queued text.
                if (position == 0) {
                    idle_frame = codes.empty();
                    if (idle_frame) {
                        shifted = false;
                        started = true;
                    }
                }
                const auto value = idle_frame ? letters_shift : codes.front();
                if (position < start_and_data_samples) {
                    const unsigned bit = position / bit_samples;
                    mark = bit != 0 && ((value >> (bit - 1)) & 1);
                }
                if (++position == frame_samples) {
                    position = 0;
                    if (!idle_frame) codes.pop_front();
                }
            } else {
                if (!trailer) break;
                --trailer;
            }
            pcm[written++] = static_cast<std::int16_t>(std::lround(amplitude * std::sin(phase)));
            phase += mark ? mark_step : space_step;
            if (phase >= 2 * pi) phase -= 2 * pi;
        }
        return written;
    }
};

Encoder::Encoder() : impl_(std::make_unique<Impl>(100, true)) {}
Encoder::Encoder(std::string_view text, unsigned level_percent)
    : impl_(std::make_unique<Impl>(level_percent, false)) {
    impl_->load(text);
}
Encoder::~Encoder() = default;
std::optional<std::string> Encoder::enqueue(std::string_view text, std::size_t frame_limit) {
    return impl_->enqueue(text, frame_limit);
}
std::size_t Encoder::read(std::int16_t* pcm, std::size_t capacity) {
    return impl_->read(pcm, capacity);
}

}
