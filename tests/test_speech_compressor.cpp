#include "qo100/speech_compressor.hpp"
#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
constexpr double pi=3.14159265358979323846;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
double rms_gain(unsigned amount,float amplitude) {
    qo100::SpeechCompressor compressor({amount});
    double input=0,output=0;
    for(unsigned i=0;i<24000;++i) {
        const float x=amplitude*std::sin(2*pi*1000*i/12000);
        const auto y=compressor.execute(x);
        if(i>12000) { input+=x*x;output+=y*y; }
    }
    return 10*std::log10(output/input);
}
void dynamics() {
    for(unsigned amount:{0,600,1000,1400}) {
        const auto quiet=rms_gain(amount,.02f);
        require(std::abs(quiet-18*amount/1000.)<.02,"Quiet speech not raised by the selected amount");
        const auto loud=rms_gain(amount,.7f);
        require(loud<quiet || !amount,"Loud signals were not compressed");
        std::cout<<amount/10.<<" %: quiet gain="<<quiet<<" dB, loud gain="<<loud<<" dB\n";
    }
    qo100::SpeechCompressor compressor;
    const double input_span=20*std::log10(.7/.2);
    const double output_span=input_span+rms_gain(1000,.7f)-rms_gain(1000,.2f);
    require(std::abs(input_span/output_span-4)<.05,"Strong compression does not achieve 4:1 steady-state dynamics");
    const double stronger_span=input_span+rms_gain(1400,.7f)-rms_gain(1400,.2f);
    require(stronger_span>0 && std::abs(input_span/stronger_span-8.8)<.15,"140% has inverted dynamics or incorrect extended ratio");
    for(unsigned i=0;i<24000;++i) {
        const float x=.9f*std::sin(2*pi*390*i/12000);
        require(compressor.execute(x)==x,"Bypass changes mono samples");
    }
    compressor.set({1000});
    for(unsigned i=0;i<3000;++i) compressor.execute(.5f);
    compressor.set({0});
    for(unsigned i=0;i<240;++i) compressor.execute(.5f);
    require(compressor.execute(.5f)==.5f,"Bypass failed to settle within 20 ms");
    compressor.set({500});compressor.reset();
    for(int i=0;i<12000;++i) require(compressor.execute(0)==0,"Silence generated noise");
    require(compressor.settings().amount==500,"Reset lost compressor setting");
    bool rejected=false;try { compressor.set({1401}); } catch(const std::exception&) { rejected=true; }
    require(rejected,"Invalid compressor amount accepted");
}
void limiter() {
    using C=std::complex<float>;
    qo100::TxPeakLimiter limiter;
    for(size_t i=0;i<30000;++i) {
        C x{.1f,-.2f};const auto y=limiter.execute(x);
        require(y==(i<limiter.lookahead?C{}:x),"Limiter modifies safe levels or has wrong delay");
    }
    limiter.reset();
    // Isolated impulse, abrupt bursts, high-crest multitone and return to silence.
    double peak=0;
    for(size_t i=0;i<400000;++i) {
        C x{};
        if(i==12345) x={12,-8};
        if(i>50000 && i<300000) x=std::polar(float(2+1.9*std::sin(.002*i)),float(.1*i));
        const auto y=limiter.execute(x);
        peak=std::max(peak,double(std::abs(y)));
        require(std::isfinite(y.real()) && std::isfinite(y.imag()) && std::abs(y)<=.500001,"SSB envelope exceeds peak ceiling");
    }
    require(peak>.49,"Limiter needlessly reduces the allowed peak");
    limiter.reset();
    for(size_t i=0;i<2*limiter.lookahead;++i) require(limiter.execute({})==C{},"Limiter reset leaks old audio");
}
void chain(unsigned amount) {
    qo100::TxChannel tx(150000);
    uint64_t index=0;
    auto add=[&](float level) {
        std::vector<int16_t> audio(240);
        for(size_t k=0;k<audio.size();++k) audio[k]=int16_t(32767*level*std::sin(2*pi*1000*(index+k)/12000));
        require(tx.add(index,audio),"TX audio rejected");index+=240;
    };
    for(int i=0;i<6;++i) add(.03f);
    double bypass=0,compressed=0;std::complex<double> usb{},lsb{};size_t count=0;
    for(unsigned block=0;block<120;++block) {
        if(block==40) tx.set_compressor({amount});
        if(block) add(block<90?.03f:.95f);
        const auto iq=tx.render(true);
        for(size_t k=0;k<iq.size();++k) {
            require(std::abs(iq[k])<=.500002,"Interpolated/mixed TX IQ exceeds peak ceiling");
            if(block>=20 && block<40) bypass+=std::norm(iq[k]);
            if(block>=70 && block<90) {
                compressed+=std::norm(iq[k]);
                double t=(uint64_t(block)*iq.size()+k)/1200000.;
                usb+=std::complex<double>(iq[k])*std::polar(1.,-2*pi*151000*t);
                lsb+=std::complex<double>(iq[k])*std::polar(1.,-2*pi*149000*t);++count;
            }
        }
    }
    const auto gain=10*std::log10(compressed/bypass);
    std::cout<<"Full TX chain: quiet gain="<<gain<<" dB; USB="<<std::abs(usb)/count
             <<"; LSB rejection="<<20*std::log10(std::abs(usb)/std::abs(lsb))<<" dB\n";
    require(std::abs(gain-18*amount/1000.)<.5,"TX speech gain incorrect");
    require(std::abs(lsb)<std::abs(usb)*.001,"Compressor degraded SSB rejection below 60 dB");
    require(tx.underruns==0 && tx.overruns==0,"Compression interrupted audio FIFO");
    const auto silence=tx.render(false);
    require(std::all_of(silence.begin(),silence.end(),[](auto x){return x==std::complex<float>{};}),"Missing UDP audio not exact silence");
    tx.reset(-150000);
    require(tx.compressor_settings().amount==amount,"Retune lost compressor amount");
    // A future second modulator starts with its own unprocessed audio.
    qo100::TxChannel other;
    require(other.compressor_settings().amount==0,"Compressor leaked to another modulator");
}
}
int main() {
    try { dynamics();limiter();chain(1000);chain(1400); }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
