#include "qo100/noise_reduction.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

int main() {
    try {
        constexpr size_t rate=12000, n=16*rate;
        constexpr double pi=3.141592653589793;
        std::mt19937 random(78129);
        std::normal_distribution<double> gaussian(0,1700);
        std::vector<int16_t> input(n),clean(n),dry,medium,strong;
        for(size_t i=0;i<n;++i) {
            const double t=double(i)/rate;
            double voice=0;
            if(t>=6 && std::fmod(t-6,.65)<.4) {
                const double envelope=std::pow(std::sin(pi*std::fmod(t-6,.65)/.4),2);
                for(int k=1;k<=18;++k)
                    voice+=3400*envelope*std::sin(k*(2*pi*155*t+1.4*std::sin(2*pi*2.7*t)))/k;
            }
            clean[i]=int16_t(std::lround(voice));
            input[i]=int16_t(std::lround(std::clamp(voice+gaussian(random),-32768.,32767.)));
        }
        const auto start=std::chrono::steady_clock::now();
        auto run=[&](unsigned percent) {
            qo100::NoiseReduction nr(percent);
            std::vector<int16_t> result;
            for(size_t i=0;i<n;i+=240) {
                std::vector<int16_t> packet(input.begin()+i,input.begin()+i+240);
                nr.process(packet);result.insert(result.end(),packet.begin(),packet.end());
            }
            return result;
        };
        dry=run(0);medium=run(50);strong=run(100);
        if(dry!=input) throw std::runtime_error("NR=0 changed PCM samples");
        auto energy=[&](const auto& v,size_t a,size_t b) {
            double sum=0;for(size_t i=a;i<b;++i) sum+=double(v[i])*v[i];return sum/(b-a);
        };
        const double mid_db=10*std::log10(energy(input,4*rate,6*rate)/energy(medium,4*rate,6*rate));
        const double high_db=10*std::log10(energy(input,4*rate,6*rate)/energy(strong,4*rate,6*rate));
        if(mid_db<7 || high_db<15 || high_db<mid_db+4)
            throw std::runtime_error("Insufficient or non-monotonic stationary-noise attenuation");
        double projection=0,reference=0;
        for(size_t i=7*rate;i<n;++i) {
            const double c=clean[i-240];
            projection+=c*strong[i];reference+=c*c;
        }
        const double voice_gain=projection/reference;
        if(voice_gain<.35 || voice_gain>1.2) throw std::runtime_error("Voiced signal lost or amplified by NR");
        qo100::NoiseReduction change(100);
        std::vector<int16_t> packet(input.begin(),input.begin()+240);
        change.process(packet);change.set(0);
        change.process(packet);
        packet.assign(input.begin(),input.begin()+240);
        const auto original=packet;change.process(packet);
        if(packet!=original) throw std::runtime_error("Bypass did not settle within one packet");
        change.set(100);change.reset();
        std::fill(packet.begin(),packet.end(),0);change.process(packet);
        if(std::any_of(packet.begin(),packet.end(),[](auto x){return x!=0;}))
            throw std::runtime_error("Retune reset leaked old audio");
        bool rejected=false;
        try {change.set(101);} catch(const std::exception&) {rejected=true;}
        if(!rejected || change.percent()!=100) throw std::runtime_error("Invalid NR setting applied");
        const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        std::cout << "SpeexDSP: stationary noise attenuation 50%=" << mid_db << " dB, 100%=" << high_db
                  << " dB; voiced projection gain=" << voice_gain << "; 48 s audio processed in " << elapsed
                  << " s; PCM bypass and reset verified\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
