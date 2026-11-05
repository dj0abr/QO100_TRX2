#include "qo100/rx_filter.hpp"
#include <stdexcept>

namespace qo100 {
RxAudioFilter::RxAudioFilter(RxFilterSettings settings)
    : filter_(make_usb_filter(1, settings)), active_(settings), target_(settings), fading_to_(settings) {}
void RxAudioFilter::set(RxFilterSettings settings) {
    if (!settings.valid()) throw std::runtime_error("RX filter must remain inside 100..4000 Hz with BW 500..3900 Hz");
    // Coalesce rapid control updates. Design only when the DSP can start the
    // next transition, never once per datagram in a burst of slider updates.
    target_ = settings;
}
void RxAudioFilter::reset() {
    if (target_ != active_) filter_ = make_usb_filter(1, target_);
    else check_liquid(firfilt_cccf_reset(filter_.get()));
    active_ = target_;
    replacement_.reset(); fade_pos_ = 0;
    history_pos_ = history_count_ = 0;
}
std::complex<float> RxAudioFilter::execute(std::complex<float> sample) {
    if (!replacement_ && target_ != active_) {
        replacement_ = make_usb_filter(1, target_);
        fading_to_ = target_; fade_pos_ = 0;
        size_t pos = (history_pos_ + history_.size() - history_count_) % history_.size();
        for (size_t i = 0; i < history_count_; ++i) {
            check_liquid(firfilt_cccf_push(replacement_.get(), history_[pos]));
            pos = (pos + 1) % history_.size();
        }
    }
    std::complex<float> result;
    check_liquid(firfilt_cccf_execute_one(filter_.get(), sample, &result));
    if (replacement_) {
        std::complex<float> next;
        check_liquid(firfilt_cccf_execute_one(replacement_.get(), sample, &next));
        const float alpha = float(++fade_pos_) / fade_samples;
        result = result*(1-alpha) + next*alpha;
        if (fade_pos_ == fade_samples) {
            filter_ = std::move(replacement_);
            active_ = fading_to_;
        }
    }
    history_[history_pos_] = sample;
    history_pos_ = (history_pos_ + 1) % history_.size();
    history_count_ = std::min(history_count_+1, history_.size());
    return result;
}
}
