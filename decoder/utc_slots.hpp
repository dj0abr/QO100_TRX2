#pragma once
#include "FT8DecoderProcess.h"
#include "qo100/channels.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace qo100 {
struct Ft8PcmBlock {
    uint32_t session=0,epoch=0,mode_revision=0;
    FtMode mode=FtMode::ft8;
    uint64_t index=0,frequency=0,timestamp_ns=0;
    std::array<float,240> audio{};
};
bool parse_ft8_audio(const uint8_t* data,size_t size,Ft8PcmBlock& block,unsigned stream=rx3_stream);

// Follow the sample counter rather than UDP arrival times. The first packet's
// send timestamp estimates its audio end; fixed path delays can be calibrated.
class Ft8UtcSlots {
public:
    using Sink=std::function<void(FT8Slot&&)>;
    explicit Ft8UtcSlots(Sink sink,int64_t time_offset_ns=0,bool fill_short_gaps=false,bool early_ft8=false);
    void push(const Ft8PcmBlock& block);
    void reset(const std::string& reason="receiver changed");
    uint64_t generation() const {return generation_;}
    uint64_t discarded() const {return discarded_;}
    uint64_t filled_samples() const {return filled_samples_;}
    const std::string& reason() const {return reason_;}
private:
    Sink sink_;
    int64_t offset_ns_=0,anchor_tick_=0;
    uint64_t anchor_index_=0,anchor_ns_=0,expected_=0,generation_=0,discarded_=0;
    uint64_t frequency_=0;
    uint32_t session_=0,epoch_=0,mode_revision_=0;
    FtMode mode_=FtMode::ft8;
    bool started_=false,collecting_=false;
    bool fill_short_gaps_,early_ft8_;
    uint64_t filled_samples_=0;
    unsigned slot_missing_samples_=0;
    std::string reason_="startup";
    FT8Slot slot_;
};
}
