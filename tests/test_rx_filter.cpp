#include "qo100/rx_filter.hpp"
#include "qo100/rx_channel.hpp"
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
using C=std::complex<float>;constexpr double pi=3.14159265358979323846;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
C tone(float amplitude,double hz,uint64_t index,double rate) {
    return std::polar(amplitude,float(std::remainder(2*pi*hz*index/rate,2*pi)));
}
void curves() {
    for(qo100::RxFilterSettings settings: {qo100::RxFilterSettings{500,700}, {1200,1100}, {1800,1300}, {2700,1650}, {3900,2050}, {501,700}, qo100::RxFilterSettings::from_edges(301,2700), qo100::RxFilterSettings::from_edges(700.5,1700)}) {
        auto f=qo100::make_usb_filter(1,settings);
        auto response=[&](double hz) { C y;qo100::check_liquid(firfilt_cccf_freqresponse(f.get(),hz/12000,&y));return std::abs(y); };
        require(std::abs(response(settings.center_hz)-1)<.005,"Incorrect filter center gain");
        require(std::abs(response(settings.low_hz())-.5)<.02 && std::abs(response(settings.high_hz())-.5)<.02,"Filter bandwidth/position is incorrect");
        require(response(settings.low_hz()-150)<.0001 && response(settings.high_hz()+150)<.0001,"Insufficient filter stopband rejection");
    }
    for(qo100::RxFilterSettings bad:{qo100::RxFilterSettings{499,700},{3901,2050},{500,349},{500,3751},{UINT32_MAX,700},{500,UINT32_MAX},{500,NAN},{NAN,1000},{500,INFINITY}})
        require(!bad.valid(),"Out-of-range filter accepted");
}
void crossfade() {
    qo100::RxAudioFilter f;
    auto full=qo100::make_usb_filter(1), narrow=qo100::make_usb_filter(1,{500,700});
    double maximum_error=0;
    for(uint64_t i=0;i<4000;++i) {
        C x=tone(.2f,700,i,12000)+tone(.15f,1700,i,12000)+tone(.1f,3900,i,12000),a,b;
        qo100::check_liquid(firfilt_cccf_execute_one(full.get(),x,&a));
        qo100::check_liquid(firfilt_cccf_execute_one(narrow.get(),x,&b));
        if(i==2000) f.set({500,700});
        float mix=i<2000?0:std::min(1.f,float(i-2000+1)/240);
        const auto expected=a*(1-mix)+b*mix;
        maximum_error=std::max(maximum_error,double(std::abs(f.execute(x)-expected)));
    }
    require(maximum_error<2e-6,"Filter change lost history, phase alignment, or 20 ms fade");
    require(f.active()==qo100::RxFilterSettings{500,700},"Filter did not settle");
    for(unsigned i=0;i<1000;++i) {
        if(i%17==0) f.set(i%34?qo100::RxFilterSettings{500,700}:qo100::RxFilterSettings{1200,2000});
        require(std::abs(f.execute(tone(.2f,700,i+4000,12000)))<.6,"Rapid filter changes caused an output spike");
    }
    f.set({1800,1800});
    for(int i=0;i<600;++i) f.execute({});
    require(f.active()==qo100::RxFilterSettings{1800,1800},"Latest slider value was not applied");
    f.reset();
    for(int i=0;i<600;++i) require(f.execute({})==C{},"Retune leaked old filter history");
    std::cout<<"20 ms warm-history crossfade maximum error="<<maximum_error<<'\n';
}
double amplitude(const std::vector<float>& x,double hz) {
    std::complex<double> sum{};const size_t count=3000;
    for(size_t i=x.size()-count;i<x.size();++i) sum+=double(x[i])*std::polar(1.,-2*pi*hz*i/12000);
    return 2*std::abs(sum)/count;
}
void receivers() {
    std::vector<float> a,b;std::vector<C> za,zb;
    auto save=[](std::vector<float>& dest,const auto& samples,uint64_t index) {
        require(index==dest.size(),"Filter change restarted the audio timeline");
        for(auto value:samples) dest.push_back(value/32768.f);
    };
    qo100::RxChannel rx1(0,[&](C x){za.push_back(x);},[&](const auto& p,uint64_t i){save(a,p,i);});
    qo100::RxChannel rx2(0,[&](C x){zb.push_back(x);},[&](const auto& p,uint64_t i){save(b,p,i);});
    for(uint64_t i=0;i<600000;++i) {
        if(i==240000) rx1.set_filter({500,700});
        C x=tone(.2f,700,i,600000)+tone(.15f,1700,i,600000);
        rx1.push(x);rx2.push(x);
    }
    require(za==zb,"Audio filter changed the zoom IQ stream");
    require(std::abs(amplitude(a,700)-.2)<.003 && amplitude(a,1700)<.0003,"RX1 filter failed to select the desired tone");
    require(std::abs(amplitude(b,700)-.2)<.003 && std::abs(amplitude(b,1700)-.15)<.003,"RX1 filtering affected RX2");
    rx1.set_filter({});
    for(uint64_t i=600000;i<900000;++i) rx1.push(tone(.2f,700,i,600000)+tone(.15f,1700,i,600000));
    require(std::abs(amplitude(a,1700)-.15)<.003,"Full BW did not restore rejected audio");
    rx1.set_filter({500,700});rx1.tune(1500);
    require(rx1.filter_settings()==qo100::RxFilterSettings{500,700},"Retuning changed filter settings");
}
}
int main() {
    try { curves();crossfade();receivers(); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
