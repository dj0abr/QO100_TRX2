#pragma once

#include <libft8/libft8.hpp>
#include <array>
#include <optional>

namespace qo100 {

struct FtPcm {
    std::uint32_t session, epoch;
    std::uint64_t index, frequency;
    std::int64_t packet_ns;
    std::array<ft8::Sample, 240> samples;
};

std::optional<FtPcm> ft_pcm(std::span<const std::uint8_t> packet, unsigned stream);

// The QTR2 timestamp estimates the end of a block. Follow the sample index
// after anchoring, so subsequent packet jitter cannot stretch FT symbols.
class FtSampleClock {
public:
    struct Position { std::int64_t first; bool discontinuity; };

    explicit FtSampleClock(std::chrono::nanoseconds offset = {}, unsigned max_gap = 0)
        : offset_(offset), max_gap_(max_gap) {}
    std::optional<Position> push(const FtPcm& block);
    void reset() { anchor_.reset(); }

private:
    std::chrono::nanoseconds offset_;
    unsigned max_gap_;
    struct Anchor {
        std::uint32_t session, epoch;
        std::uint64_t index, next, frequency;
        std::int64_t first;
    };
    std::optional<Anchor> anchor_;
};

} // namespace qo100
