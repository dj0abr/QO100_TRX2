#include "qo100/spectrum.hpp"
#include "qo100/protocol.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);}
int main() {
    try {
        constexpr uint64_t center=10489750003ULL; // Grid anchor need not be a multiple of 25 Hz.
        unsigned frames=0;uint64_t last_index=0;
        qo100::Spectrum spectrum(600000,qo100::spectrum_fft_size,20,[&](const auto& powers,uint64_t index) {
            ++frames;require(index>last_index,"Shared spectrum sample clock stalled");last_index=index;
            const auto wide=qo100::wide_spectrum(powers,center);
            const auto a=qo100::zoom_spectrum(powers,center,center+150007);
            const auto b=qo100::zoom_spectrum(powers,center,center-149988);
            require(wide.bins.size()==2000 && wide.span_hz==600000,"Wrong fullband geometry");
            require(a.bins.size()==2400 && a.span_hz==60000,"Wrong zoom geometry");
            require(a.center_hz==center+150000 && b.center_hz==center-150000,"Zoom grid rounding is wrong");
            require(std::max_element(a.bins.begin(),a.bins.end())-a.bins.begin()==137,"RX1 zoom has wrong RF frequency");
            require(std::max_element(b.bins.begin(),b.bins.end())-b.bins.begin()==1599,"RX2 zoom has wrong RF frequency");
            require(std::abs(a.bins[137]+1204)<=3 && std::abs(b.bins[1599]+2000)<=3,"Shared FFT tone normalization is wrong");
            require(wide.bins[1411]==a.bins[137] && wide.bins[533]==b.bins[1599],"Fullband peak reduction lost a narrow carrier");
            for(const auto hz:{center+275000,center-275000}) {
                const auto edge=qo100::zoom_spectrum(powers,center,hz);
                const auto start=hz>center?edge.bins.end()-200:edge.bins.begin();
                require(std::all_of(start,start+200,[](auto x){return x==-16000;}),"Off-band zoom bins are not blank");
            }
            const qo100::FrameInfo info{frames,0,index,a.center_hz,a.span_hz,uint32_t(a.bins.size()),7,1};
            const auto packets=qo100::spectrum_packets(info,a.bins);
            require(packets.size()==5 && packets.back().size()<=1200,"Reduced zoom packetization is wrong");
        });
        // Two exact-bin carriers, crossing irregular datagram boundaries.
        std::vector<uint8_t> iq;const double pi=std::acos(-1.);
        for(size_t i=0;i<600000;++i) {
            const auto x=std::polar(.25,2*pi*123425*i/600000)+std::polar(.1,-2*pi*140025*i/600000);
            for(const double v:{x.real(),x.imag()}) {
                const auto sample=uint16_t(int16_t(std::lround(v*32768)));
                iq.push_back(sample&255);iq.push_back(sample>>8);
            }
        }
        for(size_t i=0;i<iq.size();i+=4012) require(spectrum.push_iq(iq.data()+i,std::min<size_t>(4012,iq.size()-i)),"Valid IQ rejected");
        require(frames==20 && spectrum.samples()==600000,"Shared FFT publication rate/sample count is wrong");
        bool reset_frame=false;
        qo100::Spectrum reset(600000,24000,20,[&](const auto& powers,uint64_t) {
            reset_frame=true;require(std::all_of(powers.begin(),powers.end(),[](auto p){return p==0;}),"Hardware reset leaked old RF samples");
        });
        for(int i=0;i<24000;++i)reset.push({.5f,0});
        reset.reset();for(int i=0;i<60000;++i)reset.push({});
        require(reset_frame && reset.samples()==84000,"Reset restarted the sample timeline");
        std::cout<<"Shared 25-Hz FFT: RF positions, levels, peak aggregation, non-grid RX tuning, edges, packets, cadence and reset passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
