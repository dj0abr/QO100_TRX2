// SPDX-License-Identifier: GPL-3.0-only
// Port of TA2LDV's ao40_fec.py (gr-satellites/libfec conventions), with
// precomputed branch metrics and fixed storage instead of Python scalar loops.
#include "beacon_dsp.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace qo100::beacon {
uint16_t crc16(const std::vector<uint8_t>& data) {
    uint16_t crc=0xffff;
    for(auto byte:data) {
        crc^=uint16_t(byte)<<8;
        for(unsigned bit=0;bit<8;++bit)crc=uint16_t((crc<<1)^((crc&0x8000)?0x1021:0));
    }
    return crc;
}
std::vector<uint8_t> viterbi(const std::vector<uint8_t>& symbols) {
    if(symbols.size()!=5132)throw std::runtime_error("Expected 5132 AO-40 convolutional symbols");
    std::array<unsigned,128> encoded{};
    for(unsigned reg=0;reg<128;++reg)
        encoded[reg]=(unsigned(__builtin_parity(reg&79))<<1)|(unsigned(__builtin_parity(reg&109))^1u);
    std::array<int,64> previous{},next{};previous.fill(10000);previous[0]=0;
    std::vector<std::array<uint8_t,64>> path(2566);
    constexpr std::array<int,4> distance{0,1,1,2};
    for(size_t step=0;step<path.size();++step) {
        const auto received=(symbols[2*step]<<1)|symbols[2*step+1];
        for(unsigned state=0;state<64;++state) {
            const unsigned a=state>>1,b=a+32,bit=state&1;
            const int cost_a=previous[a]+distance[encoded[2*a+bit]^received];
            const int cost_b=previous[b]+distance[encoded[2*b+bit]^received];
            const bool first=cost_a<=cost_b;
            next[state]=first?cost_a:cost_b;path[step][state]=uint8_t(first?a:b);
        }
        previous=next;
    }
    std::vector<uint8_t> bytes(320,0);unsigned state=0;
    for(size_t step=path.size();step-->0;) {
        if(step<2560)bytes[step/8]|=uint8_t((state&1)<<(7-step%8));
        state=path[step][state];
    }
    return bytes;
}
std::vector<uint8_t> descramble(const std::vector<uint8_t>& bytes) {
    auto result=bytes;unsigned reg=0xff;
    for(auto& byte:result) {
        unsigned value=0;
        for(unsigned bit=0;bit<8;++bit) {
            value=(value<<1)|(reg&1);
            const auto feedback=((reg>>0)^(reg>>3)^(reg>>5)^(reg>>7))&1;
            reg=(reg>>1)|(feedback<<7);
        }
        byte^=uint8_t(value);
    }
    return result;
}
namespace {
struct Galois {
    std::array<int,256> exp{},log{};
    Galois() {
        int x=1;
        for(int i=0;i<255;++i) {exp[i]=x;log[x]=i;x<<=1;if(x&256)x^=0x187;}
        exp[255]=exp[0];
    }
    int alpha(int n)const {n%=255;if(n<0)n+=255;return exp[n];}
    int mul(int a,int b)const {return a && b?alpha(log[a]+log[b]):0;}
};
const Galois gf;
std::array<int,32> syndromes(const std::array<int,255>& data) {
    std::array<int,32> synd{};
    for(int i=0;i<32;++i) {
        const int root=gf.alpha(11*(112+i));
        for(auto c:data)synd[i]=gf.mul(synd[i],root)^c;
    }
    return synd;
}
int rs_decode(std::array<int,255>& data) {
    const auto synd=syndromes(data);
    if(std::all_of(synd.begin(),synd.end(),[](int x){return x==0;}))return 0;
    std::array<int,33> C{},B{};C[0]=B[0]=1;
    int length=0,shift=1,scale=1;
    for(int n=0;n<32;++n) {
        int discrepancy=synd[n];
        for(int i=1;i<=length;++i)discrepancy^=gf.mul(C[i],synd[n-i]);
        if(!discrepancy) {++shift;continue;}
        const auto old=C;
        const int coefficient=gf.mul(discrepancy,gf.alpha(-gf.log[scale]));
        for(int i=0;i<=32-shift;++i)C[i+shift]^=gf.mul(coefficient,B[i]);
        if(2*length<=n) {length=n+1-length;B=old;scale=discrepancy;shift=1;}
        else ++shift;
    }
    if(length>16)return -1;
    std::vector<int> positions;
    for(int j=0;j<255;++j) {
        int value=0;
        for(int i=0;i<=length;++i)if(C[i])value^=gf.alpha(gf.log[C[i]]-i*11*j);
        if(!value)positions.push_back(254-j);
    }
    if(int(positions.size())!=length)return -1;
    std::array<int,32> omega{};
    for(int i=0;i<32;++i)for(int k=0;k<=i && k<=length;++k)omega[i]^=gf.mul(C[k],synd[i-k]);
    for(int position:positions) {
        const int X=11*(254-position);
        int num=0,den=0;
        for(int i=0;i<32;++i)if(omega[i])num^=gf.alpha(gf.log[omega[i]]-i*X);
        for(int i=1;i<=length;i+=2)if(C[i])den^=gf.alpha(gf.log[C[i]]-(i-1)*X);
        if(!den)return -1;
        data[position]^=num?gf.alpha(gf.log[num]-gf.log[den]+(1-112)*X):0;
    }
    // Reject miscorrection and errors in the shortened, implicit zero prefix.
    const auto check=syndromes(data);
    if(!std::all_of(check.begin(),check.end(),[](int x){return x==0;}) ||
       !std::all_of(data.begin(),data.begin()+95,[](int x){return x==0;}))return -1;
    return length;
}
}
bool rs_frame(const std::vector<uint8_t>& bytes,std::vector<uint8_t>& payload,int& corrected) {
    if(bytes.size()!=320)return false;
    std::vector<uint8_t> result(256);corrected=0;
    for(unsigned word=0;word<2;++word) {
        std::array<int,255> data{};
        for(size_t i=0;i<160;++i)data[95+i]=bytes[2*i+word];
        const int errors=rs_decode(data);
        if(errors<0)return false;
        corrected+=errors;
        for(size_t i=0;i<128;++i)result[2*i+word]=uint8_t(data[95+i]);
    }
    payload=std::move(result);return true;
}
bool fec_frame(const std::vector<uint8_t>& bits,std::vector<uint8_t>& payload,int& corrected) {
    if(bits.size()!=5200)return false;
    std::vector<uint8_t> convolution(5132);
    for(size_t j=65;j<5197;++j)convolution[j-65]=bits[80*(j%65)+j/65];
    return rs_frame(descramble(viterbi(convolution)),payload,corrected);
}
}
