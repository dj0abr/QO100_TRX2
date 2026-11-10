// SPDX-License-Identifier: GPL-3.0-only
#include "beacon_dsp.hpp"
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace qo100::beacon {
namespace {
using Complex=std::complex<float>;
constexpr double pi=3.14159265358979323846;
constexpr const char* sync="11111110000111011110010110010010000001000100110001011101011011000";
constexpr size_t taps=65;
// Near the bottom of the audio passband, the real-input image overlaps the
// desired baseband. Use an analytic input only for this special dial range.
std::vector<Complex> analytic_signal(const std::vector<int16_t>& pcm,float peak) {
    const auto n=pcm.size();auto* p=fftwf_alloc_complex(n);
    if(!p)throw std::bad_alloc();
    const auto free=[](fftwf_complex* x){fftwf_free(x);};
    std::unique_ptr<fftwf_complex,decltype(free)> memory(p,free);
    const auto forward=fftwf_plan_dft_1d(int(n),p,p,FFTW_FORWARD,FFTW_ESTIMATE);
    const auto backward=fftwf_plan_dft_1d(int(n),p,p,FFTW_BACKWARD,FFTW_ESTIMATE);
    if(!forward || !backward) {
        if(forward)fftwf_destroy_plan(forward);
        if(backward)fftwf_destroy_plan(backward);
        throw std::runtime_error("Cannot create low-carrier analytic FFT");
    }
    for(size_t i=0;i<n;++i){p[i][0]=float(pcm[i])/peak;p[i][1]=0;}
    fftwf_execute(forward);
    for(size_t i=1;i<n;++i) {
        const float gain=i>n/2?0:((n%2==0 && i==n/2)?1:2);
        p[i][0]*=gain;p[i][1]*=gain;
    }
    fftwf_execute(backward);fftwf_destroy_plan(forward);fftwf_destroy_plan(backward);
    std::vector<Complex> result;result.reserve(n);
    for(size_t i=0;i<n;++i)result.emplace_back(p[i][0]/float(n),p[i][1]/float(n));
    return result;
}
// Real audio is mixed and FIR-decimated directly, normally avoiding a full-rate Hilbert
// FFT and two full-rate forward/backward 301-tap filters per overlapping window.
std::vector<Complex> baseband(const std::vector<int16_t>& pcm,unsigned rate,double carrier,float peak) {
    const unsigned decimation=rate/4000;
    const auto analytic=carrier<800?analytic_signal(pcm,peak):std::vector<Complex>{};
    std::array<float,taps> coefficients{};
    float sum=0;
    for(size_t i=0;i<taps;++i) {
        const double t=double(i)-double(taps-1)/2;
        const double sinc=t==0?2*900./rate:std::sin(2*pi*900*t/rate)/(pi*t);
        coefficients[i]=float(sinc*(.54-.46*std::cos(2*pi*i/(taps-1))));sum+=coefficients[i];
    }
    for(auto& x:coefficients)x/=sum;
    std::array<Complex,taps> history{};
    std::vector<Complex> out;out.reserve(pcm.size()/decimation+1);
    Complex phase{1,0};const auto step=std::polar(1.f,float(-2*pi*carrier/rate));
    size_t pos=0;
    for(size_t i=0;i<pcm.size();++i) {
        history[pos]=phase*(analytic.empty()?Complex{float(pcm[i])/peak,0}:analytic[i]);phase*=step;
        if(i%4096==4095)phase/=std::abs(phase);
        if(i%decimation==0) {
            Complex y{};size_t j=pos;
            for(size_t k=0;k<taps;++k) {y+=coefficients[k]*history[j];j=j?j-1:taps-1;}
            out.push_back(y);
        }
        if(++pos==taps)pos=0;
    }
    return out;
}
double carrier_offset(const std::vector<Complex>& bb,double expected) {
    size_t n=1;while(n<bb.size())n*=2;
    auto* values=fftwf_alloc_complex(n);
    if(!values)throw std::bad_alloc();
    const auto cleanup=[](fftwf_complex* p){fftwf_free(p);};
    std::unique_ptr<fftwf_complex,decltype(cleanup)> storage(values,cleanup);
    for(size_t i=0;i<n;++i) {
        const Complex x=i<bb.size()?bb[i]*bb[i]*float(.5-.5*std::cos(2*pi*i/(bb.size()-1))):Complex{};
        values[i][0]=x.real();values[i][1]=x.imag();
    }
    const auto plan=fftwf_plan_dft_1d(int(n),values,values,FFTW_FORWARD,FFTW_ESTIMATE);
    if(!plan)throw std::runtime_error("Cannot create beacon carrier FFT");
    fftwf_execute(plan);fftwf_destroy_plan(plan);
    auto power=[&](size_t i){return double(values[i][0])*values[i][0]+double(values[i][1])*values[i][1];};
    size_t best=0;double best_power=-1;
    for(size_t i=0;i<n;++i) {
        const double hz=double(i<=n/2?int64_t(i):int64_t(i)-int64_t(n))*4000/n;
        // Real-input mixing also produces an image whose cross term would
        // suggest a zero/negative audio carrier when the dial is near 400 Hz.
        const double carrier=expected+hz/2;
        if(std::abs(hz)<1000 && carrier>100 && carrier<3800 && power(i)>best_power) {
            best=i;best_power=power(i);
        }
    }
    const double a=std::log(std::max(1e-30,power((best+n-1)%n)));
    const double b=std::log(std::max(1e-30,power(best)));
    const double c=std::log(std::max(1e-30,power((best+1)%n)));
    const double denominator=a-2*b+c;
    const double fraction=std::abs(denominator)>1e-12?std::clamp(.5*(a-c)/denominator,-.5,.5):0;
    return (double(best<=n/2?int64_t(best):int64_t(best)-int64_t(n))+fraction)*4000/n/2;
}
void smooth(std::vector<Complex>& values) {
    const float alpha=float(1-std::exp(-2*pi*20/4000));
    // Two inexpensive poles in both directions replace the full-rate 501-tap
    // phase FIR. Filtering the squared signal removes Manchester/BPSK signs.
    for(unsigned pass=0;pass<2;++pass) {
        Complex y=values.front();
        for(auto& x:values){y+=alpha*(x-y);x=y;}
        y=values.back();
        for(auto it=values.rbegin();it!=values.rend();++it){y+=alpha*(*it-y);*it=y;}
    }
}
void matched(std::vector<float>& values) {
    // Same triangular matched response as a five-sample box run both ways.
    std::vector<float> output(values.size());
    for(size_t i=0;i<values.size();++i) {
        float value=0;
        for(int j=-4;j<=4;++j) {
            const auto k=std::clamp<int64_t>(int64_t(i)+j,0,int64_t(values.size()-1));
            value+=float(5-std::abs(j))*values[size_t(k)];
        }
        output[i]=value/25;
    }
    values.swap(output);
}
std::vector<size_t> timing(const std::vector<float>& values) {
    double best=-1,best_step=5;unsigned best_phase=0;
    for(int ppm=-100;ppm<=100;ppm+=20) {
        const double step=5*(1+ppm*1e-6);
        for(unsigned phase=0;phase<5;++phase) {
            double quality=0;size_t count=0;
            for(size_t k=0;;++k) {
                const auto i=size_t(std::nearbyint(phase+k*step));
                if(i>=values.size()-1)break;
                quality+=std::abs(values[i]);++count;
            }
            quality/=double(count);
            if(quality>best) {best=quality;best_step=step;best_phase=phase;}
        }
    }
    std::vector<size_t> result;result.reserve(values.size()/5+1);
    for(size_t k=0;;++k) {
        const auto i=size_t(std::nearbyint(best_phase+k*best_step));
        if(i>=values.size()-1)break;
        result.push_back(i);
    }
    return result;
}
}
Result decode(const std::vector<int16_t>& pcm,unsigned rate,uint64_t rx_hz) {
    if(rate!=8000 && rate!=12000)throw std::runtime_error("Beacon requires PCM16 at 8000 or 12000 Hz");
    Result result;
    const double expected=10489750000.-double(rx_hz);
    float peak=0;for(auto x:pcm)peak=std::max(peak,std::abs(float(x)));
    if(pcm.size()<14*rate || peak<1 || expected<400 || expected>3300)return result;
    auto bb=baseband(pcm,rate,expected,peak);
    const double offset=carrier_offset(bb,expected);result.carrier=expected+offset;
    Complex phase{1,0};const auto step=std::polar(1.f,float(-2*pi*offset/4000));
    std::vector<Complex> squared(bb.size());
    for(size_t i=0;i<bb.size();++i) {
        bb[i]*=phase;phase*=step;if(i%4096==4095)phase/=std::abs(phase);
        squared[i]=bb[i]*bb[i];
    }
    smooth(squared);
    std::vector<float> values(bb.size());
    double previous=std::arg(squared.front()),unwrapped=previous;
    for(size_t i=0;i<values.size();++i) {
        const double angle=std::arg(squared[i]);unwrapped+=std::remainder(angle-previous,2*pi);previous=angle;
        values[i]=(bb[i]*std::polar(1.f,float(-.5*unwrapped))).real();
    }
    matched(values);const auto indices=timing(values);
    auto position=[&](size_t bit,unsigned alignment) {
        const size_t chip=std::min(2*(bit+1)+alignment,indices.size()-1);
        const auto sample=int64_t(indices[chip]*(rate/4000))-int64_t((taps-1)/2);
        return uint64_t(std::max<int64_t>(0,sample));
    };
    for(unsigned alignment=0;alignment<2;++alignment) {
        std::vector<uint8_t> bits;
        for(size_t i=alignment+2;i<indices.size();i+=2)
            bits.push_back(uint8_t((values[indices[i]]>0)!=(values[indices[i-2]]>0)));
        uint32_t shift=0;
        for(size_t i=0;i<bits.size();++i) {
            shift=(shift<<1)|bits[i];
            if(i<31 || shift!=0x3915ed30 || i+1+514*8>bits.size())continue;
            std::vector<uint8_t> bytes(514,0);
            for(size_t j=0;j<bytes.size()*8;++j)bytes[j/8]|=uint8_t(bits[i+1+j]<<(7-j%8));
            if(crc16(bytes)==0) {
                bytes.resize(512);result.frames.push_back({position(i-31,alignment),false,std::move(bytes),0});
            }
        }
        for(unsigned polarity=0;polarity<2;++polarity) {
            size_t next=0;
            for(size_t i=0;i+5200<=bits.size();++i) {
                if(i<next)continue;
                unsigned errors=0;
                for(size_t j=0;j<65 && errors<=8;++j)errors+=((bits[i+80*j]^polarity)!=unsigned(sync[j]-'0'));
                if(errors>8)continue;
                next=i+2600;
                std::vector<uint8_t> frame(5200),payload;int corrected=0;
                for(size_t j=0;j<frame.size();++j)frame[j]=bits[i+j]^polarity;
                if(fec_frame(frame,payload,corrected))
                    result.frames.push_back({position(i,alignment),true,std::move(payload),corrected});
            }
        }
    }
    std::sort(result.frames.begin(),result.frames.end(),[](const auto& a,const auto& b){return a.sample<b.sample;});
    std::vector<Frame> unique;
    for(auto& frame:result.frames) {
        const bool duplicate=std::any_of(unique.begin(),unique.end(),[&](const auto& a) {
            return a.fec==frame.fec && a.payload==frame.payload &&
                std::abs(int64_t(a.sample)-int64_t(frame.sample))<rate/20;
        });
        if(!duplicate)unique.push_back(std::move(frame));
    }
    result.frames=std::move(unique);return result;
}
std::string format(const Frame& frame,double carrier) {
    std::string display;
    for(auto byte:frame.payload) {const auto c=byte&127;display+=char(c>=32 && c<127?c:' ');}
    std::ostringstream out;
    out<<"[PSK "<<(display.empty()?'?':display[0])<<" | ";
    if(frame.fec)out<<"FEC/RS OK, corrected "<<frame.corrections;else out<<"CRC OK";
    out<<" | carrier "<<std::fixed<<std::setprecision(1)<<carrier<<" Hz]\n";
    for(size_t first=0;first<display.size();first+=64) {
        auto line=display.substr(first,64);const auto end=line.find_last_not_of(' ');
        out<<(end==std::string::npos?std::string{}:line.substr(0,end+1))<<'\n';
    }
    return out.str();
}
}
