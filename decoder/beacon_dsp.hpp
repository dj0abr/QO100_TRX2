// SPDX-License-Identifier: GPL-3.0-only
// QO-100 framing/FEC parameters and algorithms adapted from the pinned
// TA2LDV reference; see external/qo100-beacon-decoder/ORIGIN.md.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace qo100::beacon {
struct Frame {
    uint64_t sample=0;
    bool fec=false;
    std::vector<uint8_t> payload;
    int corrections=0;
};
struct Result {std::vector<Frame> frames;double carrier=0;};
uint16_t crc16(const std::vector<uint8_t>& data);
std::vector<uint8_t> viterbi(const std::vector<uint8_t>& symbols);
std::vector<uint8_t> descramble(const std::vector<uint8_t>& bytes);
bool rs_frame(const std::vector<uint8_t>& bytes,std::vector<uint8_t>& payload,int& corrected);
bool fec_frame(const std::vector<uint8_t>& bits,std::vector<uint8_t>& payload,int& corrected);
Result decode(const std::vector<int16_t>& pcm,unsigned rate,uint64_t rx_hz);
std::string format(const Frame& frame,double carrier);
}
