#include "qo100/rx_channel.hpp"
#include "qo100/dsp_workers.hpp"
#include <chrono>
#include "qo100/spectrum.hpp"
#include "qo100/tx_channel.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <time.h>
#include <vector>

namespace {
using Complex=std::complex<float>;
constexpr double pi=3.14159265358979323846;
constexpr size_t signal_seconds=4;
volatile float sink;
double cpu(clockid_t clock=CLOCK_THREAD_CPUTIME_ID) {
    timespec t{};clock_gettime(clock,&t);
    return t.tv_sec+t.tv_nsec*1e-9;
}
struct Digest {
    uint64_t value=1469598103934665603ull,packets=0;
    void add(const std::vector<int16_t>& data,uint64_t index) {
        value=(value^index)*1099511628211ull;
        for(auto sample:data)value=(value^uint16_t(sample))*1099511628211ull;
        ++packets;
    }
    bool operator==(const Digest& other) const {return value==other.value && packets==other.packets;}
};
struct Result {double ms=0,wall_ms=0,main_ms=0;std::array<Digest,8> outputs;};
Result receive(const std::vector<Complex>& iq,unsigned mode) {
    Result result;
    std::array<std::unique_ptr<qo100::RxChannel>,5> rx;
    std::array<int64_t,5> offsets{150000,-150000,-210000,-1179,243000};
    constexpr uint64_t center=10489750000ULL;
    for(size_t i=0;i<rx.size();++i) {
        rx[i]=std::make_unique<qo100::RxChannel>(offsets[i],[](Complex){},
            [&,i](const auto& pcm,uint64_t index){result.outputs[i].add(pcm,index);},
            qo100::RxFilterSettings{},i==0?std::optional<uint32_t>(0):std::nullopt,
            i==4?qo100::RxChannel::Profile::multimedia:qo100::RxChannel::Profile::normal);
    }
    qo100::Spectrum spectrum(600000,qo100::spectrum_fft_size,20,[&](const auto& powers,uint64_t index) {
        result.outputs[5].add(qo100::wide_spectrum(powers,center).bins,index);
        for(size_t i=0;i<2;++i)result.outputs[6+i].add(qo100::zoom_spectrum(powers,center,center+offsets[i]).bins,index);
    });
    std::unique_ptr<qo100::RxWorkers> workers;
    if(mode==2)workers=std::make_unique<qo100::RxWorkers>(std::array<qo100::RxChannel*,5>{
        rx[0].get(),rx[1].get(),rx[2].get(),rx[3].get(),rx[4].get()});
    const auto wall=std::chrono::steady_clock::now();
    const auto start=cpu(CLOCK_PROCESS_CPUTIME_ID),main_start=cpu();
    for(size_t second=0;second<signal_seconds;++second) {
        if(second==2) {
            offsets[0]=-120000;offsets[1]=250000;
            for(size_t i=0;i<2;++i)rx[i]->retune(offsets[i]);
            rx[0]->set_filter(qo100::RxFilterSettings::from_edges(300,2700));rx[0]->set_nr(60);
        }
        if(mode) {
            // Actual 20-ms driver blocks arrive as 8192 + 3808 complex samples.
            bool first_fragment=true;
            for(size_t first=0;first<iq.size();) {
                const size_t count=std::min<size_t>(first_fragment?8192:3808,iq.size()-first);
                const auto fft=[&]{for(size_t i=0;i<count;++i)spectrum.push(iq[first+i]);};
                if(workers)workers->process(iq.data()+first,count,fft);
                else {fft();for(auto& channel:rx)channel->push_block(iq.data()+first,count);}
                first+=count;first_fragment=!first_fragment;
            }
        } else {
            for(auto sample:iq) {spectrum.push(sample);for(auto& channel:rx)channel->push(sample);}
        }
    }
    result.ms=(cpu(CLOCK_PROCESS_CPUTIME_ID)-start)*1000/signal_seconds;
    result.main_ms=(cpu()-main_start)*1000/signal_seconds;
    result.wall_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-wall).count()/signal_seconds;
    return result;
}
}
int main() {
    try {
        std::vector<Complex> iq(600000);uint32_t random=1;
        const auto noise=[&](){random=random*1664525+1013904223;return (float(random>>8)/16777216.f-.5f)*.2f;};
        for(auto& sample:iq)sample={noise(),noise()};
        std::vector<double> scalar_times,block_times,tx_times,serial_wall,parallel_wall,parallel_cpu,parallel_main;
        std::cout<<std::fixed<<std::setprecision(2);
        for(int repetition=0;repetition<7;++repetition) {
            Result scalar,blocked,parallel;
            // Alternate order to reduce bias from cache and CPU-frequency changes.
            if(repetition%2){parallel=receive(iq,2);blocked=receive(iq,1);scalar=receive(iq,0);}
            else {scalar=receive(iq,0);blocked=receive(iq,1);parallel=receive(iq,2);}
            if(scalar.outputs!=blocked.outputs || blocked.outputs!=parallel.outputs)throw std::runtime_error("RX/FFT output checksum mismatch");
            serial_wall.push_back(blocked.wall_ms);parallel_wall.push_back(parallel.wall_ms);
            parallel_cpu.push_back(parallel.ms);parallel_main.push_back(parallel.main_ms);
            scalar_times.push_back(scalar.ms);block_times.push_back(blocked.ms);
            std::cout<<"Five RX + shared FFT, run "<<repetition+1<<": scalar="<<scalar.ms
                <<"; blocks="<<blocked.ms<<" CPU ms/s; three workers: wall="<<parallel.wall_ms
                <<"; CPU="<<parallel.ms<<"; main="<<parallel.main_ms<<" ms/s; identical outputs\n";
            qo100::TxChannel tx(150000);uint64_t index=0;
            std::vector<int16_t> pcm(240);
            for(size_t i=0;i<pcm.size();++i)pcm[i]=int16_t(16000*std::cos(2*pi*1000*i/12000));
            for(int i=0;i<6;++i){tx.add(index,pcm);index+=240;}
            const auto start=cpu();
            for(int i=0;i<100;++i){if(i){tx.add(index,pcm);index+=240;}const auto y=tx.render(true);sink=y[12000].real();}
            tx_times.push_back((cpu()-start)*500);
        }
        std::sort(scalar_times.begin(),scalar_times.end());std::sort(block_times.begin(),block_times.end());std::sort(tx_times.begin(),tx_times.end());
        for(auto* times:{&serial_wall,&parallel_wall,&parallel_cpu,&parallel_main})std::sort(times->begin(),times->end());
        std::cout<<"Median RX block throughput: serial="<<serial_wall[3]<<"; three workers="<<parallel_wall[3]
            <<" wall ms/s; speedup="<<serial_wall[3]/parallel_wall[3]<<"; worker total CPU="<<parallel_cpu[3]
            <<"; main CPU="<<parallel_main[3]<<" ms/s\n";
        std::cout<<"Median five RX + shared FFT: scalar="<<scalar_times[3]<<"; blocks="<<block_times[3]
            <<" CPU ms/s; reduction="<<(1-block_times[3]/scalar_times[3])*100<<"%; TX="<<tx_times[3]<<" CPU ms/s\n"
            <<"DSP-only offline benchmark; excludes hardware, I/Q generation, UDP and GUI.\n";
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
