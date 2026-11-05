#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bridge {
using Bytes = std::vector<uint8_t>;
inline uint64_t get(const Bytes& p, size_t off, size_t n) {
    if(n>8 || off>p.size() || n>p.size()-off) throw std::runtime_error("Short QTR2 packet");
    uint64_t v=0; for(size_t i=0;i<n;++i) v=(v<<8)|p[off+i]; return v;
}
inline void put(Bytes& p, size_t off, uint64_t v, size_t n) {
    if(n>8 || off>p.size() || n>p.size()-off) throw std::runtime_error("Short QTR2 packet");
    for(size_t i=0;i<n;++i) p[off+n-1-i]=uint8_t(v>>(i*8));
}
inline Bytes header(uint8_t type, uint32_t stream=0, uint32_t id=0, uint32_t session=0) {
    Bytes p(64,0); std::memcpy(p.data(),"QTR2",4);p[4]=1;p[5]=type;
    put(p,6,64,2);put(p,8,stream,4);put(p,12,id,4);put(p,60,session,4);return p;
}
inline bool valid(const Bytes& p) {
    return p.size()>=64 && p.size()<=1200 && !std::memcmp(p.data(),"QTR2",4) &&
        p[4]==1 && p[6]==0 && p[7]==64;
}
}
