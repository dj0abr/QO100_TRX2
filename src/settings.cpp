#include "qo100/settings.hpp"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

namespace qo100 {
namespace {
template<class T> std::string value(T n) { std::ostringstream s;s<<std::setprecision(12)<<n;return s.str(); }
}
SettingsMap settings_values(const Config& c) {
    return {{"pluto_connection",c.pluto_connection},{"pluto_ip",c.pluto_ip},
        {"lnb_reference_hz",value(c.lnb_reference_hz)},{"rx_correction_hz",value(c.rx_correction_hz)},
        {"tx_correction_hz",value(c.tx_correction_hz)},{"pluto_tx_base_hz",value(c.pluto_tx_base_hz)},
        {"tx_attenuation_db",value(c.tx_attenuation_db)},
        {"initial_rx_hz",value(c.initial_rx_hz)},{"initial_rx2_hz",value(c.initial_rx2_hz)},
        {"rx3_frequency_hz",value(c.rx3_frequency_hz)},{"rx4_frequency_hz",value(c.rx4_frequency_hz)},
        {"rx5_frequency_hz",value(c.rx5_frequency_hz)},
        {"initial_tx_hz",value(c.initial_tx_hz)},
        {"initial_tx2_hz",value(c.initial_tx2_hz)},
        {"rx1_filter_low_hz",value(c.rx1_filter.low_hz())},
        {"rx1_filter_high_hz",value(c.rx1_filter.high_hz())},
        {"rx1_nr_percent",value(c.rx1_nr_percent)},
        {"tx1_compressor_percent",value(c.tx1_compressor.amount/10.)},
        {"gui_audio_gain",value(c.gui_audio_gain)},{"gui_display_floor",value(c.gui_display_floor)},
        {"gui_wheel_step_hz",value(c.gui_wheel_step_hz)},{"gui_nr_strength",value(c.gui_nr_strength)}};
}
SettingsMap parse_settings(const std::string& text) {
    if(text.size()>1136) throw std::runtime_error("Settings payload too large");
    SettingsMap result;
    std::istringstream input(text);std::string line;
    while(std::getline(input,line)) {
        if(line.empty()) continue;
        if(line.find_first_of("\r\0",0,2)!=std::string::npos) throw std::runtime_error("Invalid settings text");
        const auto eq=line.find('=');
        if(eq==std::string::npos || !eq || eq>64 || line.size()-eq>200) throw std::runtime_error("Expected key=value");
        const auto key=line.substr(0,eq),v=line.substr(eq+1);
        if(key.find_first_not_of("abcdefghijklmnopqrstuvwxyz_0123456789")!=std::string::npos || v.empty() ||
           v.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .:+-_")!=std::string::npos ||
           !result.emplace(key,v).second) throw std::runtime_error("Invalid or duplicate settings key: "+key);
    }
    return result;
}
std::string encode_settings(const SettingsMap& values) {
    std::string out;for(const auto& [k,v]:values) out+=k+"="+v+"\n";return out;
}
Config patch_settings(const Config& old,const SettingsMap& patch) {
    Config next=old;const auto supported=settings_values(old);
    const bool legacy=patch.count("rx1_filter_bandwidth_hz") || patch.count("rx1_filter_center_hz");
    if(legacy && (patch.count("rx1_filter_low_hz") || patch.count("rx1_filter_high_hz")))
        throw std::runtime_error("Use filter edges or legacy bandwidth/center, not both");
    for(const auto& [k,v]:patch) {
        if(!supported.count(k) && k!="rx1_filter_bandwidth_hz" && k!="rx1_filter_center_hz") throw std::runtime_error("Unknown or read-only setting: "+k);
        set_config_value(next,k,v);
    }
    if(next.rx1_nr_percent) next.gui_nr_strength=next.rx1_nr_percent;
    next.derive();next.validate();return next;
}
bool hardware_settings_changed(const Config& a,const Config& b) {
    return a.pluto_uri!=b.pluto_uri || a.pluto_rx_hz!=b.pluto_rx_hz ||
        a.pluto_tx_hz!=b.pluto_tx_hz || a.tx_attenuation_db!=b.tx_attenuation_db;
}
SettingsStore::SettingsStore(Config defaults,std::filesystem::path path):config_(std::move(defaults)),path_(std::move(path)) {
    if(std::filesystem::exists(path_)) {
        if(std::filesystem::file_size(path_)>1136) throw std::runtime_error("Saved settings file too large");
        std::ifstream input(path_);if(!input) throw std::runtime_error("Cannot read saved settings");
        std::ostringstream text;text<<input.rdbuf();
        config_=patch_settings(config_,parse_settings(text.str()));
    }
}
void SettingsStore::changed() {
    ++revision_;
    if(!dirty_) deadline_=std::chrono::steady_clock::now()+std::chrono::milliseconds(500);
    dirty_=true;
}
void SettingsStore::write(const Config& next) {
    const auto text=encode_settings(settings_values(next));
    std::string pattern=path_.string()+".tmp-XXXXXX";
    std::vector<char> name(pattern.begin(),pattern.end());name.push_back(0);
    const int fd=mkstemp(name.data());
    const std::filesystem::path temp=name.data();
    if(fd<0) throw std::runtime_error("Cannot create settings file: "+std::string(std::strerror(errno)));
    (void)fcntl(fd,F_SETFD,FD_CLOEXEC);
    bool closed=false;
    try {
        size_t used=0;
        while(used<text.size()) {
            const auto n=::write(fd,text.data()+used,text.size()-used);
            if(n<0 && errno==EINTR) continue;
            if(n<=0) throw std::runtime_error("Cannot write settings");
            used+=size_t(n);
        }
        if(fsync(fd)<0) throw std::runtime_error("Cannot sync settings");
        closed=true;if(close(fd)<0) throw std::runtime_error("Cannot close settings");
        if(rename(temp.c_str(),path_.c_str())<0) throw std::runtime_error("Cannot replace settings file");
        const auto directory=path_.parent_path().empty()?std::filesystem::path("."):path_.parent_path();
        const int dir=open(directory.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(dir>=0) { (void)fsync(dir);close(dir); }
    } catch(...) {if(!closed) close(fd);unlink(temp.c_str());throw;}
}
void SettingsStore::commit(const Config& next) {
    if(!dirty_ && settings_values(config_)==settings_values(next)) return;
    write(next);config_=next;++revision_;dirty_=false;error_.clear();
}
void SettingsStore::flush() {
    if(!dirty_) return;
    try {write(config_);dirty_=false;error_.clear();}
    catch(const std::exception& e) {error_=e.what();deadline_=std::chrono::steady_clock::now()+std::chrono::seconds(2);throw;}
}
void SettingsStore::flush_due() {
    if(dirty_ && std::chrono::steady_clock::now()>=deadline_) {
        try {flush();} catch(const std::exception&) {} // Keep RX running; report in SETTINGS_STATE.
    }
}
}
