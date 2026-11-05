#pragma once
#include "qo100/config.hpp"
#include <chrono>
#include <map>

namespace qo100 {
using SettingsMap=std::map<std::string,std::string>;
SettingsMap parse_settings(const std::string& text);
std::string encode_settings(const SettingsMap& values);
SettingsMap settings_values(const Config&);
Config patch_settings(const Config&,const SettingsMap&);
bool hardware_settings_changed(const Config&,const Config&);

class SettingsStore {
public:
    SettingsStore(Config defaults,std::filesystem::path path);
    Config& config() { return config_; }
    uint64_t revision() const { return revision_; }
    bool dirty() const { return dirty_; }
    const std::string& error() const { return error_; }
    void changed(); // Existing audio/tuning controls: coalesce disk writes.
    void commit(const Config& next); // Settings transaction: durable before ACK.
    void flush();
    void flush_due();
private:
    void write(const Config&);
    Config config_;
    std::filesystem::path path_;
    uint64_t revision_=1;
    bool dirty_=false;
    std::string error_;
    std::chrono::steady_clock::time_point deadline_;
};
}
