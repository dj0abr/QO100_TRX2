// Build: g++ -std=c++17 -O2 -Wall -Wextra examples/gui_udp.cpp -o gui_udp_example
#include "qtr2.hpp"
#include <cmath>
#include <iostream>

int main(int argc,char** argv) {
    try {
        if(argc<2) {
            std::cout<<"Usage: gui_udp_example BACKEND_IP [--port 40900] [--seconds 3] [--rx-hz HZ] [--set key=value]\n";
            return 0;
        }
        uint16_t port=40900;double seconds=3;uint64_t rx_override=0;std::string patch;
        for(int i=2;i<argc;++i) {
            const std::string option=argv[i];if(++i>=argc) throw std::runtime_error("Missing option value");
            const std::string value=argv[i];size_t used=0;
            if(option=="--port") {
                const auto n=std::stoul(value,&used);if(used!=value.size() || !n || n>65535) throw std::runtime_error("Invalid port");
                port=uint16_t(n);
            } else if(option=="--seconds") {
                seconds=std::stod(value,&used);if(used!=value.size() || !std::isfinite(seconds) || seconds<=0) throw std::runtime_error("Invalid duration");
            } else if(option=="--rx-hz") {
                rx_override=std::stoull(value,&used);if(used!=value.size() || !rx_override) throw std::runtime_error("Invalid RX frequency");
            } else if(option=="--set") {
                if(value.find('=')==std::string::npos) throw std::runtime_error("Use --set key=value");
                patch+=value+"\n";
            } else throw std::runtime_error("Unknown option: "+option);
        }
        using namespace qtr2;
        Udp udp(argv[1],port);
        uint32_t settings_request=1;
        auto snapshot=rpc(udp,settings(settings_request),18);
        if(get(snapshot,54,2)) throw std::runtime_error("Settings query rejected");
        const auto session=uint32_t(get(snapshot,60,4));
        // Optional durable patch. Rebase on revision conflicts; never resend with a new ID just because of loss.
        if(!patch.empty()) {
            for(int attempt=0;attempt<4;++attempt) {
                auto request=settings(++settings_request,session,get(snapshot,24,8),patch);
                snapshot=rpc(udp,request,18);
                const auto status=get(snapshot,54,2);
                if(status==2 && attempt<3) continue;
                if(status) throw std::runtime_error("Settings rejected: "+values(snapshot)["error"]);
                std::cout<<"SETTINGS saved revision="<<get(snapshot,24,8)<<'\n';break;
            }
        }
        std::cout<<"SESSION "<<session<<"\n"<<std::string(snapshot.begin()+64,snapshot.end());
        const auto config=values(snapshot);
        std::array<uint64_t,3> frequencies{0,rx_override?rx_override:std::stoull(config.at("initial_rx_hz")),std::stoull(config.at("initial_rx2_hz"))};
        std::array<Packet,3> tunes;
        std::array<uint32_t,3> epochs{};
        for(uint32_t stream:{1u,2u}) {
            tunes[stream]=header(3,stream,1,session);put(tunes[stream],32,frequencies[stream],8);
            const auto state=rpc(udp,tunes[stream],4);
            if(get(state,54,2)) throw std::runtime_error("RX frequency rejected");
            epochs[stream]=uint32_t(get(state,24,8));
        }
        std::array<uint64_t,3> audio{},next_index{};
        std::array<uint64_t,2> fragments{};
        auto next_sub=Clock::now(),next_tune=next_sub;
        const auto end=Clock::now()+std::chrono::duration<double>(seconds);
        while(Clock::now()<end) {
            const auto now=Clock::now();
            if(now>=next_sub) {udp.send(header(1));next_sub=now+std::chrono::seconds(2);}
            if(now>=next_tune) {for(uint32_t stream:{1u,2u}) udp.send(tunes[stream]);next_tune=now+std::chrono::seconds(1);}
            const auto p=udp.receive();if(p.empty()) continue;
            if(get(p,60,4)!=session) throw std::runtime_error("Backend restarted: repeat settings/epoch handshake");
            const auto stream=get(p,8,4);
            if(p[5]==4 && stream>=1 && stream<=2 && get(p,12,4)==1 && !get(p,54,2)) {
                const auto epoch=uint32_t(get(p,24,8));
                if(epoch!=epochs[stream]) {epochs[stream]=epoch;next_index[stream]=0;}
            } else if(p[5]==5 && stream>=1 && stream<=2 && get(p,12,4)==epochs[stream] &&
                      get(p,40,4)==12000 && get(p,54,2)==2 && get(p,52,2)==240 && get(p,56,4)==240 && p.size()==544) {
                const auto index=get(p,24,8);if(index<next_index[stream]) continue;
                // A real GUI inserts silence for index gaps and reorders packets in a jitter buffer.
                std::array<int16_t,240> pcm;
                for(size_t i=0;i<pcm.size();++i) {
                    const int v=int(get(p,64+2*i,2));pcm[i]=int16_t(v>=32768?v-65536:v);
                }
                // Hook for the external GUI: enqueue pcm (12 kHz mono) here.
                next_index[stream]=index+240;++audio[stream];
            } else if(p[5]==2 && stream<=1 && get(p,54,2)==1) {
                const auto size=get(p,44,4),first=get(p,48,4),count=get(p,52,2);
                if(size<1024 || size>65536 || get(p,56,4)!=size || !count ||
                   first>=size || count>size-first || p.size()!=64+2*count) continue;
                // Hook: assemble bins by (session, stream, frame_id); each signed int16 / 100 is dBFS.
                // Bin k: center_hz + (k - fft_size/2) * sample_rate / fft_size.
                ++fragments[stream];
            }
        }
        std::cout<<"RX1 packets="<<audio[1]<<" RX2 packets="<<audio[2]
                 <<" wide_fragments="<<fragments[0]<<" zoom_fragments="<<fragments[1]<<'\n';
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
