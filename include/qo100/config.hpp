#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

namespace qo100 {
constexpr uint16_t pluto_tx_port = 40808;
constexpr uint16_t pluto_iq_port = 40809;
constexpr uint16_t pluto_status_port = 40810;
struct Config {
    std::string pluto_uri = "ip:192.168.10.25";
    uint32_t pluto_sample_rate = 1200000;
    uint64_t rx_center_hz = 10489750000ULL;
    uint64_t pluto_rx_hz = 739750000;
    uint64_t tx_center_hz = 2400250000ULL;
    uint64_t pluto_tx_hz = 1290250000;
    uint32_t rx_bandwidth_hz = 600000;
    std::string gui_bind = "0.0.0.0";
    uint16_t gui_port = 40900;
    uint32_t fft_size = 8192;
    uint32_t spectrum_fps = 20;
    uint64_t initial_rx_hz = 10489748500ULL;
    uint64_t initial_rx2_hz = 10489600000ULL;
    double subscription_seconds = 8;
    uint32_t iq_rate() const { return pluto_sample_rate / 2; }
    void validate() const;
};
Config load_config(const std::filesystem::path& path);
}
