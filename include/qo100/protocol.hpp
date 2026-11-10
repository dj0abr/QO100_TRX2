#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>

namespace qo100 {
constexpr size_t header_size = 64, max_datagram = 1200;
constexpr uint8_t protocol_version = 1, subscribe_type = 1, spectrum_type = 2;
constexpr uint8_t tune_type = 3, state_type = 4, audio_type = 5;
constexpr uint8_t tx_control_type = 6, tx_state_type = 7, tx_audio_type = 8;
constexpr uint8_t filter_control_type = 9, filter_state_type = 10;
constexpr uint8_t compressor_control_type = 11, compressor_state_type = 12;
constexpr uint8_t loop_control_type = 13, loop_state_type = 14;
constexpr uint8_t nr_control_type = 15, nr_state_type = 16;
constexpr uint8_t settings_control_type = 17, settings_state_type = 18;
// Leased RX2/TX2 demand from the current GUI subscriber. Flags: RX2=1, TX2=2.
struct DigitalActivity {uint32_t request,session,flags;};
bool parse_digital_activity(const uint8_t*,size_t,DigitalActivity&);
struct SettingsControl { uint32_t request,session; uint64_t revision; bool set; std::string payload; };
bool parse_settings_control(const uint8_t*,size_t,SettingsControl&);
std::vector<uint8_t> settings_state_packet(uint32_t request,uint32_t session,uint64_t revision,
    uint16_t status,uint32_t flags,const std::string& payload);
struct NrControl { uint32_t request, session, percent; bool set; };
bool parse_nr_control(const uint8_t*, size_t, NrControl&);
std::vector<uint8_t> nr_state_packet(uint32_t request, uint32_t percent, uint32_t epoch,
    uint32_t session, uint16_t status);
enum class TestLoop : uint32_t { normal=0, audio=1, rf=2 };
struct LoopControl { uint32_t request, session, mode; bool set; };
bool parse_loop_control(const uint8_t*, size_t, LoopControl&);
std::vector<uint8_t> loop_state_packet(uint32_t request, TestLoop mode, TestLoop target,
    uint64_t rx_lo, uint32_t generation, uint32_t session, uint16_t status, uint64_t timestamp_ns=0);
struct CompressorControl { uint32_t request, session, amount; bool set; };
bool parse_compressor_control(const uint8_t*, size_t, CompressorControl&);
std::vector<uint8_t> compressor_state_packet(uint32_t request, uint32_t amount, uint32_t epoch,
    uint32_t session, uint16_t status, uint32_t compression_millidb, uint32_t limiting_millidb);
struct FilterControl {
    uint32_t request, session;
    double bandwidth_hz, center_hz;
    bool set, edges;
};
bool parse_filter_control(const uint8_t*, size_t, FilterControl&);
std::vector<uint8_t> filter_state_packet(uint32_t request, double bandwidth, double center,
    uint32_t epoch, uint32_t session, uint16_t status, bool edges=false);
struct TxControl { uint32_t request, session; uint64_t frequency; bool enabled; uint32_t stream=3; };
struct TxAudio { uint32_t epoch, session; uint64_t index, frequency; std::vector<int16_t> samples; uint32_t stream=3; };
bool parse_tx_control(const uint8_t*, size_t, TxControl&);
bool parse_tx_audio(const uint8_t*, size_t, TxAudio&);
std::vector<uint8_t> tx_state_packet(uint32_t request, uint64_t frequency, uint32_t epoch,
    uint32_t session, uint16_t status, bool enabled, uint32_t audio_fill, uint16_t pluto_fill, int32_t ppm, uint32_t stream=3);
struct FrameInfo {
    uint32_t frame_id;
    uint64_t timestamp_ns, sample_index, center_hz;
    uint32_t sample_rate, fft_size;
    uint32_t session_id = 0;
    uint32_t stream_id = 0;
};
bool is_subscription(const uint8_t* data, size_t size);
bool parse_tune(const uint8_t* data, size_t size, uint32_t& stream_id, uint32_t& request, uint64_t& frequency, uint32_t* session=nullptr);
std::vector<uint8_t> state_packet(uint32_t request, uint64_t frequency, uint32_t epoch, uint32_t session, uint16_t status, uint32_t stream_id = 1);
std::vector<uint8_t> audio_packet(const FrameInfo& info, const std::vector<int16_t>& audio);
std::vector<std::vector<uint8_t>> spectrum_packets(const FrameInfo& info, const std::vector<int16_t>& bins);
}
