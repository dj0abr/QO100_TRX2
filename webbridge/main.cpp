#include "bridge.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace {
std::string trim(std::string s) {
    const auto first=s.find_first_not_of(" \t\r\n");
    return first==std::string::npos?"":s.substr(first,s.find_last_not_of(" \t\r\n")-first+1);
}
unsigned number(const std::string& s,unsigned lo,unsigned hi) {
    size_t used=0;const auto n=std::stoul(s,&used);
    if(used!=s.size() || n<lo || n>hi) throw std::runtime_error("Invalid numeric value: "+s);
    return unsigned(n);
}
struct FtEnable {
    std::optional<bool> enabled;
    unsigned legacy_audio = 0, legacy_text = 0;
    bool value() const {
        if (enabled) return *enabled;
        if (bool(legacy_audio) != bool(legacy_text))
            throw std::runtime_error("Both legacy RX2 decoder ports must be set together");
        return legacy_audio != 0;
    }
};
std::string local_path(const std::filesystem::path& base, const std::string& value) {
    return value.empty() ? "" : std::filesystem::absolute(base/value).lexically_normal().string();
}
void setting(bridge::Config& c,FtEnable& ft,const std::string& k,const std::string& v,const std::filesystem::path& base={}) {
    if(k=="listen") c.listen=v;
    else if(k=="port") c.port=number(v,1,65535);
    else if(k=="backend_port") c.backend_port=number(v,1,65535);
    else if(k=="ft_enabled") ft.enabled=number(v,0,1)!=0;
    else if(k=="rx2_decoder_audio_port") ft.legacy_audio=number(v,0,65535);
    else if(k=="rx2_decoder_text_port") ft.legacy_text=number(v,0,65535);
    else if(k=="ft_station_path") c.ft_station_path=local_path(base,v);
    else if(k=="ft_log_path") c.ft_log_path=local_path(base,v);
    else if(k=="ft8_tx_lead_ms") c.ft8_tx_lead_ms=number(v,0,500);
    else if(k=="ft_ptt_lead_ms") c.ft_ptt_lead_ms=number(v,0,1000);
    else if(k=="ft_rx_offset_ms") {
        size_t used=0;const auto offset=std::stoi(v,&used);
        if(used!=v.size() || offset < -5000 || offset > 5000)
            throw std::runtime_error("Invalid FT RX time offset: "+v);
        c.ft_rx_offset_ms=offset;
    }
    else if(k=="rx3_decoder_audio_port") c.decoders[0].audio_port=number(v,0,65535);
    else if(k=="rx3_decoder_text_port") c.decoders[0].text_port=number(v,0,65535);
    else if(k=="rx4_decoder_audio_port") c.decoders[1].audio_port=number(v,0,65535);
    else if(k=="rx4_decoder_text_port") c.decoders[1].text_port=number(v,0,65535);
    else if(k=="rx5_decoder_audio_port") c.rx5_decoder_audio_port=number(v,0,65535);
    else if(k=="mm_http_port") c.mm_http_port=number(v,0,65535);
    else if(k=="digital_audio_port") c.digital_audio_port=number(v,0,65535);
    else if(k=="digital_bridge_port") c.digital_bridge_port=number(v,0,65535);
    else if(k=="idle_seconds") c.idle_seconds=number(v,0,86400);
    else if(k=="heartbeat_seconds") c.heartbeat_seconds=number(v,2,3600);
    else if(k=="max_clients") c.max_clients=number(v,1,256);
    else if(k=="queue_bytes") c.queue_bytes=number(v,65536,4194304);
    else if(k=="web_root") c.web_root=(base/std::filesystem::path(v)).lexically_normal().string();
    else throw std::runtime_error("Unknown webbridge setting: "+k);
}
}
int main(int argc,char** argv) {
    try {
        bridge::Config c;
        FtEnable ft;
        if (const char* key = std::getenv("QO100_QRZ_API_KEY")) c.ft_qrz_api_key = trim(key);
        for(int i=1;i<argc;++i) if(std::string(argv[i])=="--config") {
            if(++i==argc) throw std::runtime_error("Missing config path");
            const auto path=std::filesystem::absolute(argv[i]);std::ifstream file(path);
            if(!file) throw std::runtime_error("Cannot read "+path.string());
            setting(c,ft,"ft_log_path","../data/ft4-ft8.adi",path.parent_path());
            setting(c,ft,"ft_station_path","../data/ft-station.json",path.parent_path());
            std::string line;std::set<std::string> seen;
            while(std::getline(file,line)) {
                line=trim(line.substr(0,line.find('#')));if(line.empty()) continue;
                const auto eq=line.find('=');if(eq==std::string::npos) throw std::runtime_error("Expected key = value");
                const auto k=trim(line.substr(0,eq));if(!seen.insert(k).second) throw std::runtime_error("Duplicate setting: "+k);
                setting(c,ft,k,trim(line.substr(eq+1)),path.parent_path());
            }
        }
        for(int i=1;i<argc;++i) {
            std::string option=argv[i];
            if(option=="--help") {
                std::cout<<"qo100_webbridge [--config FILE] [--listen IPv4] [--port 8080] [--backend-port 40900]\n"
                    "                [--ft-enabled 0|1] [--ft8-tx-lead-ms 80] [--ft-ptt-lead-ms 450]\n"
                    "                [--ft-rx-offset-ms 0] [--ft-station-path FILE] [--ft-log-path FILE]\n"
                    "                [--rx3-decoder-audio-port 0] [--rx3-decoder-text-port 0]\n"
                    "                [--rx4-decoder-audio-port 0] [--rx4-decoder-text-port 0]\n"
                    "                [--rx5-decoder-audio-port 0] [--mm-http-port 0]\n"
                    "                [--digital-audio-port 0] [--digital-bridge-port 0]\n"
                    "                [--web-root DIR] [--idle-seconds 0] [--heartbeat-seconds 60]\n"
                    "                [--max-clients 16] [--queue-bytes 524288]\n";return 0;
            }
            if(++i==argc) throw std::runtime_error("Missing value for "+option);
            if(option=="--config") continue;
            if(option.rfind("--",0)!=0) throw std::runtime_error("Unknown argument: "+option);
            option=option.substr(2);for(auto& ch:option) if(ch=='-') ch='_';
            setting(c,ft,option,argv[i]);
        }
        c.ft_enabled=ft.value();
        c.web_root=std::filesystem::canonical(c.web_root).string();
        // One event loop serializes roles, UDP controls, and socket operations.
        bridge::net::io_context io(1);bridge::Bridge adapter(io,c);
        bridge::listen(io,adapter);adapter.start();
        bridge::net::signal_set signals(io,SIGINT,SIGTERM);
        bridge::net::steady_timer shutdown(io);
        signals.async_wait([&](auto ec,int) {if(ec) return;adapter.stop();shutdown.expires_after(std::chrono::seconds(2));
            shutdown.async_wait([&](auto){io.stop();});});
        std::cout<<"HTTP/WS http://"<<c.listen<<':'<<c.port<<"; backend UDP 127.0.0.1:"<<c.backend_port
            <<"; idle timeout "<<c.idle_seconds<<" s (0=disabled); WebSocket heartbeat timeout "<<c.heartbeat_seconds<<" s\n"<<std::flush;
        for(size_t i=0;i<c.decoders.size();++i) if(c.decoders[i].audio_port)
            std::cout<<"RX"<<(i+3)<<" audio UDP 127.0.0.1:"<<c.decoders[i].audio_port
                <<"; decoder text UDP 127.0.0.1:"<<c.decoders[i].text_port<<'\n'<<std::flush;
        io.run();return 0;
    } catch(const std::exception& e) {std::cerr<<"Webbridge: "<<e.what()<<'\n';return 1;}
}
