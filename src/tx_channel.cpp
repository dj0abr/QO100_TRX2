#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace qo100 {
namespace {
void process(SRC_STATE* state, SRC_DATA& data) {
    const int error = src_process(state, &data);
    if (error) throw std::runtime_error(src_strerror(error));
}
}
TxChannel::TxChannel(int64_t offset, CompressorSettings compressor)
    : clock_(make_audio_resampler(SRC_SINC_FASTEST, 1)),
      // Fixed TX voice band; independent of the wider RX defaults/settings.
      usb_filter_(make_usb_filter(2, RxFilterSettings::from_edges(100, 3000))),
      interpolator_(firinterp_crcf_create_kaiser(100, 16, 100)),
      compressor_(compressor) {
    if (!interpolator_) throw std::runtime_error("Cannot create liquid TX interpolator");
    reset(offset);
}
TxChannel::~TxChannel() = default;
void TxChannel::clear_dsp() {
    pending_.clear();
    compressor_.reset();limiter_.reset();
    check_liquid(firfilt_cccf_reset(usb_filter_.get()));
    check_liquid(firinterp_crcf_reset(interpolator_.get()));
    src_reset(clock_.get());
    // Prime the adaptive sinc clock history. The exact integer polyphase
    // interpolator always emits 100 IQs per audio sample, including startup.
    std::vector<float> zeros(512), out(600);
    SRC_DATA d{}; d.data_in=zeros.data();d.data_out=out.data();
    d.input_frames=zeros.size();d.output_frames=out.size();d.src_ratio=1;
    process(clock_.get(),d);
    started_=false; correction_=integral_=smoothed_=clock_buffered_=0;
}
void TxChannel::reset(int64_t offset) {
    if(std::abs(offset)>275000) throw std::runtime_error("TX frequency outside 600 kHz window");
    packets_.clear(); next_=0;have_next_=false;
    mixer_.tune(offset);
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
    process(clock_.get(),d);
    clock_buffered_+=d.input_frames_used-d.output_frames_gen*(1+correction_);
    pending_.erase(pending_.begin(),pending_.begin()+d.input_frames_used);
    if(d.output_frames_gen!=audio_block) { ++underruns;clear_dsp();return result; }
    return audio;
}
std::vector<std::complex<float>> TxChannel::render(bool recent) {
    const auto audio=pull_audio(recent);
    std::vector<std::complex<float>> result(iq_block);
    if(audio.empty()) return result;
    std::vector<std::complex<float>> input(audio_block), analytic(audio_block);
    for(size_t i=0;i<audio.size();++i) input[i]={compressor_.execute(audio[i]),0};
    check_liquid(firfilt_cccf_execute_block(usb_filter_.get(),input.data(),audio_block,analytic.data()));
    check_liquid(firinterp_crcf_execute_block(interpolator_.get(),analytic.data(),audio_block,result.data()));
    for(auto& sample:result) sample=limiter_.execute(sample*.5f);
    mixer_.execute_block(result.data(),result.size());
    return result;
}
}
