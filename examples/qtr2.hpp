#pragma once
// Standalone QTR2 helpers for a Linux/POSIX GUI; no backend or DSP headers needed.
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace qtr2 {
using Packet=std::vector<uint8_t>;
using Clock=std::chrono::steady_clock;
inline uint64_t get(const Packet& p,size_t off,size_t width) {
    if(width>8 || off+width>p.size()) throw std::runtime_error("Short QTR2 packet");
    uint64_t v=0;for(size_t i=0;i<width;++i) v=(v<<8)|p[off+i];return v;
}
inline void put(Packet& p,size_t off,uint64_t v,size_t width) {
    if(width>8 || off+width>p.size()) throw std::runtime_error("Short QTR2 packet");
    for(size_t i=0;i<width;++i) p[off+width-1-i]=uint8_t(v>>(8*i));
}
inline Packet header(uint8_t type,uint32_t stream=0,uint32_t request=0,uint32_t session=0) {
    Packet p(64,0);std::memcpy(p.data(),"QTR2",4);p[4]=1;p[5]=type;
    put(p,6,64,2);put(p,8,stream,4);put(p,12,request,4);put(p,60,session,4);return p;
}
inline Packet settings(uint32_t request,uint32_t session=0,uint64_t revision=0,const std::string& patch="") {
    if(patch.size()>1136) throw std::runtime_error("Settings patch too large");
    auto p=header(17,0,request,session);
    if(!patch.empty()) {
        put(p,24,revision,8);put(p,44,patch.size(),4);put(p,52,patch.size(),2);
        put(p,54,1,2);put(p,56,patch.size(),4);p.insert(p.end(),patch.begin(),patch.end());
    }
    return p;
}
inline std::map<std::string,std::string> values(const Packet& p) {
    std::map<std::string,std::string> out;
    std::istringstream text(std::string(p.begin()+64,p.end()));std::string line;
    while(std::getline(text,line)) {auto eq=line.find('=');if(eq!=std::string::npos) out[line.substr(0,eq)]=line.substr(eq+1);}
    return out;
}
inline Packet tx_audio(uint32_t epoch,uint64_t index,uint64_t hz,uint32_t session,const std::array<int16_t,240>& pcm) {
    if(index%240) throw std::runtime_error("TX index must be a multiple of 240");
    auto p=header(8,3,epoch,session);p.resize(544);
    put(p,24,index,8);put(p,32,hz,8);put(p,40,12000,4);put(p,52,240,2);put(p,54,2,2);put(p,56,240,4);
    for(size_t i=0;i<pcm.size();++i) put(p,64+2*i,uint16_t(pcm[i]),2);
    return p;
}
class Udp {
    int fd_=-1;
public:
    explicit Udp(const std::string& ip,uint16_t port=40900) {
        sockaddr_in peer{};peer.sin_family=AF_INET;peer.sin_port=htons(port);
        if(!port || inet_pton(AF_INET,ip.c_str(),&peer.sin_addr)!=1) throw std::runtime_error("Expected backend IPv4 address/port");
        fd_=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
        if(fd_<0) throw std::runtime_error("Cannot create UDP socket");
        int buffer=4*1024*1024;setsockopt(fd_,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer));
        if(connect(fd_,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))<0) {
            close(fd_);throw std::runtime_error("Cannot connect UDP socket");
        } // The OS chooses a local port. Keep this socket for sends AND receives.
    }
    ~Udp() {if(fd_>=0) close(fd_);}
    Udp(const Udp&)=delete;
    Udp& operator=(const Udp&)=delete;
    void send(const Packet& p) {
        const auto n=::send(fd_,p.data(),p.size(),0);
        if(n<0 && (errno==ECONNREFUSED || errno==EINTR)) return; // Retry while backend starts.
        if(n!=ssize_t(p.size())) throw std::runtime_error("UDP send failed");
    }
    Packet receive(int timeout_ms=100) {
        pollfd event{fd_,POLLIN,0};if(poll(&event,1,timeout_ms)<=0) return {};
        std::array<uint8_t,65536> bytes;const auto n=recv(fd_,bytes.data(),bytes.size(),0);
        if(n<0 && (errno==ECONNREFUSED || errno==EINTR)) return {};
        if(n<64 || n>1200 || std::memcmp(bytes.data(),"QTR2",4) || bytes[4]!=1 || bytes[6]!=0 || bytes[7]!=64) return {};
        return Packet(bytes.begin(),bytes.begin()+n);
    }
};
// Retries controls, renews SUBSCRIBE, matches type/stream/request/session.
// Streaming packets received during this short handshake are discarded.
inline Packet rpc(Udp& udp,const Packet& request,uint8_t reply_type) {
    auto next_sub=Clock::time_point::min(),next_send=next_sub;
    const auto end=Clock::now()+std::chrono::seconds(12);
    while(Clock::now()<end) {
        const auto now=Clock::now();
        if(now>=next_sub) {udp.send(header(1));next_sub=now+std::chrono::seconds(2);}
        if(now>=next_send) {udp.send(request);next_send=now+std::chrono::milliseconds(250);}
        auto p=udp.receive();if(p.empty()) continue;
        if(p[5]!=reply_type || get(p,8,4)!=get(request,8,4) || get(p,12,4)!=get(request,12,4) ||
           (get(request,60,4) && get(p,60,4)!=get(request,60,4))) continue;
        if((reply_type==18 && (get(p,48,4)&2)) || (reply_type==14 && get(p,54,2)==3)) continue;
        return p;
    }
    throw std::runtime_error("No matching backend reply within 12 seconds");
}
}
