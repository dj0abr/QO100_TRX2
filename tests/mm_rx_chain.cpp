// Offline 48-kHz audio -> synthetic 600-kHz RF IQ -> real RX5 -> PCM16 stdout.
#include "qo100/rx_channel.hpp"
#include <array>
#include <cstdio>
#include <iostream>

int main() {
    try {
        qo100::RxChannel rx(243220,[](auto){},[](const auto& pcm,uint64_t) {
            if(std::fwrite(pcm.data(),sizeof(int16_t),pcm.size(),stdout)!=pcm.size())
                throw std::runtime_error("PCM output failed");
        },{},std::nullopt,qo100::RxChannel::Profile::multimedia);
        qo100::FrequencyShifter carrier(600000);carrier.tune(243220);
        auto src=qo100::make_audio_resampler(SRC_SINC_MEDIUM_QUALITY,1);
        std::array<float,960> input{};std::array<float,16000> output{};
        while(const auto count=std::fread(input.data(),sizeof(float),input.size(),stdin)) {
            long used=0;
            while(used<static_cast<long>(count)) {
                SRC_DATA data{};data.data_in=input.data()+used;data.input_frames=count-used;
                data.data_out=output.data();data.output_frames=output.size();data.src_ratio=12.5;
                if(src_process(src.get(),&data)) throw std::runtime_error("RF resampler failed");
                used+=data.input_frames_used;
                for(long i=0;i<data.output_frames_gen;++i) rx.push(carrier.execute({2*output[i],0}));
            }
        }
        return std::ferror(stdin)?1:0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
