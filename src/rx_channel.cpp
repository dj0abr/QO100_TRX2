#include "qo100/rx_channel.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace qo100 {
RxChannel::RxChannel(int64_t offset, Zoom zoom, Audio audio, RxFilterSettings filter, std::optional<uint32_t> nr_percent, Profile profile)
    : decimator_(make_rx_decimator()), usb_filter_(filter),
      mm_filter_(profile==Profile::multimedia?make_mm_usb_filter():nullptr),
      src_(make_audio_resampler(SRC_SINC_MEDIUM_QUALITY, 2)),
      zoom_(std::move(zoom)), audio_(std::move(audio)) {
    input_.reserve(512); audio_packet_.reserve(240);
    if(nr_percent) nr_=std::make_unique<NoiseReduction>(*nr_percent);
    tune(offset);
}
RxChannel::~RxChannel() = default;
void RxChannel::retune(int64_t offset) {
    if (std::abs(offset) > 275000) throw std::runtime_error("Zoom must remain within the 600 kHz RX window");
    mixer_.tune(-offset, false);
}
void RxChannel::tune(int64_t offset) {
    if (std::abs(offset) > 275000) throw std::runtime_error("Zoom must remain within the 600 kHz RX window");
    mixer_.tune(-offset);
    decimation_ = 0;
    check_liquid(firdecim_crcf_reset(decimator_.get()));
    usb_filter_.reset();
    if(mm_filter_) check_liquid(firfilt_cccf_reset(mm_filter_.get()));
    if(nr_) nr_->reset();
    input_.clear(); audio_packet_.clear(); audio_index_ = 0;
    src_reset(src_.get());
}
void RxChannel::push(std::complex<float> x) {
    mixed_[decimation_++] = mixer_.execute(x);
    if (decimation_ != mixed_.size()) return;
    decimation_ = 0;
    std::complex<float> y;
    check_liquid(firdecim_crcf_execute(decimator_.get(), mixed_.data(), &y));
    zoom_(y);
    input_.push_back(y.real()); input_.push_back(y.imag());
    if (input_.size() == 512) resample();
}
void RxChannel::resample() {
    float output[512];
    long used = 0;
    while (used < 256) {
        SRC_DATA data{};
        data.data_in = input_.data() + 2 * used;
        data.input_frames = 256 - used;
        data.data_out = output; data.output_frames = 256;
        data.src_ratio = 12000. / 75000.;
        const auto error = src_process(src_.get(), &data);
        if (error) throw std::runtime_error(src_strerror(error));
        if (!data.input_frames_used && !data.output_frames_gen) throw std::runtime_error("RX resampler stalled");
        used += data.input_frames_used;
        for (long i = 0; i < data.output_frames_gen; ++i) {
            std::complex<float> filtered;
            if(mm_filter_) check_liquid(firfilt_cccf_execute_one(mm_filter_.get(),{output[2*i], output[2*i+1]},&filtered));
            else filtered = usb_filter_.execute({output[2*i], output[2*i+1]});
            audio_packet_.push_back(static_cast<int16_t>(std::lround(std::clamp(filtered.real(), -1.f, 1.f) * 32767)));
            if (audio_packet_.size() == 240) {
                if(nr_) nr_->process(audio_packet_);
                audio_(audio_packet_, audio_index_);
                audio_index_ += audio_packet_.size();
                audio_packet_.clear();
            }
        }
    }
    input_.clear();
}
}
