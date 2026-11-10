#include "qo100/rx_channel.hpp"
#include <algorithm>
#include <array>
#include <complex>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

using Complex=std::complex<float>;
struct Packet {
    uint64_t index;
    std::vector<int16_t> pcm;
    bool operator==(const Packet& other) const {return index==other.index && pcm==other.pcm;}
};
struct Capture {
    std::vector<Complex> channel;
    std::vector<Packet> audio;
    uint64_t next=0;
    void add(const std::vector<int16_t>& samples,uint64_t index) {
        if(index!=next || samples.size()!=240) throw std::runtime_error("PCM clock/packet size changed");
        next+=samples.size();audio.push_back({index,samples});
    }
};
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
void compare(qo100::RxChannel::Profile profile,std::optional<uint32_t> nr) {
    Capture expected,actual;
    qo100::RxChannel scalar(-1500,[&](Complex x){expected.channel.push_back(x);},
        [&](const auto& p,uint64_t i){expected.add(p,i);},{},nr,profile);
    qo100::RxChannel blocked(-1500,[&](Complex x){actual.channel.push_back(x);},
        [&](const auto& p,uint64_t i){actual.add(p,i);},{},nr,profile);
    const std::array<size_t,9> lengths{42017,57003,5011,12029,15013,30011,45001,67003,65537};
    const std::array<size_t,12> sizes{1,3,7,8,255,256,4093,4096,4097,8192,16383,65537};
    uint32_t random=1;size_t chunk=0;
    for(size_t phase=0;phase<lengths.size();++phase) {
        const auto retune=[&](int64_t hz){scalar.retune(hz);blocked.retune(hz);};
        switch(phase) {
            case 1:retune(1500);break;
            case 2:retune(0);break;
            case 3:retune(-1);break;
            case 4:
                scalar.set_filter(qo100::RxFilterSettings::from_edges(300,2700));
                blocked.set_filter(qo100::RxFilterSettings::from_edges(300,2700));
                if(nr){scalar.set_nr(60);blocked.set_nr(60);}break;
            case 5:
                scalar.set_filter(qo100::RxFilterSettings::from_edges(700,1200));
                blocked.set_filter(qo100::RxFilterSettings::from_edges(700,1200));
                if(nr){scalar.set_nr(0);blocked.set_nr(0);}break;
            case 6:
                scalar.tune(-2000);blocked.tune(-2000);expected.next=actual.next=0;break;
            case 7:retune(275000);break;
            case 8:
                scalar.tune(-275000);blocked.tune(-275000);expected.next=actual.next=0;break;
        }
        std::vector<Complex> input(lengths[phase]);
        for(auto& x:input) {
            const auto noise=[&](){random=random*1664525+1013904223;return (float(random>>8)/16777216.f-.5f)*.2f;};
            x=phase==8?Complex{}:Complex{noise(),noise()};
        }
        const auto unchanged=input;
        blocked.push_block(nullptr,0);
        for(size_t first=0;first<input.size();) {
            const size_t count=std::min(sizes[chunk%sizes.size()],input.size()-first);
            for(size_t i=0;i<count;++i)scalar.push(input[first+i]);
            if(chunk%3==0) {
                blocked.push(input[first]);
                blocked.push_block(input.data()+first+1,count-1);
            } else blocked.push_block(input.data()+first,count);
            first+=count;++chunk;
        }
        require(input==unchanged,"RX modified the I/Q block shared with other receivers");
        require(expected.channel==actual.channel,"Complex channel samples depend on block boundaries");
        require(expected.audio==actual.audio,"PCM or sample indices differ from scalar processing");
    }
    require(expected.audio.size()>20,"Block comparison did not produce sufficient audio");
}
int main() {
    try {
        compare(qo100::RxChannel::Profile::normal,std::nullopt);
        compare(qo100::RxChannel::Profile::normal,0);
        compare(qo100::RxChannel::Profile::multimedia,std::nullopt);
        std::cout<<"RX blocks: exact complex/PCM equality, immutable input, odd/large/empty blocks, mixed calls, retuning, filters, NR and hardware resets passed\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
