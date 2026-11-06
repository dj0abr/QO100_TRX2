#include "utc_slots.hpp"
#include "wire.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace qo100 {
namespace {
constexpr int64_t rate=12000,slot_samples=180000,packet_ns=20000000;
int64_t sample_tick(int64_t ns) {
    return ns/1000000000*rate+std::llround((ns%1000000000)*(double(rate)/1e9));
}
int64_t elapsed_ns(uint64_t samples) {
    return int64_t(samples/rate)*1000000000+int64_t(samples%rate)*1000000000/rate;
}
}
bool parse_ft8_audio(const uint8_t* data,size_t size,Ft8PcmBlock& block) {
    if(size!=544) return false;
    const bridge::Bytes p(data,data+size);
    if(!bridge::valid(p) || p[5]!=5 || bridge::get(p,8,4)!=2 || bridge::get(p,40,4)!=12000 ||
       bridge::get(p,44,8) || bridge::get(p,52,2)!=240 || bridge::get(p,54,2)!=2 ||
       bridge::get(p,56,4)!=240 || !bridge::get(p,60,4)) return false;
    block.session=bridge::get(p,60,4);block.epoch=bridge::get(p,12,4);
    block.index=bridge::get(p,24,8);block.frequency=bridge::get(p,32,8);block.timestamp_ns=bridge::get(p,16,8);
    if(!block.frequency || block.frequency>1000000000000ULL || block.index>UINT64_MAX-240 ||
       block.timestamp_ns<6000000000ULL || block.timestamp_ns>uint64_t(INT64_MAX)-6000000000ULL) return false;
    for(size_t i=0;i<240;++i) {
        const int value=bridge::get(p,64+2*i,2);
        block.audio[i]=(value>=32768?value-65536:value)/32768.f;
    }
    return true;
}
Ft8UtcSlots::Ft8UtcSlots(Sink sink,int64_t offset):sink_(std::move(sink)),offset_ns_(offset) {
    if(offset < -5000000000LL || offset > 5000000000LL) throw std::runtime_error("FT8 time offset must be within +/-5000 ms");
}
void Ft8UtcSlots::reset(const std::string& reason) {
    if(collecting_ && !slot_.audio.empty()) ++discarded_;
    slot_=FT8Slot{};collecting_=false;started_=false;reason_=reason;++generation_;
}
void Ft8UtcSlots::push(const Ft8PcmBlock& block) {
    const bool same_stream=started_ && block.session==session_ && block.epoch==epoch_;
    // A delayed old-frequency datagram must not undo a subsequent retune.
    if(same_stream && block.index<expected_) return;
    const char* reason=nullptr;
    if(!started_) reason="startup";
    else if(block.session!=session_ || block.epoch!=epoch_) reason="session/epoch changed";
    else if(block.frequency!=frequency_) reason="RX2 frequency changed";
    else if(block.index!=expected_) reason="audio sample gap";
    else {
        const auto predicted=int64_t(anchor_ns_)+elapsed_ns(block.index-anchor_index_);
        if(std::abs(int64_t(block.timestamp_ns)-predicted)>250000000LL) reason="UTC clock step or stream stall";
    }
    if(reason) {
        reset(reason);started_=true;session_=block.session;epoch_=block.epoch;frequency_=block.frequency;
        anchor_index_=block.index;anchor_ns_=block.timestamp_ns;
        anchor_tick_=sample_tick(int64_t(block.timestamp_ns)-packet_ns+offset_ns_);
    }
    expected_=block.index+240;
    auto tick=anchor_tick_+int64_t(block.index-anchor_index_);
    for(float sample:block.audio) {
        const auto index=tick%slot_samples;
        if(index==0) {
            slot_=FT8Slot{};slot_.utcSeconds=tick/rate;slot_.dialHz=frequency_;slot_.generation=generation_;
            slot_.audio.reserve(slot_samples);collecting_=true;
        }
        if(!std::isfinite(sample)) {reset("non-finite PCM");return;}
        if(collecting_) {
            slot_.audio.push_back(sample);
            if(slot_.audio.size()==slot_samples) {collecting_=false;sink_(std::move(slot_));}
        }
        ++tick;
    }
}
}
