// SPDX-License-Identifier: GPL-3.0-only
#include "beacon_dsp.hpp"
#include "cpu_budget.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
using Clock=std::chrono::steady_clock;
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> running{true};
void stop(int) {running.store(false,std::memory_order_relaxed);}
uint16_t be16(const uint8_t* p) {return uint16_t((unsigned(p[0])<<8)|p[1]);}
uint32_t be32(const uint8_t* p) {return (uint32_t(be16(p))<<16)|be16(p+2);}
uint64_t be64(const uint8_t* p) {return (uint64_t(be32(p))<<32)|be32(p+4);}
uint64_t number(const std::string& text,uint64_t maximum) {
    if(text.empty() || text.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("Expected an unsigned integer");
    const auto value=std::stoull(text);if(!value || value>maximum)throw std::runtime_error("Argument out of range");return value;
}
struct Wave {unsigned rate;std::vector<int16_t> pcm;};
Wave wave(const std::string& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input || input.tellg()<44 || input.tellg()>32*1024*1024)throw std::runtime_error("Invalid WAV size");
    std::vector<uint8_t> bytes(size_t(input.tellg()));input.seekg(0);input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
    if(!input || std::memcmp(bytes.data(),"RIFF",4) || std::memcmp(bytes.data()+8,"WAVE",4))throw std::runtime_error("Expected RIFF/WAVE");
    auto le16=[&](size_t p){return unsigned(bytes.at(p))|(unsigned(bytes.at(p+1))<<8);};
    auto le32=[&](size_t p){return uint32_t(le16(p))|(uint32_t(le16(p+2))<<16);};
    unsigned rate=0,channels=0;size_t start=0,count=0;
    for(size_t i=12;i+8<=bytes.size();) {
        const auto length=le32(i+4);const size_t p=i+8;
        if(!std::memcmp(bytes.data()+i,"fmt ",4)) {
            if(length<16 || p+16>bytes.size() || le16(p)!=1 || le16(p+14)!=16)throw std::runtime_error("WAV must be PCM16");
            channels=le16(p+2);rate=le32(p+4);
        } else if(!std::memcmp(bytes.data()+i,"data",4)) {
            start=p;count=std::min<size_t>(length,bytes.size()-p);
        }
        if(length>bytes.size()-p)break;
        i=p+length+(length&1);
    }
    // The pinned recording has a known overlarge RIFF size; valid PCM is kept.
    if(!start || !channels || channels>2 || (rate!=8000 && rate!=12000) || count%(2*channels))
        throw std::runtime_error("WAV must contain mono/stereo PCM16 at 8000 or 12000 Hz");
    Wave result{rate,{}};result.pcm.reserve(count/(2*channels));
    for(size_t i=start;i<start+count;i+=2*channels)result.pcm.push_back(int16_t(le16(i)));
    return result;
}
struct Job {uint64_t generation,start,frequency; sockaddr_in peer; std::vector<int16_t> pcm;};
struct Output {uint64_t generation,start;sockaddr_in peer;qo100::beacon::Result result;};
class Worker {
public:
    Worker():thread_([this]{run();}) {}
    ~Worker() {
        {std::lock_guard<std::mutex> lock(mutex_);stopping_=true;pending_.reset();}
        ready_.notify_one();thread_.join();
    }
    void submit(Job job) {
        {std::lock_guard<std::mutex> lock(mutex_);pending_=std::move(job);}
        ready_.notify_one();
    }
    std::deque<Output> take() {
        std::lock_guard<std::mutex> lock(mutex_);std::deque<Output> out;out.swap(output_);return out;
    }
private:
    void run() {
        for(;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);ready_.wait(lock,[&]{return stopping_ || pending_.has_value();});
                if(stopping_)return;
                job=std::move(*pending_);pending_.reset();
            }
            try {
                const auto started=Clock::now();
                auto result=qo100::beacon::decode(job.pcm,12000,job.frequency);
                const auto fec=std::count_if(result.frames.begin(),result.frames.end(),[](const auto& f){return f.fec;});
                std::cout<<"Window "<<job.pcm.size()/12000<<"s: carrier="<<std::fixed<<std::setprecision(1)<<result.carrier
                    <<", CRC="<<result.frames.size()-fec<<", FEC="<<fec<<", "
                    <<std::chrono::duration<double>(Clock::now()-started).count()<<"s elapsed\n";
                std::lock_guard<std::mutex> lock(mutex_);
                if(output_.size()==2)output_.pop_front();
                output_.push_back({job.generation,job.start,job.peer,std::move(result)});
            } catch(const std::exception& e) {std::cerr<<"Beacon decode failed: "<<e.what()<<'\n';}
        }
    }
    std::mutex mutex_;std::condition_variable ready_;std::optional<Job> pending_;
    std::deque<Output> output_;bool stopping_=false;std::thread thread_;
};
struct Socket {int fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0);~Socket(){if(fd>=0)close(fd);}};
int serve(unsigned port) {
    Socket audio;if(audio.fd<0)throw std::runtime_error("Cannot create beacon UDP socket");
    const int capacity=4*1024*1024;setsockopt(audio.fd,SOL_SOCKET,SO_RCVBUF,&capacity,sizeof(capacity));
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(port);
    if(bind(audio.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local)))throw std::runtime_error("Cannot bind beacon UDP port");
    std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
    constexpr size_t limit=40*12000;
    std::vector<int16_t> history(limit);size_t pos=0,fill=0;
    uint64_t generation=0,received=0,next_decode=20*12000,expected=0,frequency=0;
    uint32_t epoch=0,session=0;uint16_t peer_port=0;bool identified=false;
    std::deque<qo100::beacon::Frame> recent;
    Worker worker;
    std::cout<<"PSK beacon receiver: RX4 UDP 127.0.0.1:"<<port
        <<"; C++ 4-kHz DSP; first window 20s, then every 10s; maximum 40s\n";
    std::array<uint8_t,1201> bytes{};
    while(running.load(std::memory_order_relaxed)) {
        pollfd descriptor{audio.fd,POLLIN,0};const int ready=poll(&descriptor,1,25);
        if(ready<0 && errno!=EINTR)throw std::runtime_error("Beacon UDP poll failed");
        if(ready>0 && (descriptor.revents&POLLIN))for(unsigned budget=0;budget<256;++budget) {
            sockaddr_in from{};socklen_t length=sizeof(from);
            const auto n=recvfrom(audio.fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr*>(&from),&length);
            if(n<0)break;
            const auto* p=bytes.data();
            if(n!=544 || from.sin_addr.s_addr!=htonl(INADDR_LOOPBACK) || std::memcmp(p,"QTR2",4) ||
               p[4]!=1 || p[5]!=5 || be16(p+6)!=64 || be32(p+8)!=6 || be32(p+40)!=12000 ||
               be32(p+44) || be32(p+48) || be16(p+52)!=240 || be16(p+54)!=2 || be32(p+56)!=240)continue;
            const auto index=be64(p+24),hz=be64(p+32);
            if(index>std::numeric_limits<uint64_t>::max()-240)continue;
            const bool same=identified && session==be32(p+60) && epoch==be32(p+12) && frequency==hz && peer_port==from.sin_port;
            if(same && index<expected)continue;
            if(!same || index!=expected) {
                ++generation;pos=fill=received=0;next_decode=20*12000;recent.clear();
                std::cout<<"RX4 reset generation="<<generation<<", frequency="<<hz<<", index="<<index<<'\n';
            }
            identified=true;session=be32(p+60);epoch=be32(p+12);frequency=hz;peer_port=from.sin_port;
            expected=index+240;received+=240;
            for(size_t i=0;i<240;++i){history[pos]=int16_t(be16(p+64+2*i));pos=(pos+1)%limit;}
            fill=std::min(limit,fill+240);
            if(received>=next_decode) {
                next_decode+=10*12000;
                Job job{generation,expected-fill,frequency,from,{}};job.pcm.reserve(fill);
                const size_t start=(pos+limit-fill)%limit;
                for(size_t i=0;i<fill;++i)job.pcm.push_back(history[(start+i)%limit]);
                worker.submit(std::move(job));
            }
        }
        for(auto& output:worker.take()) {
            if(output.generation!=generation)continue;
            for(auto& frame:output.result.frames) {
                frame.sample+=output.start;
                if(std::any_of(recent.begin(),recent.end(),[&](const auto& a) {
                    return a.fec==frame.fec && a.payload==frame.payload &&
                        std::abs(int64_t(a.sample)-int64_t(frame.sample))<6000;
                }))continue;
                const auto text=qo100::beacon::format(frame,output.result.carrier);
                sendto(audio.fd,text.data(),text.size(),MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&output.peer),sizeof(output.peer));
                std::cout<<text;recent.push_back(std::move(frame));if(recent.size()>64)recent.pop_front();
            }
        }
    }
    return 0;
}
}
int main(int argc,char** argv) {
    try {
        unsigned port=40912,percent=10;uint64_t frequency=10489748500ULL;std::string wav;bool json=false;
        for(int i=1;i<argc;++i) {
            const std::string option=argv[i];
            if(option=="--help") {
                std::cout<<"qo100_beacon_decoder [--port 40912] [--cpu-percent 10]\n"
                    "                     [--wav FILE --rx-hz 10489748500] [--json]\n";return 0;
            }
            if(option=="--json") {json=true;continue;}
            if(++i==argc)throw std::runtime_error("Missing value for "+option);
            if(option=="--port")port=unsigned(number(argv[i],65535));
            else if(option=="--cpu-percent")percent=unsigned(number(argv[i],100));
            else if(option=="--rx-hz")frequency=number(argv[i],1000000000000ULL);
            else if(option=="--wav")wav=argv[i];
            else throw std::runtime_error("Unknown option: "+option);
        }
        std::cout<<std::unitbuf;std::cerr<<std::unitbuf;
        if(auto exit=qo100::decoder_cpu_budget(percent))return *exit;
        if(wav.empty())return serve(port);
        const auto input=wave(wav);const auto result=qo100::beacon::decode(input.pcm,input.rate,frequency);
        if(json) {
            std::cout<<"{\"carrier_hz\":"<<std::setprecision(12)<<result.carrier<<",\"frames\":[";
            bool first=true;
            for(const auto& frame:result.frames) {
                if(!first)std::cout<<',';
                first=false;
                std::ostringstream hex;hex<<std::hex<<std::setfill('0');for(auto x:frame.payload)hex<<std::setw(2)<<unsigned(x);
                std::cout<<"{\"sample\":"<<frame.sample<<",\"kind\":\""<<(frame.fec?"FEC":"CRC")
                    <<"\",\"corrections\":"<<frame.corrections<<",\"payload_hex\":\""<<hex.str()<<"\"}";
            }
            std::cout<<"]}\n";
        } else {
            for(const auto& frame:result.frames)std::cout<<qo100::beacon::format(frame,result.carrier);
            std::cout<<"Decoded "<<result.frames.size()<<" beacon frames.\n";
        }
        return result.frames.empty()?1:0;
    } catch(const std::exception& e) {std::cerr<<"Beacon decoder: "<<e.what()<<'\n';return 1;}
}
