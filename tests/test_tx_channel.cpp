#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main() {
    using namespace qo100;
    constexpr double pi=3.14159265358979323846;
    TxChannel tx(150000);
    uint64_t index=0,out_index=0;
    auto add=[&] {
        std::vector<int16_t> x(240);
        for(size_t k=0;k<x.size();++k) x[k]=int16_t(std::lround(16000*std::cos(2*pi*1000*(index+k)/12000)));
        require(tx.add(index,x),"Audio rejected");index+=240;
    };
    for(int i=0;i<6;++i) add();
    std::complex<double> upper{},lower{},carrier{},image{};
    uint64_t count=0;
    for(int block=0;block<160;++block) {
        if(block) add();
        auto y=tx.render(true);
        require(y.size()==24000,"Wrong IQ block size");
        if(block>20) for(size_t i=0;i<y.size();++i) {
            const double t=double(out_index+i)/1200000;
            upper+=std::complex<double>(y[i])*std::polar(1.,-2*pi*151000*t);
            lower+=std::complex<double>(y[i])*std::polar(1.,-2*pi*149000*t);
            carrier+=std::complex<double>(y[i])*std::polar(1.,-2*pi*150000*t);
            image+=std::complex<double>(y[i])*std::polar(1.,-2*pi*139000*t);
            ++count;
        }
        out_index+=y.size();
    }
    const auto a=std::abs(upper)/count,b=std::abs(lower)/count;
    std::cout << "USB amplitude="<<a<<"; LSB rejection="<<20*std::log10(a/std::max(b,1e-15))<<" dB; FIFO="<<tx.level()<<"; ppm="<<tx.ppm()<<std::endl;
    require(a>.23 && a<.26,"USB level incorrect");
    require(b<a*.001,"Lower sideband not rejected by 60 dB");
    require(std::abs(carrier)<std::abs(upper)*.001,"Carrier leakage");
    require(std::abs(image)<std::abs(upper)*.001,"Interpolation image");
    require(tx.underruns==0 && tx.overruns==0,"Steady clock underrun");
    auto silent=tx.render(false);
    require(std::all_of(silent.begin(),silent.end(),[](auto x){return x==std::complex<float>{};}),"Missing audio is not exact silence");
    require(tx.render_active(false).empty(),"Inactive TX must expose known-silent IQ");
    TxChannel waking(-150000,{},TxChannel::Profile::digital);
    require(waking.render_active(true).empty(),"Empty FIFO was modulated");
    for(int i=0;i<6;++i)require(waking.add(i*240,std::vector<int16_t>(240)),"Silent client packet rejected");
    require(waking.render_active(true).size()==TxChannel::iq_block,"Connected silent client was put to sleep");
    // Five virtual minutes with both positive and negative soundcard drift.
    // Exercise the real sinc clock correction without the expensive IQ stage.
    for(double drift:{.0015,-.0015}) {
        tx.reset(150000);index=0;
        for(int i=0;i<6;++i) add();
        double arrival=0;
        for(int block=0;block<15000;++block) {
            arrival+=1+drift;
            while(arrival>=1) { add();arrival-=1; }
            auto a=tx.pull_audio(true);
            require(a.size()==240,"Adaptive audio starved");
        }
        std::cout << "Drift=" << drift*1e6 << " ppm; correction=" << tx.ppm() << "; FIFO=" << tx.level() << '\n';
        require(tx.underruns==0 && tx.overruns==0,"Clock drift caused FIFO failure");
        require(std::abs(tx.ppm()-drift*1e6)<600,"Adaptive resampler did not track soundcard drift");
        require(tx.level()>700 && tx.level()<2100,"FIFO did not stay near midpoint");
    }
    tx.reset(-150000);
    require(tx.level()==0,"Retune did not clear FIFO");
    require(!tx.add(1,std::vector<int16_t>(240)),"Misaligned audio accepted");
    require(tx.add(0,std::vector<int16_t>(240)),"Valid packet rejected");
    require(!tx.add(0,std::vector<int16_t>(240)),"Duplicate accepted");
    for(int i=1;i<20;++i) tx.add(i*240,std::vector<int16_t>(240));
    require(tx.level()<=TxChannel::capacity && tx.overruns>0,"Unbounded FIFO");
}
