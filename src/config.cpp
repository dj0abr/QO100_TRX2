#include "qo100/config.hpp"
#include <arpa/inet.h>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>

namespace qo100 {
namespace {
std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}
uint64_t integer(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected an unsigned integer: " + s);
    size_t used = 0;
    const auto value = std::stoull(s, &used);
    if (used != s.size()) throw std::runtime_error("Invalid integer: " + s);
    return value;
}
uint32_t u32(const std::string& s) {
    const auto v = integer(s);
    if (v > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("Integer out of range");
    return static_cast<uint32_t>(v);
}
}
void Config::validate() const {
    // The vendored driver is built with RX_DECIMATION=2. Keep this milestone explicit.
    if (pluto_sample_rate != 1200000) throw std::runtime_error("This backend requires pluto_sample_rate=1200000");
    if (fft_size < 1024 || fft_size > 65536 || (fft_size & (fft_size - 1)))
        throw std::runtime_error("fft_size must be a power of two from 1024 to 65536");
    if (spectrum_fps < 1 || spectrum_fps > 60) throw std::runtime_error("spectrum_fps must be 1..60");
    if (gui_port == 0 || gui_port == pluto_tx_port || gui_port == pluto_iq_port || gui_port == pluto_status_port)
        throw std::runtime_error("gui_port must be nonzero and different from the Pluto ports");
    in_addr addr{};
    if (inet_pton(AF_INET, gui_bind.c_str(), &addr) != 1) throw std::runtime_error("gui_bind must be an IPv4 address");
    if (pluto_uri.rfind("ip:", 0) != 0 || inet_pton(AF_INET, pluto_uri.c_str() + 3, &addr) != 1)
        throw std::runtime_error("pluto_uri must have the form ip:192.168.10.25");
    if (pluto_rx_hz < 70000000 || pluto_rx_hz > 6000000000ULL ||
        pluto_tx_hz < 70000000 || pluto_tx_hz > 6000000000ULL)
        throw std::runtime_error("Pluto frequency must be in 70 MHz..6 GHz");
    if (rx_center_hz < iq_rate() / 2 || rx_center_hz > 1000000000000ULL ||
        tx_center_hz > 1000000000000ULL)
        throw std::runtime_error("RF reference frequency out of range");
    if (rx_bandwidth_hz < 200000 || rx_bandwidth_hz > pluto_sample_rate)
        throw std::runtime_error("rx_bandwidth_hz must be 200000..1200000");
    if (initial_rx_hz + 275000 < rx_center_hz || initial_rx_hz > rx_center_hz + 275000)
        throw std::runtime_error("initial_rx_hz must leave room for the +/-25 kHz zoom");
    if (initial_rx2_hz + 275000 < rx_center_hz || initial_rx2_hz > rx_center_hz + 275000)
        throw std::runtime_error("initial_rx2_hz must remain inside the RX channel window");
    if (tx_center_hz < 275000 || initial_tx_hz + 275000 < tx_center_hz || initial_tx_hz > tx_center_hz + 275000)
        throw std::runtime_error("initial_tx_hz must remain inside the TX channel window");
    if (!std::isfinite(tx_attenuation_db) || tx_attenuation_db < 0 || tx_attenuation_db > 89.75)
        throw std::runtime_error("tx_attenuation_db must be 0..89.75");
    if (!std::isfinite(subscription_seconds) || subscription_seconds < 2 || subscription_seconds > 60)
        throw std::runtime_error("subscription_seconds must be 2..60");
}
Config load_config(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot read config: " + path.string());
    Config c;
    std::map<std::string, bool> seen;
    std::string line;
    unsigned line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) throw std::runtime_error("Missing '=' at config line " + std::to_string(line_no));
        const auto k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (v.empty() || seen[k]) throw std::runtime_error("Empty or duplicate config key: " + k);
        seen[k] = true;
        if (k == "pluto_uri") c.pluto_uri = v;
        else if (k == "pluto_sample_rate") c.pluto_sample_rate = u32(v);
        else if (k == "rx_center_hz") c.rx_center_hz = integer(v);
        else if (k == "pluto_rx_hz") c.pluto_rx_hz = integer(v);
        else if (k == "tx_center_hz") c.tx_center_hz = integer(v);
        else if (k == "pluto_tx_hz") c.pluto_tx_hz = integer(v);
        else if (k == "rx_bandwidth_hz") c.rx_bandwidth_hz = u32(v);
        else if (k == "gui_bind") c.gui_bind = v;
        else if (k == "gui_port") {
            auto p = integer(v);
            if (p > 65535) throw std::runtime_error("gui_port out of range");
            c.gui_port = static_cast<uint16_t>(p);
        }
        else if (k == "fft_size") c.fft_size = u32(v);
        else if (k == "spectrum_fps") c.spectrum_fps = u32(v);
        else if (k == "initial_rx_hz") c.initial_rx_hz = integer(v);
        else if (k == "initial_rx2_hz") c.initial_rx2_hz = integer(v);
        else if (k == "initial_tx_hz") c.initial_tx_hz = integer(v);
        else if (k == "tx_attenuation_db") {
            size_t used=0;c.tx_attenuation_db=std::stod(v,&used);
            if(used!=v.size()) throw std::runtime_error("Invalid tx_attenuation_db");
        }
        else if (k == "subscription_seconds") {
            size_t used = 0;
            c.subscription_seconds = std::stod(v, &used);
            if (used != v.size()) throw std::runtime_error("Invalid subscription_seconds");
        }
        else throw std::runtime_error("Unknown config key: " + k);
    }
    c.validate();
    return c;
}
}
