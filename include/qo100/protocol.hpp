#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace qo100 {
constexpr size_t header_size = 64, max_datagram = 1200;
constexpr uint8_t protocol_version = 1, subscribe_type = 1, spectrum_type = 2;
constexpr uint8_t tune_type = 3, state_type = 4, audio_type = 5;
constexpr uint8_t tx_control_type = 6, tx_state_type = 7, tx_audio_type = 8;
struct TxControl { uint32_t request, session; uint64_t frequency; bool enabled; };
struct TxAudio { uint32_t epoch, session; uint64_t index, frequency; std::vector<int16_t> samples; };
bool parse_tx_control(const uint8_t*, size_t, TxControl&);
bool parse_tx_audio(const uint8_t*, size_t, TxAudio&);
std::vector<uint8_t> tx_state_packet(uint32_t request, uint64_t frequency, uint32_t epoch,
    uint32_t session, uint16_t status, bool enabled, uint32_t audio_fill, uint16_t pluto_fill, int32_t ppm);
struct FrameInfo {
    uint32_t frame_id;
    uint64_t timestamp_ns, sample_index, center_hz;
    uint32_t sample_rate, fft_size;
    uint32_t session_id = 0;
    uint32_t stream_id = 0;
};
bool is_subscription(const uint8_t* data, size_t size);
bool parse_tune(const uint8_t* data, size_t size, uint32_t& stream_id, uint32_t& request, uint64_t& frequency);
std::vector<uint8_t> state_packet(uint32_t request, uint64_t frequency, uint32_t epoch, uint32_t session, uint16_t status, uint32_t stream_id = 1);
std::vector<uint8_t> audio_packet(const FrameInfo& info, const std::vector<int16_t>& audio);
std::vector<std::vector<uint8_t>> spectrum_packets(const FrameInfo& info, const std::vector<int16_t>& bins);
}
