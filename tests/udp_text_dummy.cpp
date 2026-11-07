// Standalone decoder placeholder: receive RX3 PCM, return slow plain UTF-8 text.
// This program never subscribes to or controls the backend.
#include "wire.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>

namespace {
volatile std::sig_atomic_t running=1;
void stop(int) {running=0;}
struct Socket {
    int fd=socket(AF_INET,SOCK_DGRAM,0);
    ~Socket() {if(fd>=0) close(fd);}
};
unsigned number(const std::string& s,unsigned maximum) {
    if(s.empty() || s.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("Expected positive integer: "+s);
    const auto n=std::stoul(s);
    if(!n || n>maximum) throw std::runtime_error("Number out of range: "+s);
    return unsigned(n);
}
}
int main(int argc,char** argv) {
    try {
        unsigned port=40910,interval_ms=1000;
        for(int i=1;i<argc;++i) {
            const std::string option=argv[i];
            if(option=="--help") {
                std::cout<<"test_udp_text_decoder [--port 40910] [--interval-ms 1000]\n"
                    "Receives QTR2 RX3 AUDIO at 12 kHz on loopback UDP; replies with abcdef, one letter per interval.\n";
                return 0;
            }
            if(++i==argc) throw std::runtime_error("Missing value for "+option);
            if(option=="--port") port=number(argv[i],65535);
            else if(option=="--interval-ms") interval_ms=number(argv[i],60000);
            else throw std::runtime_error("Unknown argument: "+option);
        }
        Socket audio;
        if(audio.fd<0) throw std::runtime_error("Cannot create decoder UDP socket");
        sockaddr_in local{};local.sin_family=AF_INET;local.sin_addr.s_addr=htonl(INADDR_LOOPBACK);local.sin_port=htons(port);
        if(bind(audio.fd,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0)
            throw std::runtime_error("Cannot bind decoder UDP port "+std::to_string(port));
        std::signal(SIGINT,stop);std::signal(SIGTERM,stop);
        std::cerr<<"Dummy decoder: RX3 QTR2 audio on 127.0.0.1:"<<port<<"; one letter every "<<interval_ms<<" ms\n";
        using Clock=std::chrono::steady_clock;
        Clock::time_point last_rx{},next_text{};
        std::array<uint8_t,1201> buffer{};sockaddr_in peer{};
        uint32_t session=0,epoch=0;uint64_t frequency=0,packets=0;
        unsigned letter=0;
        while(running) {
            pollfd descriptor{audio.fd,POLLIN,0};
            const auto result=poll(&descriptor,1,100);
            if(result<0) {if(errno==EINTR) continue;throw std::runtime_error("Decoder UDP poll failed");}
            if(!result || !(descriptor.revents&POLLIN)) continue;
            sockaddr_in from{};socklen_t length=sizeof(from);
            const auto n=recvfrom(audio.fd,buffer.data(),buffer.size(),0,reinterpret_cast<sockaddr*>(&from),&length);
            if(n<0) {if(errno==EINTR) continue;throw std::runtime_error("Decoder UDP receive failed");}
            if(n!=544 || from.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)) continue;
            const bridge::Bytes p(buffer.begin(),buffer.begin()+n);
            if(!bridge::valid(p) || p[5]!=5 || bridge::get(p,8,4)!=5 || bridge::get(p,40,4)!=12000 ||
               bridge::get(p,52,2)!=240 || bridge::get(p,54,2)!=2 || bridge::get(p,56,4)!=240 || !bridge::get(p,60,4)) continue;
            ++packets;
            const auto now=Clock::now();
            if(from.sin_port!=peer.sin_port || bridge::get(p,60,4)!=session || bridge::get(p,12,4)!=epoch ||
               bridge::get(p,32,8)!=frequency) {
                peer=from;session=bridge::get(p,60,4);epoch=bridge::get(p,12,4);frequency=bridge::get(p,32,8);
                letter=0;next_text=now+std::chrono::milliseconds(interval_ms);
            }
            // No incoming audio means no text; resume the timer after a receive gap.
            if(now-last_rx>std::chrono::milliseconds(500)) next_text=now+std::chrono::milliseconds(interval_ms);
            last_rx=now;
            if(now<next_text) continue;
            std::string text(1,"abcdef"[letter]);
            if(++letter==6) {letter=0;text+='\n';}
            (void)sendto(audio.fd,text.data(),text.size(),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer));
            std::cout<<text<<std::flush;
            next_text=now+std::chrono::milliseconds(interval_ms);
        }
        std::cerr<<"Dummy decoder: received "<<packets<<" packets / "<<packets*240<<" PCM samples\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"Dummy decoder: "<<e.what()<<'\n';return 1;}
}
