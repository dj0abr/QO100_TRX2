#include "utc_slots.hpp"
#include "wire.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint64_t utc=1700000010,frequency=10489610000;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
float value(uint64_t i) {return float(i%10007)/10007;}
qo100::Ft8PcmBlock block(uint64_t i) {
    qo100::Ft8PcmBlock p;p.session=9;p.epoch=7;p.index=i;p.frequency=frequency;
    p.timestamp_ns=utc*1000000000+(i+240)*1000000000/12000;
    for(unsigned k=0;k<240;++k) p.audio[k]=value(i+k);
    return p;
}
void slots() {
    std::vector<FT8Slot> out;
    auto sink=[&](FT8Slot&& slot){out.push_back(std::move(slot));};
    qo100::Ft8UtcSlots s(sink);
    for(uint64_t i=0;i<180000;i+=240) {auto p=block(i);if(i) p.timestamp_ns+=(i/240%5)*1000000;s.push(p);if(i==480) s.push(block(240));}
    require(out.size()==1 && out[0].utcMilliseconds==int64_t(utc)*1000 && out[0].dialHz==frequency,"UTC/64-bit frequency lost");
    require(out[0].audio.size()==180000 && out[0].audio.front()==value(0) && out[0].audio.back()==value(179999),"Slot/sample clock corrupted by jitter or reordered packet");
    out.clear();s.reset();
    for(uint64_t i=48000;i<360000;i+=240) s.push(block(i));
    require(out.size()==1 && out[0].utcMilliseconds==int64_t(utc+15)*1000 && out[0].audio.front()==value(180000),"Partial startup slot decoded");
    out.clear();qo100::Ft8UtcSlots shifted(sink,7000000);
    for(uint64_t i=0;i<360000;i+=240) shifted.push(block(i));
    require(out.size()==1 && out[0].utcMilliseconds==int64_t(utc+15)*1000 && out[0].audio.front()==value(179916) && out[0].audio.back()==value(359915),"UTC boundary inside a packet or positive offset failed");
    out.clear();s.reset();const auto lost=s.discarded();
    for(uint64_t i=0;i<360000;i+=240) if(i!=60000) s.push(block(i));
    require(out.size()==1 && out[0].utcMilliseconds==int64_t(utc+15)*1000 && s.discarded()==lost+1,"Packet loss did not discard only its partial slot");
    out.clear();s.reset();
    for(uint64_t i=0;i<360000;i+=240) {
        auto p=block(i);if(i>=60000) p.frequency+=5000;s.push(p);
        if(i==60240) s.push(block(60000));
    }
    require(out.size()==1 && out[0].dialHz==frequency+5000 && out[0].utcMilliseconds==int64_t(utc+15)*1000,"Retune mixed frequencies or a delayed packet undid tuning");
    out.clear();s.reset();
    for(uint64_t i=0;i<360000;i+=240) {auto p=block(i);if(i>=60000) p.timestamp_ns+=1000000000;s.push(p);}
    require(out.size()==1 && out[0].utcMilliseconds==int64_t(utc+15)*1000 && out[0].audio.front()==value(168000),"Clock step did not realign next UTC slot");
    out.clear();s.reset();
    for(uint64_t i=0;i<60000;i+=240) s.push(block(i));
    for(uint64_t i=0;i<300000;i+=240) {auto p=block(i);p.epoch=8;p.timestamp_ns+=5000000000;s.push(p);}
    require(out.size()==1 && out[0].utcMilliseconds==int64_t(utc+15)*1000 && out[0].audio.front()==value(120000),"Epoch/sample counter reset was not handled");
    const auto generation=out[0].generation;s.reset();require(s.generation()!=generation,"Old result generation not invalidated");
}
void rx2_gaps() {
    std::vector<FT8Slot> out;
    qo100::Ft8UtcSlots slots([&](FT8Slot&& slot){out.push_back(std::move(slot));},0,true);
    for(uint64_t i=0;i<360000;i+=240) {
        if(i==60000 || i==179760 || i==180000) continue;
        slots.push(block(i));
    }
    require(out.size()==2 && slots.discarded()==0 && slots.filled_samples()==720,"Short RX2 gap dropped a UTC slot");
    require(out[0].audio[59999]==value(59999) && out[0].audio[60000]==0 &&
            out[0].audio[60239]==0 && out[0].audio[60240]==value(60240),"RX2 gap shifted the samples");
    require(out[0].audio.back()==0 && out[1].audio[239]==0 &&
            out[1].audio[240]==value(180240),"RX2 gap crossing a slot boundary was misplaced");
    out.clear();slots.reset();
    const auto discarded=slots.discarded();
    for(uint64_t i=0;i<540000;i+=240) {
        if((i>=60000 && i<61440) || i==180000+24000 || (i>=180000+60000 && i<180000+61200) ||
           (i>=180000+90000 && i<180000+91200)) continue;
        slots.push(block(i));
    }
    require(slots.discarded()==discarded+2 && out.size()==1 && out[0].utcMilliseconds==int64_t(utc+30)*1000,
            "Large or repeated RX2 gaps did not discard only their damaged slots");
}
void early_ft4() {
    std::vector<FT8Slot> out;
    qo100::Ft8UtcSlots slots([&](FT8Slot&& slot){out.push_back(std::move(slot));},0,true);
    const auto feed=[&](uint64_t i) {
        auto p=block(i);p.mode=FtMode::ft4;p.mode_revision=1;slots.push(p);
    };
    for(uint64_t i=0;i<72480;i+=240) feed(i);
    require(out.empty(),"FT4 decoded before the required samples arrived");
    feed(72480);
    require(out.size()==1 && out[0].audio.size()==72576 && out[0].audio.back()==value(72575) &&
            out[0].utcMilliseconds==int64_t(utc)*1000,"FT4 did not submit at the 6.048-second cutoff");
    for(uint64_t i=72720;i<162720;i+=240) feed(i);
    require(out.size()==2 && out[1].audio.front()==value(90000) &&
            out[1].utcMilliseconds==int64_t(utc)*1000+7500,"FT4 lost its UTC alignment after early decode");
}
void wire() {
    auto p=bridge::header(5,5,7,9);p.resize(544);
    bridge::put(p,16,utc*1000000000+20000000,8);bridge::put(p,32,frequency,8);bridge::put(p,40,12000,4);
    bridge::put(p,52,240,2);bridge::put(p,54,2,2);bridge::put(p,56,240,4);
    bridge::put(p,64,32768,2);bridge::put(p,66,65535,2);bridge::put(p,70,32767,2);
    qo100::Ft8PcmBlock b;require(qo100::parse_ft8_audio(p.data(),p.size(),b),"Valid QTR2 RX3 audio rejected");
    require(b.frequency==frequency && b.audio[0]==-1 && b.audio[1]==-1/32768.f && b.audio[2]==0 && b.audio[3]==32767/32768.f,"Big-endian PCM16 conversion failed");
    for(const auto change:std::vector<std::pair<size_t,uint64_t>>{{8,2},{40,48000},{52,239},{54,1},{60,0},{16,0},{24,UINT64_MAX}}) {
        auto bad=p;const size_t width=change.first==16 || change.first==24?8:change.first==52 || change.first==54?2:4;
        bridge::put(bad,change.first,change.second,width);
        require(!qo100::parse_ft8_audio(bad.data(),bad.size(),b),"Malformed RX3 packet accepted");
    }
    require(!qo100::parse_ft8_audio(p.data(),543,b),"Short packet accepted");
}
}
int main() {
    try {wire();slots();rx2_gaps();early_ft4();std::cout<<"FT8 UTC slots: signed PCM, UTC alignment/offset, jitter, reorder, loss, 64-bit RF, retune, clock/epoch reset and stale-result generation passed\n";}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
