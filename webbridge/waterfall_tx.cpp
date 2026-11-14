#include "bridge.hpp"
#include "waterfall_font.hpp"

#include <algorithm>
#include <cmath>

namespace bridge {
namespace {
namespace audio = qo100::audio;
constexpr auto pi = 3.14159265358979323846;
constexpr unsigned bands = 9;
constexpr unsigned columns = 7;
constexpr unsigned first_tone_hz = 525;
constexpr unsigned tone_step_hz = 25;
constexpr unsigned tones_per_band = 7;
constexpr unsigned band_step_bins = 9;
constexpr unsigned period_samples = audio::sample_rate / tone_step_hz;
constexpr unsigned ramp_samples = audio::sample_rate / 100;
constexpr std::size_t text_limit = 24;
static_assert(audio::sample_rate % tone_step_hz == 0);

std::vector<unsigned> rasterize(std::string_view message) {
    using Glyph = std::array<unsigned, bands>;
    static const auto font = Json::parse(waterfall_font_json).get<std::map<std::string, Glyph>>();
    constexpr auto allowed = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 /-.?+_";
    if (message.size() > text_limit || message.find_first_not_of(allowed) != std::string_view::npos)
        throw std::runtime_error("Waterfall text: use up to 24 letters A–Z, digits, spaces or /-.?+_");
    const auto first = message.find_first_not_of(' ');
    if (first == std::string_view::npos) throw std::runtime_error("Waterfall text is empty");
    message = message.substr(first, message.find_last_not_of(' ') - first + 1);

    std::vector<unsigned> raster;
    for (char c : message) {
        if (!raster.empty()) raster.push_back(0);
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        const auto& glyph = font.at(std::string(1, c));
        // Left-to-right columns become successive rows of tones. The top of
        // the glyph is at the low-frequency end of each row.
        for (unsigned col = 0; col < columns; ++col) {
            unsigned mask = 0;
            for (unsigned band = 0; band < bands; ++band)
                if (glyph[band] & (1u << (columns - 1 - col))) mask |= 1u << band;
            raster.push_back(mask);
        }
    }
    return raster;
}

using TonePeriod = std::array<double, period_samples>;

std::array<TonePeriod, bands> make_band_periods() {
    std::array<TonePeriod, bands> periods{};
    constexpr unsigned tone_count = (bands - 1) * band_step_bins + tones_per_band;
    for (unsigned band = 0; band < bands; ++band) {
        for (unsigned tone = 0; tone < tones_per_band; ++tone) {
            const auto bin = band * band_step_bins + tone;
            const auto hz = first_tone_hz + bin * tone_step_hz;
            // Spread the starting phases of the comb to reduce aligned peaks.
            const auto phase = pi * bin * (double(bin) - 1) / tone_count;
            for (unsigned sample = 0; sample < period_samples; ++sample)
                periods[band][sample] += std::cos(2 * pi * hz * sample / audio::sample_rate + phase);
        }
    }
    return periods;
}

std::vector<int16_t> render_pcm(const std::vector<unsigned>& raster, unsigned row_ms) {
    static const auto band_periods = make_band_periods();
    std::map<unsigned, TonePeriod> periods;
    double peak = 1;
    for (const auto mask : raster) {
        auto [it, inserted] = periods.try_emplace(mask);
        if (!inserted) continue;
        auto& period = it->second;
        for (unsigned band = 0; band < bands; ++band) {
            if (!(mask & (1u << band))) continue;
            for (unsigned sample = 0; sample < period_samples; ++sample)
                period[sample] += band_periods[band][sample];
        }
        for (const auto sample : period) peak = std::max(peak, std::abs(sample));
    }

    // A common gain preserves the relative brightness of strokes across the text.
    const auto gain = 24576 / peak;
    const auto row_samples = row_ms * audio::sample_rate / 1000;
    std::vector<int16_t> pcm;
    pcm.reserve(raster.size() * row_samples);
    for (const auto mask : raster) {
        const auto& period = periods.at(mask);
        for (unsigned i = 0; i < row_samples; ++i) {
            const auto edge = std::min(i, row_samples - 1 - i);
            const double envelope = edge < ramp_samples ? .5 - .5 * std::cos(pi * edge / ramp_samples) : 1.;
            pcm.push_back(static_cast<int16_t>(std::lround(gain * envelope * period[i % period_samples])));
        }
    }
    return pcm;
}
}

void Bridge::begin_waterfall_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level,
                                const std::string& message, unsigned row_ms) {
    if (!can_start_tx2(client, id, frequency, level)) return;
    if (row_ms < 100 || row_ms > 400 || row_ms % 20)
        throw std::runtime_error("Raster row duration must be 100–400 ms in 20 ms steps");

    auto pcm = render_pcm(rasterize(message), row_ms);
    start_tx2(Tx2Mode::waterfall, WaterfallTx{std::move(pcm), id}, frequency, level);
    text(client, {{"type", "ack"}, {"command", "waterfall_tx_start"}, {"id", id}, {"status", 0}});
}
}
