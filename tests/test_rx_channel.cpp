#include "qo100/rx_channel.hpp"
#include <cmath>
#include <complex>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    try {
        constexpr double pi=3.14159265358979323846;
        std::vector<float> audio;
        std::vector<float> bypass_audio;
        size_t zoom_samples=0;
        qo100::RxChannel rx(-1500,[&](auto){++zoom_samples;},[&](const auto& p,uint64_t index){
            if(index!=audio.size()) throw std::runtime_error("Audio counter discontinuity");
            for(auto x:p) audio.push_back(x/32768.f);
        });
        qo100::RxChannel nr_bypass(-1500,[](auto){},[&](const auto& p,uint64_t){
            for(auto x:p) bypass_audio.push_back(x/32768.f);
        },{},0);
        for(int i=0;i<600000;++i) {
            const double t=i/600000.;
            auto tone=[&](float a,double f){return std::polar(a,float(std::remainder(2*pi*f*t,2*pi)));};
            const auto sample=tone(.25f,-500)+tone(.4f,-3500)+tone(.2f,74500);
            rx.push(sample);nr_bypass.push(sample);
        }
        if(zoom_samples!=75000 || audio.size()<11000 || audio.size()>12000) throw std::runtime_error("Incorrect zoom/audio rate");
        if(audio!=bypass_audio) throw std::runtime_error("NR bypass changed demodulated PCM or sample count");
        auto amplitude=[&](double hz) {
            std::complex<double> sum{};
            for(size_t i=3000;i<audio.size();++i) sum+=double(audio[i])*std::polar(1.,-2*pi*hz*i/12000);
            return 2*std::abs(sum)/double(audio.size()-3000);
        };
        const auto upper=amplitude(1000), lower=amplitude(2000);
        if(std::abs(upper-.25)>.005) throw std::runtime_error("USB amplitude or anti-alias rejection incorrect");
        if(lower>.00025) throw std::runtime_error("Lower sideband rejection is less than 60 dB");
        rx.tune(1500); // reset all histories on retune
        bool rejected=false;
        try { rx.tune(275001); } catch(const std::exception&) {rejected=true;}
        if(!rejected) throw std::runtime_error("Out-of-band tuning accepted");
        std::cout<<"USB 1 kHz amplitude="<<upper<<"; unwanted 2 kHz amplitude="<<lower
                 <<"; LSB rejection="<<20*std::log10(upper/std::max(lower,1e-12))<<" dB\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
