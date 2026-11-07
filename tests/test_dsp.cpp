#include "qo100/dsp.hpp"
#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fftw3.h>
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
constexpr double pi=3.14159265358979323846;
using Complex=std::complex<float>;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void mixers() {
    for(int old_offset:{-1379,1379}) for(int new_offset:{-1404,0,1404}) {
        qo100::FrequencyShifter mixer(600000), reference(600000);
        mixer.tune(old_offset);reference.tune(old_offset);
        for(int i=0;i<137;++i) {mixer.execute({1,0});reference.execute({1,0});}
        mixer.tune(new_offset,false);
        require(std::abs(mixer.execute({1,0})-reference.execute({1,0}))<1e-6,"Retuning changed oscillator phase");
    }
    for(unsigned rate:{600000U,1200000U}) for(int offset:{-275000,-150001,-1500,-1,0,1,1500,150001,275000}) {
        qo100::FrequencyShifter mixer(rate);
        mixer.tune(offset);
        const size_t n=65536;
        std::vector<Complex> samples(n,Complex(1,0)),single(n);
        mixer.execute_block(samples.data(),n);
        mixer.tune(offset);
        for(auto& x:single) x=mixer.execute({1,0});
        require(samples==single,"Mixer output depends on block boundaries");
        std::vector<std::complex<double>> residual(n);
        for(size_t i=0;i<n;++i)
            residual[i]=std::complex<double>(samples[i])*std::polar(1.,-2*pi*offset*i/rate);
        const double phase=std::arg(residual.back()/residual.front());
        const double frequency_error=phase*rate/(2*pi*(n-1));
        require(std::abs(frequency_error)<.05,"Mixer frequency error exceeds 0.05 Hz");
        double noise=0;
        for(size_t i=0;i<n;++i) {
            auto error=residual[i]*std::polar(1.,-phase*double(i)/(n-1))-std::complex<double>(1,0);
            noise+=std::norm(error);
        }
        const double rms=std::sqrt(noise/n);
        require(rms<5e-6,"Mixer interpolation noise exceeds -106 dBc");
        std::cout<<"Mixer "<<rate<<" / "<<offset<<" Hz: error="<<frequency_error<<" Hz; residual="<<20*std::log10(std::max(rms,1e-15))<<" dBc\n";
    }
}
void filters() {
    auto filter=qo100::make_usb_filter(1);
    for(float hz:{200.f,1000.f,3000.f,3900.f}) {
        Complex value;
        qo100::check_liquid(firfilt_cccf_freqresponse(filter.get(),hz/12000,&value));
        require(std::abs(std::abs(value)-1)<.005,"USB passband ripple exceeds 0.5 percent");
    }
    for(float hz:{100.f,4000.f}) {
        Complex value;
        qo100::check_liquid(firfilt_cccf_freqresponse(filter.get(),hz/12000,&value));
        require(std::abs(std::abs(value)-.5)<.02,"USB passband edge moved");
    }
    for(float hz:{-3900.f,-1000.f,-100.f,4100.f,5000.f}) {
        Complex value;
        qo100::check_liquid(firfilt_cccf_freqresponse(filter.get(),hz/12000,&value));
        require(std::abs(value)<.0001,"USB stopband rejection is below 80 dB");
    }
}
void tx_spectrum() {
    constexpr int carrier=-273457;
    // Measure the actual interpolated/mixed IQ, including the compressor.
    // RX's full bandwidth remains 100..4000 Hz; TX ends at 3000 Hz.
    for(unsigned amount:{0U,1000U,1400U}) for(int frequency:{200,1000,2000,2800,3000,3100,3500,3900}) {
        if(amount && frequency<3100) continue; // In-band compression is tested separately.
        qo100::TxChannel tx(carrier,{amount});
        uint64_t index=0;
        auto add=[&] {
            std::vector<int16_t> pcm(240);
            for(size_t k=0;k<pcm.size();++k) pcm[k]=int16_t(std::lround(16000*std::cos(2*pi*frequency*(index+k)/12000)));
            require(tx.add(index,pcm),"TX test input rejected");index+=240;
        };
        for(int i=0;i<6;++i) add();
        std::vector<Complex> capture;
        for(int block=0;block<32;++block) {
            if(block) add();
            auto iq=tx.render(true);
            require(iq.size()==24000,"TX sample count changed");
            if(block>=30) capture.insert(capture.end(),iq.begin(),iq.end());
        }
        const size_t n=capture.size();
        auto* in=fftwf_alloc_complex(n);auto* out=fftwf_alloc_complex(n);
        require(in && out,"FFT allocation failed");
        auto plan=fftwf_plan_dft_1d(n,in,out,FFTW_FORWARD,FFTW_ESTIMATE);
        require(plan!=nullptr,"FFT plan failed");
        double coherent=0;
        for(size_t i=0;i<n;++i) {
            const float window=float(.5-.5*std::cos(2*pi*i/(n-1)));
            const auto value=capture[i]*std::polar(window,float(std::remainder(-2*pi*carrier*i/1200000,2*pi)));
            in[i][0]=value.real();in[i][1]=value.imag();coherent+=window;
        }
        fftwf_execute(plan);
        double peak=0,unwanted=0,spur_hz=0;
        for(size_t i=0;i<n;++i) {
            const double hz=(i<=n/2?double(i):double(i)-n)*1200000/n;
            const double magnitude=std::hypot(out[i][0],out[i][1]);
            if(std::abs(hz-frequency)<75) peak=std::max(peak,magnitude);
            // Exclude the finite-window skirts around the desired tone.
            if(std::abs(hz-frequency)>250 && magnitude>unwanted) { unwanted=magnitude;spur_hz=hz; }
        }
        const double level=peak/coherent;
        const double rejection=20*std::log10(peak/std::max(unwanted,1e-20));
        const double attenuation=-20*std::log10(std::max(level,1e-20)/(16000./32768*.5));
        std::cout<<"TX "<<frequency<<" Hz / compressor "<<amount/10.<<"%: amplitude="<<level
                 <<"; attenuation="<<attenuation<<" dB; strongest distant spur="<<-rejection<<" dBc at "<<spur_hz<<" Hz\n";
        if(frequency<3000) {
            require(level>.22 && level<.26,"TX audio passband or interpolation gain changed");
            require(rejection>90,"TX interpolation image or mixer spur exceeds -90 dBc");
        } else if(frequency==3000) {
            require(level>.10 && level<.14,"TX upper filter edge is not at 3 kHz");
        } else {
            require(attenuation>75,"TX audio above 3.1 kHz is not sufficiently suppressed");
        }
        require(tx.underruns==0 && tx.overruns==0,"TX bandwidth test interrupted audio FIFO");
        fftwf_destroy_plan(plan);fftwf_free(in);fftwf_free(out);
    }
}
}
int main() {
    try {
        require(std::strcmp(liquid_libversion(),"1.8.3")==0,"Wrong liquid runtime version");
        require(liquid_libversion_number()==LIQUID_VERSION_NUMBER,"Liquid header/library mismatch");
        mixers();filters();tx_spectrum();
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
