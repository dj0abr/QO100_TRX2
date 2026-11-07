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
int64_t signed_integer(const std::string& s) {
    size_t used=0;const auto v=std::stoll(s,&used);
    if(used!=s.size() || s.empty() || s.find_first_not_of("+-0123456789")!=std::string::npos)
        throw std::runtime_error("Expected signed integer: "+s);
    return v;
}
double real(const std::string& s) {
    size_t used=0;const double v=std::stod(s,&used);
    if(used!=s.size() || !std::isfinite(v)) throw std::runtime_error("Expected finite number: "+s);
    return v;
}
}
void Config::derive() {
    if(lnb_reference_hz<10000000 || lnb_reference_hz>30000000)
        throw std::runtime_error("lnb_reference_hz must be 10000000..30000000");
    if(rx_correction_hz < -1000000 || rx_correction_hz>1000000 || tx_correction_hz < -1000000 || tx_correction_hz>1000000)
        throw std::runtime_error("Frequency corrections must be within +/-1000000 Hz");
    if(rx_center_hz>1000000000000ULL || pluto_tx_base_hz>6000000000ULL)
        throw std::runtime_error("RF reference frequency out of range");
    const int64_t rx=int64_t(rx_center_hz)-int64_t(lnb_reference_hz)*390+rx_correction_hz;
    const int64_t tx=int64_t(pluto_tx_base_hz)+tx_correction_hz;
    if(rx<70000000 || rx>6000000000LL || tx<70000000 || tx>6000000000LL)
        throw std::runtime_error("Calculated Pluto LO outside 70 MHz..6 GHz");
    pluto_rx_hz=uint64_t(rx);pluto_tx_hz=uint64_t(tx);
    pluto_uri=pluto_connection=="usb" ? "usb:" : "ip:"+pluto_ip;
}
void set_config_value(Config& c,const std::string& k,const std::string& v) {
        if (k == "pluto_uri") {
            if(v!="usb:" && v.rfind("ip:",0)!=0) throw std::runtime_error("pluto_uri must be usb: or ip:IPv4");
            c.pluto_uri=v;c.pluto_connection=v=="usb:"?"usb":"network";
            if(v.rfind("ip:",0)==0) c.pluto_ip=v.substr(3);
        }
        else if (k == "pluto_sample_rate") c.pluto_sample_rate = u32(v);
        else if (k == "rx_center_hz") c.rx_center_hz = integer(v);
        else if (k == "pluto_rx_hz") c.pluto_rx_hz = integer(v);
        else if (k == "tx_center_hz") c.tx_center_hz = integer(v);
        else if (k == "pluto_tx_hz" || k == "pluto_tx_base_hz") c.pluto_tx_base_hz = integer(v);
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
        else if (k == "rx3_frequency_hz") c.rx3_frequency_hz = integer(v);
        else if (k == "rx4_frequency_hz") c.rx4_frequency_hz = integer(v);
        else if (k == "rx1_filter_low_hz") c.rx1_filter = RxFilterSettings::from_edges(real(v), c.rx1_filter.high_hz());
        else if (k == "rx1_filter_high_hz") c.rx1_filter = RxFilterSettings::from_edges(c.rx1_filter.low_hz(), real(v));
        else if (k == "rx1_filter_bandwidth_hz") c.rx1_filter.bandwidth_hz = u32(v);
        else if (k == "rx1_filter_center_hz") c.rx1_filter.center_hz = u32(v);
        else if (k == "rx1_nr_percent") c.rx1_nr_percent = u32(v);
        else if (k == "initial_tx_hz") c.initial_tx_hz = integer(v);
        else if (k == "initial_tx2_hz") c.initial_tx2_hz = integer(v);
        else if (k == "tx1_compressor_percent") {
            size_t used=0;const double percent=std::stod(v,&used);
            if (used!=v.size() || !std::isfinite(percent) || percent<0 || percent>140)
                throw std::runtime_error("tx1_compressor_percent must be 0..140");
            c.tx1_compressor.amount=uint32_t(std::lround(percent*10));
        }
        else if (k == "tx_attenuation_db") {
            size_t used=0;c.tx_attenuation_db=std::stod(v,&used);
            if(used!=v.size()) throw std::runtime_error("Invalid tx_attenuation_db");
        }
        else if (k == "subscription_seconds") {
            size_t used = 0;
            c.subscription_seconds = std::stod(v, &used);
            if (used != v.size()) throw std::runtime_error("Invalid subscription_seconds");
        }
        else if (k=="pluto_connection") c.pluto_connection=v;
        else if (k=="pluto_ip") c.pluto_ip=v;
        else if (k=="lnb_reference_hz") c.lnb_reference_hz=integer(v);
        else if (k=="rx_correction_hz") c.rx_correction_hz=signed_integer(v);
        else if (k=="tx_correction_hz") c.tx_correction_hz=signed_integer(v);
        else if (k=="gui_audio_gain") c.gui_audio_gain=real(v);
        else if (k=="gui_display_floor") c.gui_display_floor=real(v);
        else if (k=="gui_wheel_step_hz") c.gui_wheel_step_hz=u32(v);
        else if (k=="gui_nr_strength") c.gui_nr_strength=u32(v);
        else throw std::runtime_error("Unknown config key: " + k);

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
    if(pluto_connection!="network" && pluto_connection!="usb") throw std::runtime_error("pluto_connection must be network or usb");
    if(inet_pton(AF_INET,pluto_ip.c_str(),&addr)!=1) throw std::runtime_error("pluto_ip must be an IPv4 address");
    if(!std::isfinite(gui_audio_gain) || gui_audio_gain<0 || gui_audio_gain>20 ||
       !std::isfinite(gui_display_floor) || gui_display_floor < -160 || gui_display_floor > -40 ||
       gui_wheel_step_hz<1 || gui_wheel_step_hz>1000 || gui_nr_strength<1 || gui_nr_strength>100)
        throw std::runtime_error("GUI settings outside supported range");
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
    for (const auto frequency : {rx3_frequency_hz, rx4_frequency_hz})
        if (frequency < rx_center_hz - 275000 || frequency > rx_center_hz + 275000)
            throw std::runtime_error("Monitoring frequency must remain inside the RX channel window (+/-275000 Hz)");
    if (tx_center_hz < 275000 || initial_tx_hz + 275000 < tx_center_hz || initial_tx_hz > tx_center_hz + 275000)
        throw std::runtime_error("initial_tx_hz must remain inside the TX channel window");
    if(initial_tx2_hz < tx_center_hz-275000 || initial_tx2_hz > tx_center_hz+275000)
        throw std::runtime_error("initial_tx2_hz must remain inside the TX channel window");
    if (!std::isfinite(tx_attenuation_db) || tx_attenuation_db < 0 || tx_attenuation_db > 40 || std::abs(tx_attenuation_db*4-std::round(tx_attenuation_db*4))>1e-6)
        throw std::runtime_error("tx_attenuation_db must be 0..40 in 0.25 dB steps");
    if (!rx1_filter.valid()) throw std::runtime_error("RX1 filter requires BW 500..3900 Hz and edges inside 100..4000 Hz");
    if (rx1_nr_percent>100) throw std::runtime_error("rx1_nr_percent must be 0..100");
    if (!tx1_compressor.valid()) throw std::runtime_error("tx1_compressor_percent must be 0..140");
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
        set_config_value(c,k,v);
    }
    if((seen.count("rx1_filter_low_hz") || seen.count("rx1_filter_high_hz")) &&
       (seen.count("rx1_filter_bandwidth_hz") || seen.count("rx1_filter_center_hz")))
        throw std::runtime_error("Use filter edges or legacy bandwidth/center, not both");
    c.derive();c.validate();
    return c;
}
}
