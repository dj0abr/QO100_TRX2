#include "qo100/protocol.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace qo100 {
namespace {
void put(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) bytes[offset + width - i - 1] = (value >> (i * 8)) & 255;
}
}
bool is_subscription(const uint8_t* d, size_t n) {
    return n == header_size && !std::memcmp(d, "QTR2", 4) && d[4] == protocol_version &&
        d[5] == subscribe_type && d[6] == 0 && d[7] == header_size &&
        std::all_of(d + 8, d + header_size, [](uint8_t b) { return b == 0; });
}
std::vector<std::vector<uint8_t>> spectrum_packets(const FrameInfo& f, const std::vector<int16_t>& bins) {
    if (bins.size() != f.fft_size || bins.empty() || bins.size() > 65536)
        throw std::runtime_error("Invalid spectrum frame");
    constexpr size_t chunk = (max_datagram - header_size) / 2;
    std::vector<std::vector<uint8_t>> packets;
    for (size_t start = 0; start < bins.size(); start += chunk) {
        const auto count = std::min(chunk, bins.size() - start);
        std::vector<uint8_t> p(header_size + count * 2, 0);
        std::memcpy(p.data(), "QTR2", 4);
        p[4] = protocol_version; p[5] = spectrum_type;
        put(p, 6, header_size, 2); // header length
        put(p, 8, f.stream_id, 4); // 0: wide; 1: receiver zoom
        put(p, 12, f.frame_id, 4);
        put(p, 16, f.timestamp_ns, 8);
        put(p, 24, f.sample_index, 8);
        put(p, 32, f.center_hz, 8);
        put(p, 40, f.sample_rate, 4);
        put(p, 44, f.fft_size, 4);
        put(p, 48, start, 4);
        put(p, 52, count, 2);
        put(p, 54, 1, 2); // signed int16, 0.01 dBFS, big endian
        put(p, 56, bins.size(), 4);
        put(p, 60, f.session_id, 4);
        for (size_t i = 0; i < count; ++i) put(p, header_size + i * 2, static_cast<uint16_t>(bins[start + i]), 2);
        packets.push_back(std::move(p));
    }
    return packets;
}
bool parse_tune(const uint8_t* p, size_t n, uint32_t& stream_id, uint32_t& request, uint64_t& frequency) {
    if (n != 64 || std::memcmp(p,"QTR2",4) || p[4]!=1 || p[5]!=tune_type || p[6]!=0 || p[7]!=64) return false;
    auto get = [&](size_t off, size_t width) {
        uint64_t value = 0; for (size_t i=0;i<width;++i) value = (value<<8) | p[off+i]; return value;
    };
    const auto stream=get(8,4);
    if (stream!=1 && stream!=2) return false;
    stream_id=static_cast<uint32_t>(stream);
    request=static_cast<uint32_t>(get(12,4)); frequency=get(32,8);
    return true;
}
std::vector<uint8_t> state_packet(uint32_t request, uint64_t frequency, uint32_t epoch, uint32_t session, uint16_t status, uint32_t stream_id) {
    std::vector<uint8_t> p(64,0); std::memcpy(p.data(),"QTR2",4);
    p[4]=1;p[5]=state_type;put(p,6,64,2);put(p,8,stream_id,4);put(p,12,request,4);
    put(p,24,epoch,8);put(p,32,frequency,8);put(p,40,12000,4);put(p,54,status,2);put(p,60,session,4);
    return p;
}
std::vector<uint8_t> audio_packet(const FrameInfo& f, const std::vector<int16_t>& audio) {
    if (audio.empty() || audio.size()>568) throw std::runtime_error("Invalid audio packet size");
    if (f.stream_id!=1 && f.stream_id!=2) throw std::runtime_error("Invalid audio stream ID");
    auto p=state_packet(f.frame_id,f.center_hz,0,f.session_id,2,f.stream_id);
    p[5]=audio_type;put(p,16,f.timestamp_ns,8);put(p,24,f.sample_index,8);
    put(p,52,audio.size(),2);put(p,56,audio.size(),4);p.resize(64+2*audio.size());
    for(size_t i=0;i<audio.size();++i) put(p,64+2*i,static_cast<uint16_t>(audio[i]),2);
    return p;
}
}
