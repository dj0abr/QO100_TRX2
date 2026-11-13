#include "utc_slots.hpp"
#include "wire.hpp"
#include "qo100/channels.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace qo100 {
namespace {
constexpr int64_t rate=12000,packet_ns=20000000;
constexpr unsigned max_gap_samples=rate/10, max_missing_per_slot=rate/5;
int64_t sample_tick(int64_t ns) {
    return ns/1000000000*rate+std::llround((ns%1000000000)*(double(rate)/1e9));
}
int64_t elapsed_ns(uint64_t samples) {
    return int64_t(samples/rate)*1000000000+int64_t(samples%rate)*1000000000/rate;
}
}
bool parse_ft8_audio(const uint8_t* data,size_t size,Ft8PcmBlock& block,unsigned stream) {
    if(size!=544) return false;
    const bridge::Bytes p(data,data+size);
    if(!bridge::valid(p) || p[5]!=5 || bridge::get(p,8,4)!=stream || bridge::get(p,40,4)!=12000 ||
       bridge::get(p,52,2)!=240 || bridge::get(p,54,2)!=2 ||
       bridge::get(p,56,4)!=240 || !bridge::get(p,60,4)) return false;
    // The private RX2 link carries the current decoder selection in the reserved fields.
    const auto revision=bridge::get(p,44,4), mode=bridge::get(p,48,4);
    if ((revision || mode) && (stream!=2 || !revision || (mode!=4 && mode!=8))) return false;
    block.mode_revision=revision;
    block.mode=mode==4?FtMode::ft4:FtMode::ft8;
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
Ft8UtcSlots::Ft8UtcSlots(Sink sink,int64_t offset,bool fill_short_gaps,bool early_ft8)
    :sink_(std::move(sink)),offset_ns_(offset),fill_short_gaps_(fill_short_gaps),early_ft8_(early_ft8) {
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
    uint64_t missing=0;
    if(!started_) reason="startup";
    else if(block.session!=session_ || block.epoch!=epoch_) reason="session/epoch changed";
    else if(block.mode!=mode_ || block.mode_revision!=mode_revision_) reason="FT mode changed";
    else if(block.frequency!=frequency_) reason="receiver frequency changed";
    else if(block.index-expected_>(fill_short_gaps_?max_gap_samples:0)) reason="audio sample gap";
    else {
        const auto predicted=int64_t(anchor_ns_)+elapsed_ns(block.index-anchor_index_);
        if(std::abs(int64_t(block.timestamp_ns)-predicted)>250000000LL) reason="UTC clock step or stream stall";
        else missing=block.index-expected_;
    }
    if(reason) {
        reset(reason);started_=true;session_=block.session;epoch_=block.epoch;frequency_=block.frequency;
        mode_=block.mode;mode_revision_=block.mode_revision;
        anchor_index_=block.index;anchor_ns_=block.timestamp_ns;
        anchor_tick_=sample_tick(int64_t(block.timestamp_ns)-packet_ns+offset_ns_);
    }
    expected_=block.index+240;
    auto tick=anchor_tick_+int64_t(block.index-anchor_index_-missing);
    const auto slot_samples=ft_slot_samples(mode_);
    const auto decode_samples=ft_decode_samples(mode_);
    const auto append=[&](float sample,bool padded) {
        if(tick%slot_samples==0) {
            slot_=FT8Slot{};slot_.utcMilliseconds=tick/(rate/1000);slot_.dialHz=frequency_;slot_.generation=generation_;
            slot_.session=block.session;slot_.epoch=block.epoch;
            slot_.mode=mode_;slot_.modeRevision=mode_revision_;
            slot_.audio.reserve(decode_samples);collecting_=true;slot_missing_samples_=0;
        }
        ++tick;
        if(!collecting_) return;
        if(padded && ++slot_missing_samples_>max_missing_per_slot) {
            slot_.audio.clear();collecting_=false;++discarded_;reason_="too much missing audio in UTC slot";
            return;
        }
        slot_.audio.push_back(sample);
        if(early_ft8_ && mode_==FtMode::ft8 && slot_.audio.size()==ft8_early_samples) {
            // Keep collecting the full window for signals the early pass misses.
            auto early=slot_;
            sink_(std::move(early));
        }
        if(slot_.audio.size()==decode_samples) {collecting_=false;sink_(std::move(slot_));}
    };
    // Keep the original sample clock and symbol positions through short UDP gaps.
    filled_samples_+=missing;
    for(uint64_t i=0;i<missing;++i) append(0.f,true);
    for(float sample:block.audio) {
        if(!std::isfinite(sample)) {reset("non-finite PCM");return;}
        append(sample,false);
    }
}
}
