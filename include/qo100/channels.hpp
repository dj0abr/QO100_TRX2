#pragma once
#include <array>
#include <cstdint>

namespace qo100 {
// Logical RX numbers are distinct from wire IDs: 3 is TX1, 4 is audio-loop playback.
inline constexpr uint32_t rx3_stream = 5, rx4_stream = 6;
inline constexpr uint32_t tx2_stream = 7;
inline constexpr std::array<uint32_t, 4> rx_streams{1, 2, rx3_stream, rx4_stream};
inline constexpr uint32_t receiver_number(uint32_t stream) {
    return stream == rx3_stream ? 3 : stream == rx4_stream ? 4 : stream;
}
}
