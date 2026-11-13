// RX2 or RX3 UDP audio -> UTC slots -> independent WSJT-X process.
#include "utc_slots.hpp"
#include "cpu_budget.hpp"
#include "qo100/channels.hpp"
#include <nlohmann/json.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> running{true};
void stop(int) {running.store(false,std::memory_order_relaxed);}
struct Socket {int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);~Socket(){if(fd>=0) close(fd);}};
unsigned number(const std::string& s,unsigned maximum) {
    if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Expected positive integer: "+s);
    const auto n=std::stoul(s);if(!n || n>maximum) throw std::runtime_error("Number out of range: "+s);return unsigned(n);
}
int64_t offset_ns(const std::string& text) {
    size_t used=0;const auto value=std::stod(text,&used);
    if(used!=text.size() || !std::isfinite(value) || std::abs(value)>5000) throw std::runtime_error("FT8 time offset must be within +/-5000 ms");
    return std::llround(value*1e6);
}
}
int main(int argc,char** argv) {
    try {
        unsigned port=0,receiver=3,timeout_ms=45000,cpu_percent=10;int64_t offset=0;std::string executable;
        if(const char* value=std::getenv("FT8_TIME_OFFSET_MS")) offset=offset_ns(value);
        for(int i=1;i<argc;++i) {
            const std::string option=argv[i];
            if(option=="--help") {
                std::cout<<"qo100_ft8_decoder [--rx 2|3] [--port PORT] [--time-offset-ms 0]\n"
                    "                  [--decoder FILE] [--decode-timeout-ms 45000] [--cpu-percent 10]\n"
                    "QTR2 AUDIO (12 kHz) on loopback UDP; RX3 returns text, RX2 returns JSON.\n"
                    "Collects UTC-aligned FT8 or FT4 decode windows selected by the RX2 bridge; ft8-decoder must be beside this program.\n";
                return 0;
            }
            if(++i==argc) throw std::runtime_error("Missing value for "+option);
            if(option=="--port") port=number(argv[i],65535);
            else if(option=="--rx") {
                receiver=number(argv[i],3);
                if(receiver<2) throw std::runtime_error("FT8 receiver must be RX2 or RX3");
            }
            else if(option=="--time-offset-ms") offset=offset_ns(argv[i]);
            else if(option=="--decoder") executable=argv[i];
            else if(option=="--cpu-percent") cpu_percent=number(argv[i],100);
            else if(option=="--decode-timeout-ms") timeout_ms=number(argv[i],120000);
            else throw std::runtime_error("Unknown argument: "+option);
        }
        const bool rx2=receiver==2;
        if(!port) port=rx2?40918:40910;
        if(executable.empty()) executable=FT8DecoderProcess::executablePath();
        if(access(executable.c_str(),X_OK)!=0) throw std::runtime_error("Cannot execute FT8 core: "+executable);
        if(auto exit=qo100::decoder_cpu_budget(cpu_percent))return *exit;
        Socket audio;if(audio.fd<0) throw std::runtime_error("Cannot create FT8 UDP socket");
        const int capacity=4*1024*1024;(void)setsockopt(audio.fd,SOL_SOCKET,SO_RCVBUF,&capacity,sizeof(capacity));
        if(fcntl(audio.fd,F_SETFL,O_NONBLOCK)<0) throw std::runtime_error("Cannot set FT8 UDP nonblocking mode");
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(port);
        if(bind(audio.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0) throw std::runtime_error("Cannot bind FT8 UDP port "+std::to_string(port)+" (stop the old decoder first)");
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
        sockaddr_in peer{};std::mutex stream_mutex;FT8DecoderProcess* worker=nullptr;
        qo100::Ft8UtcSlots slots([&](FT8Slot&& slot) {worker->enqueue(std::move(slot));},offset,rx2,rx2);
        int64_t publishedSlot=0;
        uint64_t publishedGeneration=0;
        std::vector<std::pair<std::string,double>> published;
        FT8DecoderProcess decoder(executable,std::chrono::milliseconds(timeout_ms),[&](FT8DecodedSlot&& result) {
            std::lock_guard<std::mutex> lock(stream_mutex);
            if(result.generation!=slots.generation()) return;
            if(result.utcMilliseconds!=publishedSlot || result.generation!=publishedGeneration) {
                publishedSlot=result.utcMilliseconds;
                publishedGeneration=result.generation;
                published.clear();
            }
            for(const auto& line:result.lines) {
                if(line.size()>4095) {std::cerr<<"[FT8] Skipped oversized decoder line\n";continue;}
                auto text=line+'\n';
                if(rx2) {
                    auto decode=nlohmann::json::parse(line,nullptr,false);
                    if(!decode.is_object() || decode.value("type","")!="decode") continue;
                    const auto message=decode.value("message","");
                    const auto frequency=decode.value("frequency_hz",0.0);
                    if(std::any_of(published.begin(),published.end(),[&](const auto& prior) {
                        return prior.first==message && std::abs(prior.second-frequency)<10.0;
                    })) continue;
                    published.emplace_back(message,frequency);
                    decode["session"]=result.session;
                    decode["epoch"]=result.epoch;
                    decode["dial_hz"]=result.dialHz;
                    decode["slot_utc"]=result.utcMilliseconds/1000.0;
                    decode["mode"]=ft_mode_name(result.mode);
                    decode["mode_revision"]=result.modeRevision;
                    text=decode.dump();
                }
                if(sendto(audio.fd,text.data(),text.size(),MSG_DONTWAIT,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer))<0)
                    std::cerr<<"[FT8] Could not send decoder text\n";
            }
        },rx2);
        worker=&decoder;
        std::cerr<<"FT8 receiver: RX"<<receiver<<" UDP 127.0.0.1:"<<port<<"; 12000 samples/s; UTC offset "<<offset/1e6<<" ms\n";
        std::cerr<<"FT8 core: "<<executable<<"; waiting for a complete UTC slot (00/15/30/45 s)\n";
        std::array<uint8_t,1201> buffer{};uint64_t packets=0,malformed=0;
        while(running.load(std::memory_order_relaxed)) {
            pollfd descriptor{audio.fd,POLLIN,0};const auto ready=poll(&descriptor,1,100);
            if(ready<0) {if(errno==EINTR) continue;throw std::runtime_error("FT8 UDP poll failed");}
            if(!ready || !(descriptor.revents&POLLIN)) continue;
            for(unsigned budget=0;budget<256 && running.load(std::memory_order_relaxed);++budget) {
                sockaddr_in from{};socklen_t size=sizeof(from);
                const auto n=recvfrom(audio.fd,buffer.data(),buffer.size(),0,reinterpret_cast<sockaddr*>(&from),&size);
                if(n<0) {if(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR) break;throw std::runtime_error("FT8 UDP receive failed");}
                qo100::Ft8PcmBlock block;
                if(from.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || !qo100::parse_ft8_audio(buffer.data(),size_t(n),block,rx2?2u:qo100::rx3_stream)) {++malformed;continue;}
                ++packets;
                std::lock_guard<std::mutex> lock(stream_mutex);
                if(from.sin_port!=peer.sin_port) {slots.reset("adapter endpoint changed");peer=from;}
                const auto discarded=slots.discarded(),filled=slots.filled_samples();
                slots.push(block);
                if(slots.filled_samples()!=filled)
                    std::cerr<<"[FT8] Filled short audio gap: "<<(slots.filled_samples()-filled)/12<<" ms\n";
                if(slots.discarded()!=discarded) std::cerr<<"[FT8] Discarded partial slot: "<<slots.reason()<<'\n';
            }
        }
        decoder.stop();
        std::cerr<<"FT8 receiver: "<<packets<<" packets; "<<malformed<<" malformed; "<<slots.discarded()<<" discarded partial slots\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FT8 receiver: "<<e.what()<<'\n';return 1;}
}
