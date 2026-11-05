#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace qo100 {
namespace {
constexpr double pi = 3.14159265358979323846;
void process(SRC_STATE* state, SRC_DATA& data) {
    const int error = src_process(state, &data);
    if (error) throw std::runtime_error(src_strerror(error));
}
}
TxChannel::TxChannel(int64_t offset) : ring_(513), taps_(513) {
    // Analytic bandpass +100..4000 Hz. Multiplication by two restores the
    // amplitude of the positive-frequency half of real microphone audio.
    const double beta=8.6, denom=std::cyl_bessel_i(0.,beta);
    double sum=0;
    for(size_t i=0;i<taps_.size();++i) {
        const double t=double(i)-256, x=t/256;
        const double h=(t==0?3900./12000:std::sin(pi*3900*t/12000)/(pi*t)) *
            std::cyl_bessel_i(0.,beta*std::sqrt(std::max(0.,1-x*x)))/denom;
        taps_[i]=float(h)*std::polar(1.f,float(2*pi*2050*t/12000));
        sum+=h;
    }
    for(auto& h:taps_) h*=float(2/sum);
    int error=0;
    clock_=src_new(SRC_SINC_FASTEST,1,&error);
    if(!clock_) throw std::runtime_error(src_strerror(error));
    interpolator_=src_new(SRC_SINC_FASTEST,2,&error);
    if(!interpolator_) { src_delete(clock_); throw std::runtime_error(src_strerror(error)); }
    reset(offset);
}
TxChannel::~TxChannel() { src_delete(clock_); src_delete(interpolator_); }
void TxChannel::clear_dsp() {
    pending_.clear(); std::fill(ring_.begin(),ring_.end(),0.f); pos_=0;
    src_reset(clock_); src_reset(interpolator_);
    // Prime both sinc histories so every render returns exactly 20 ms.
    std::vector<float> zeros(1024), out(120000);
    SRC_DATA d{}; d.data_in=zeros.data(); d.data_out=out.data();
    d.input_frames=512;d.output_frames=60000;d.src_ratio=1;
    process(clock_,d);
    d={};d.data_in=zeros.data();d.data_out=out.data();
    d.input_frames=512;d.output_frames=60000;d.src_ratio=100;
    process(interpolator_,d);
    started_=false; correction_=integral_=smoothed_=clock_buffered_=0;
}
void TxChannel::reset(int64_t offset) {
    if(std::abs(offset)>275000) throw std::runtime_error("TX frequency outside 600 kHz window");
    packets_.clear(); next_=0;have_next_=false;
    oscillator_={1,0};step_=std::polar(1.,2*pi*double(offset)/1200000);
    clear_dsp();
}
size_t TxChannel::level() const { return packets_.size()*audio_block+pending_.size()+size_t(std::max(0.,clock_buffered_)); }
bool TxChannel::add(uint64_t index,const std::vector<int16_t>& samples) {
    if(samples.size()!=audio_block || index%audio_block || index>std::numeric_limits<uint64_t>::max()-audio_block ||
       (have_next_ && index<next_) || packets_.count(index)) return false;
    packets_.emplace(index,samples);
    if(level()>capacity) {
        // Recover to recent sound instead of building an unbounded TX delay.
        while(packets_.size()>target/audio_block) packets_.erase(packets_.begin());
        clear_dsp();have_next_=false;++overruns;
    }
    return true;
}
std::vector<float> TxChannel::pull_audio(bool recent) {
    std::vector<float> result;
    if(!recent) {
        packets_.clear();
        if(started_ || !pending_.empty()) clear_dsp();
        return result;
    }
    if(!started_) {
        if(level()<target) return result;
        next_=packets_.begin()->first;have_next_=true;started_=true;
    }
    const double error=(double(level())-target)/target;
    smoothed_+=.02*(error-smoothed_);
    integral_=std::clamp(integral_+smoothed_*.000004,-.003,.003);
    correction_=std::clamp(.004*smoothed_+integral_,-.005,.005);
    // Positive correction consumes more input when the FIFO is too full.
    while(pending_.size()<audio_block+64) {
        auto it=packets_.find(next_);
        if(it!=packets_.end()) {
            for(auto sample:it->second) pending_.push_back(sample/32768.f);
            packets_.erase(it);
        } else if(!packets_.empty() && packets_.begin()->first>next_) {
            pending_.insert(pending_.end(),audio_block,0.f); ++losses;
        } else break;
        next_+=audio_block;
    }
    if(pending_.size()<audio_block+8) {
        ++underruns;clear_dsp();return result;
    }
    std::vector<float> audio(audio_block);
    SRC_DATA d{};d.data_in=pending_.data();d.data_out=audio.data();
    d.input_frames=static_cast<long>(pending_.size());d.output_frames=audio_block;d.src_ratio=1/(1+correction_);
    process(clock_,d);
    clock_buffered_+=d.input_frames_used-d.output_frames_gen*(1+correction_);
    pending_.erase(pending_.begin(),pending_.begin()+d.input_frames_used);
    if(d.output_frames_gen!=audio_block) { ++underruns;clear_dsp();return result; }
    return audio;
}
std::vector<std::complex<float>> TxChannel::render(bool recent) {
    const auto audio=pull_audio(recent);
    std::vector<std::complex<float>> result(iq_block);
    if(audio.empty()) return result;
    std::vector<float> analytic(audio_block*2), up(iq_block*2);
    for(size_t i=0;i<audio_block;++i) {
        ring_[pos_]=audio[i];pos_=(pos_+1)%ring_.size();
        std::complex<float> value{};size_t p=pos_;
        for(size_t k=0;k<taps_.size();++k) {
            p=p?p-1:ring_.size()-1;value+=ring_[p]*taps_[k];
        }
        analytic[2*i]=value.real();analytic[2*i+1]=value.imag();
    }
    SRC_DATA d{};d.data_in=analytic.data();d.data_out=up.data();
    d.input_frames=audio_block;d.output_frames=iq_block;d.src_ratio=100;
    process(interpolator_,d);
    if(d.output_frames_gen!=iq_block || d.input_frames_used!=audio_block)
        throw std::runtime_error("TX interpolator did not produce a full block");
    for(size_t i=0;i<iq_block;++i) {
        result[i]=std::complex<float>(up[2*i],up[2*i+1])*std::complex<float>(oscillator_)*.5f;
        oscillator_*=step_;
        if((i&4095)==4095) oscillator_/=std::abs(oscillator_);
    }
    return result;
}
}
