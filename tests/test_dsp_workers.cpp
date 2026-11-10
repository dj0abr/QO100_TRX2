#include "qo100/dsp_workers.hpp"
#include "qo100/spectrum.hpp"
#include <atomic>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

using Complex = std::complex<float>;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct Digest {
    uint64_t hash=1469598103934665603ull,count=0;
    void add(const std::vector<int16_t>& pcm,uint64_t index) {
        hash=(hash^index)*1099511628211ull;
        for(auto x:pcm) hash=(hash^uint16_t(x))*1099511628211ull;
        ++count;
    }
    bool operator==(const Digest& b)const {return hash==b.hash && count==b.count;}
};
struct Chain {
    std::array<Digest,8> output;
    std::array<std::unique_ptr<qo100::RxChannel>,5> rx;
    std::array<int64_t,5> offsets{150000,-150000,-210000,-1179,243000};
    qo100::Spectrum spectrum{600000,24000,20,[this](const auto& p,uint64_t index) {
        constexpr uint64_t center=10489750000ULL;
        output[5].add(qo100::wide_spectrum(p,center).bins,index);
        for(size_t i=0;i<2;++i)
            output[6+i].add(qo100::zoom_spectrum(p,center,center+offsets[i]).bins,index);
    }};
    Chain() {
        for(size_t i=0;i<5;++i) rx[i]=std::make_unique<qo100::RxChannel>(offsets[i],[](Complex){},
            [this,i](const auto& pcm,uint64_t index){output[i].add(pcm,index);},
            qo100::RxFilterSettings{},i==0?std::optional<uint32_t>(0):std::nullopt,
            i==4?qo100::RxChannel::Profile::multimedia:qo100::RxChannel::Profile::normal);
    }
    void controls(unsigned phase) {
        if(phase==1) {
            offsets[0]=-120000;offsets[1]=250000;
            rx[0]->retune(offsets[0]);rx[1]->retune(offsets[1]);
            rx[0]->set_filter(qo100::RxFilterSettings::from_edges(300,2700));rx[0]->set_nr(60);
        }
        if(phase==2) {
            offsets[0]=1500;rx[0]->set_nr(0);
            rx[0]->set_filter(qo100::RxFilterSettings::from_edges(700,1200));
            spectrum.reset();for(size_t i=0;i<5;++i)rx[i]->tune(offsets[i]);
        }
    }
};
int main() {
    try {
        const auto main_thread=std::this_thread::get_id();
        std::atomic<unsigned> completed{0};
        {
            qo100::DspWorker worker("qo100-test");worker.wait();
            worker.start([&]{ require(std::this_thread::get_id()!=main_thread,"Job ran on main");++completed; });
            bool rejected=false;
            try {worker.start([]{});} catch(const std::logic_error&) {rejected=true;}
            require(rejected,"Outstanding job was overwritten");worker.wait();
            worker.start([]{throw std::runtime_error("test exception");});
            bool propagated=false;
            try {worker.wait();} catch(const std::runtime_error&) {propagated=true;}
            require(propagated,"Worker exception was lost");
            worker.start([&]{++completed;}); // Destructor must join outstanding work.
        }
        require(completed==2,"Worker shutdown lost work");
        Chain serial,parallel;
        qo100::RxWorkers workers({parallel.rx[0].get(),parallel.rx[1].get(),parallel.rx[2].get(),
            parallel.rx[3].get(),parallel.rx[4].get()});
        std::vector<Complex> input(600000);uint32_t random=1;
        auto noise=[&]{random=random*1664525+1013904223;return(float(random>>8)/16777216.f-.5f)*.2f;};
        for(auto& x:input)x={noise(),noise()};
        const auto original=input;
        // Missing groups, holes inside a group and no channels must all join safely.
        for(const unsigned mask:{0u,1u,2u,4u,8u,16u,17u,21u}) {
            Chain sparse;
            std::array<qo100::RxChannel*,5> selected{};
            for(size_t i=0;i<5;++i)if(mask&(1u<<i))selected[i]=sparse.rx[i].get();
            qo100::RxWorkers optional(selected);
            bool called=false;
            optional.process(input.data(),60000,[&]{called=true;});
            require(called,"FFT callback skipped with disabled RX");
            for(size_t i=0;i<5;++i)
                require((sparse.output[i].count>0)==bool(mask&(1u<<i)),"Disabled RX was processed or enabled RX skipped");
            optional.process(nullptr,0,[]{});
        }
        // Runtime sleep and wake must also work without reconstructing workers.
        {
            Chain dynamic;
            qo100::RxWorkers active({dynamic.rx[0].get(),dynamic.rx[1].get(),dynamic.rx[2].get(),dynamic.rx[3].get(),dynamic.rx[4].get()});
            for(unsigned mask:{31u,29u,0u,2u,31u}) {
                const auto before=dynamic.output;
                bool fft=false;active.process(input.data(),60000,[&]{fft=true;},mask);
                require(fft,"FFT stopped during RX sleep");
                for(size_t i=0;i<5;++i)require((dynamic.output[i].count>before[i].count)==bool(mask&(1u<<i)),"Runtime RX mask ignored");
            }
        }
        // Main exceptions must still join all RX jobs before input/control reuse.
        bool propagated=false;
        try { workers.process(nullptr,0,[]{throw std::runtime_error("main exception");}); }
        catch(const std::runtime_error&) {propagated=true;}
        require(propagated,"Main exception was lost");
        workers.process(nullptr,0,[]{});
        const std::array<size_t,7> sizes{1,7,255,4097,8192,3808,16384};size_t block=0;
        for(unsigned phase=0;phase<3;++phase) {
            serial.controls(phase);parallel.controls(phase);
            for(size_t first=0;first<input.size();) {
                const size_t count=std::min(sizes[block++%sizes.size()],input.size()-first);
                for(auto& rx:serial.rx)rx->push_block(input.data()+first,count);
                for(size_t i=0;i<count;++i)serial.spectrum.push(input[first+i]);
                workers.process(input.data()+first,count,[&]{
                    require(std::this_thread::get_id()==main_thread,"FFT moved off main");
                    for(size_t i=0;i<count;++i)parallel.spectrum.push(input[first+i]);
                });
                require(serial.output==parallel.output,"Serial/parallel PCM or waterfall differs");
                first+=count;
            }
        }
        require(input==original,"Shared IQ input was modified");
        require(serial.output[0].count>100,"Insufficient output coverage");
        std::cout<<"Three RX workers: identical five PCM streams and three waterfalls; retune/filter/NR/reset, immutable IQ, exceptions and shutdown passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
