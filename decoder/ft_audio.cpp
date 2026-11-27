#include "ft_audio.hpp"
#include "../webbridge/wire.hpp"

#include <cstdlib>
#include <limits>

namespace qo100 {
using namespace std::chrono;

std::optional<FtPcm> ft_pcm(std::span<const std::uint8_t> p, unsigned stream)
{
    if (p.size() != 544 || !bridge::valid(p) || p[5] != 5) return {};
    const auto get = [&](std::size_t at, std::size_t width) { return bridge::get(p, at, width); };
    if (get(8, 4) != stream || get(40, 4) != ft8::sample_rate || get(44, 8) ||
        get(52, 2) != 240 || get(54, 2) != 2 || get(56, 4) != 240 || !get(60, 4)) return {};

    const auto index = get(24, 8), frequency = get(32, 8), ns = get(16, 8);
    if (!frequency || frequency > 1'000'000'000'000ULL ||
        index > std::uint64_t(ft8::sample_index(ft8::Time::max())) - 240 ||
        ns < 6'000'000'000ULL || ns > std::uint64_t(std::numeric_limits<std::int64_t>::max()) - 6'000'000'000ULL)
        return {};

    FtPcm block{std::uint32_t(get(60, 4)), std::uint32_t(get(12, 4)), index, frequency, std::int64_t(ns), {}};
    for (std::size_t i = 0; i < block.samples.size(); ++i) {
        const int sample = get(64 + 2 * i, 2);
        block.samples[i] = static_cast<ft8::Sample>(sample >= 32768 ? sample - 65536 : sample);
    }
    return block;
}

std::optional<FtSampleClock::Position> FtSampleClock::push(const FtPcm& p)
{
    const auto end = p.index + p.samples.size();
    const auto first = ft8::sample_index(ft8::Time(nanoseconds(p.packet_ns) - 20ms + offset_));
    if (anchor_ && p.session == anchor_->session) {
        auto& a = *anchor_;
        if (std::int32_t(p.epoch - a.epoch) < 0) return {};
        if (p.epoch == a.epoch) {
            if (p.index < a.next) return {};
            if (p.frequency == a.frequency && p.index - a.next <= max_gap_) {
                const auto expected = a.first + std::int64_t(p.index - a.index);
                if (std::abs(first - expected) <= ft8::sample_rate / 4) {
                    a.next = end;
                    return Position{expected, false};
                }
            }
        }
    }

    const bool discontinuity = anchor_.has_value();
    anchor_ = Anchor{p.session, p.epoch, p.index, end, p.frequency, first};
    return Position{first, discontinuity};
}

} // namespace qo100
