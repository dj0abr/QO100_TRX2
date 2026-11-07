#pragma once
#include "qo100/filter_settings.hpp"
#include "qo100/compressor_settings.hpp"
#include <cstdint>
#include <filesystem>
#include <string>

namespace qo100 {
constexpr uint16_t pluto_tx_port = 40808;
constexpr uint16_t pluto_iq_port = 40809;
constexpr uint16_t pluto_status_port = 40810;
struct Config {
    std::string pluto_connection = "network", pluto_ip = "192.168.10.25";
    std::string pluto_uri = "ip:192.168.10.25";
    uint32_t pluto_sample_rate = 1200000;
    uint64_t rx_center_hz = 10489750000ULL;
    uint64_t pluto_rx_hz = 739750000;
    uint64_t tx_center_hz = 2400250000ULL;
    uint64_t pluto_tx_hz = 1290250000;
    uint64_t pluto_tx_base_hz = 1290250000;
    uint64_t lnb_reference_hz = 25000000;
    int64_t rx_correction_hz = 0, tx_correction_hz = 0;
    uint32_t rx_bandwidth_hz = 600000;
    std::string gui_bind = "0.0.0.0";
    uint16_t gui_port = 40900;
    uint32_t fft_size = 8192;
    uint32_t spectrum_fps = 20;
    uint64_t initial_rx_hz = 10489900000ULL;
    uint64_t initial_rx2_hz = 10489600000ULL;
    uint64_t rx3_frequency_hz = 10489540000ULL;
    uint64_t rx4_frequency_hz = 10489748821ULL;
    uint64_t initial_tx_hz = 2400400000ULL;
    uint64_t initial_tx2_hz = 2400100000ULL;
    double tx_attenuation_db = 30;
    RxFilterSettings rx1_filter;
    uint32_t rx1_nr_percent = 0;
    CompressorSettings tx1_compressor{1000};
    double subscription_seconds = 8;
    double gui_audio_gain = 4, gui_display_floor = -100;
    uint32_t gui_wheel_step_hz = 10, gui_nr_strength = 60;
    uint32_t iq_rate() const { return pluto_sample_rate / 2; }
    void validate() const;
    void derive();
};
void set_config_value(Config&,const std::string& key,const std::string& value);
Config load_config(const std::filesystem::path& path);
}
