#include "qo100/tx_channel.hpp"
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>

namespace {
constexpr double pi=3.14159265358979323846;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
std::complex<double> measure(double amplitude) {
    // Even a voice compressor setting of 140% must have no effect in digital mode.
    qo100::TxChannel digital(-150000,{1400},qo100::TxChannel::Profile::digital);
    uint64_t index=0,iq_index=0,count=0;
    auto add=[&] {
        std::vector<int16_t> pcm(240);
        for(size_t i=0;i<pcm.size();++i) pcm[i]=int16_t(std::lround(amplitude*32767*(
            std::cos(2*pi*1000*(index+i)/12000)+std::cos(2*pi*3500*(index+i)/12000))));
        require(digital.add(index,pcm),"Digital PCM rejected");index+=240;
    };
    for(int i=0;i<6;++i) add();
    std::complex<double> low{},high{},lsb{};
    for(int block=0;block<55;++block) {
        if(block) add();
        const auto iq=digital.render(true);
        if(block>15) for(size_t i=0;i<iq.size();++i) {
            const double time=double(iq_index+i)/1200000;
            const auto value=std::complex<double>(iq[i]);
            low+=value*std::polar(1.,2*pi*149000*time);
            high+=value*std::polar(1.,2*pi*146500*time);
            lsb+=value*std::polar(1.,2*pi*151000*time);++count;
        }
        iq_index+=iq.size();
    }
    require(digital.compression_db()==0 && digital.limiting_db()==0,"Digital profile ran speech processing");
    require(std::abs(lsb)<std::abs(low)*.001,"Digital lower sideband leaked");
    require(digital.underruns==0 && digital.overruns==0,"Digital steady clock interrupted");
    const auto silence=digital.render(false);
    for(auto x:silence) require(x==std::complex<float>{},"Stale digital audio was repeated");
    return {std::abs(low)/count,std::abs(high)/count};
}
}
int main() {
    try {
        const auto strong=measure(.1),weak=measure(.01);
        require(strong.real()>.048 && strong.real()<.052,"Digital 1-kHz level changed");
        require(strong.imag()>.048 && strong.imag()<.052,"Digital 3.5-kHz passband missing");
        require(std::abs(strong.real()/weak.real()-10)<.02 && std::abs(strong.imag()/weak.imag()-10)<.02,"Digital modulation is nonlinear");
        std::cout<<"TX2 digital: flat 1/3.5-kHz passband, linear gain, compressor/limiter bypass, USB rejection and exact silence passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
