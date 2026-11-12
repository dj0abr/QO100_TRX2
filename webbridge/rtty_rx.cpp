#include "bridge.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace bridge {
namespace audio = qo100::audio;
namespace rtty = qo100::rtty;

struct RttyReceiver::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<audio::RxBlock> pending;
    Output output;
    std::uint64_t generation = 0;
    bool reverse = false, stopping = false;
    audio::AudioSpectrum spectrum;
    std::thread worker{[this] { run(); }};

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        worker.join();
    }

    void discard() {
        ++generation;
        pending.clear();
        output = {};
    }

    void run() {
        std::unique_ptr<rtty::Decoder> decoder;
        std::uint64_t current = 0, next_index = 0, frequency = 0;
        std::optional<std::uint32_t> epoch;
        std::string decoded;
        while (true) {
            audio::RxBlock block;
            std::uint64_t block_generation;
            bool reversed;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !pending.empty(); });
                if (stopping) return;
                block = std::move(pending.front());
                pending.pop_front();
                block_generation = generation;
                reversed = reverse;
            }
            if (!decoder || current != block_generation) {
                current = block_generation;
                decoder = std::make_unique<rtty::Decoder>([&](char c) { decoded += c; },
                                                         rtty::Decoder::Options{reversed});
                spectrum.reset();
                epoch.reset();
            }
            if (epoch && block.epoch == *epoch && block.index < next_index) continue;
            const bool discontinuity = epoch && (block.epoch != *epoch ||
                block.index != next_index || block.frequency_hz != frequency);
            if (discontinuity) {
                decoder->reset();
                spectrum.reset();
            }
            epoch = block.epoch;
            frequency = block.frequency_hz;
            next_index = block.index + block.pcm.size();
            decoded.clear();
            decoder->push(block.pcm.data(), block.pcm.size());
            auto row = spectrum.push(block.pcm);

            if (discontinuity || !decoded.empty() || row) {
                std::lock_guard lock(mutex);
                if (current != generation) continue;
                if (output.events.size() >= 64) {
                    discard();
                    output.events.emplace_back(std::nullopt);
                    continue;
                }
                if (discontinuity) {
                    output.events.emplace_back(std::nullopt);
                    output.spectrum.reset();
                }
                if (!decoded.empty()) {
                    if (output.events.empty() || !output.events.back())
                        output.events.emplace_back(decoded);
                    else
                        *output.events.back() += decoded;
                }
                if (row) output.spectrum = std::move(row);
            }
        }
    }
};

RttyReceiver::RttyReceiver() : impl_(std::make_unique<Impl>()) {}
RttyReceiver::~RttyReceiver() = default;

void RttyReceiver::push(audio::RxBlock block) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->pending.size() == 32) {
            impl_->discard();
            impl_->output.events.emplace_back(std::nullopt);
        }
        impl_->pending.push_back(std::move(block));
    }
    impl_->wake.notify_one();
}

void RttyReceiver::reset(bool reverse) {
    std::lock_guard lock(impl_->mutex);
    impl_->reverse = reverse;
    impl_->discard();
}

RttyReceiver::Output RttyReceiver::take_output() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->output, Output{});
}

void Bridge::subscribe_rtty(uint64_t client, bool enabled) {
    clients_.at(client).rtty_rx = enabled;
    update_rtty_subscription();
}

void Bridge::update_rtty_subscription() {
    const bool active = std::any_of(clients_.begin(), clients_.end(),
        [](const auto& item) { return item.second.rtty_rx; });
    if (active == rtty_active_) return;
    rtty_active_ = active;
    next_activity_ = {};
    reset_rtty();
}

void Bridge::reset_rtty() {
    rtty_receiver_.reset(rtty_reverse_);
    last_rtty_audio_ = {};
    if (rtty_active_ && !stopping_)
        send_rx2({{"type", "rtty_reset"}, {"session", session_}}, &Client::rtty_rx);
}

void Bridge::set_rtty_reverse(uint64_t client, const Json& id, bool enabled) {
    if (enabled != rtty_reverse_) {
        rtty_reverse_ = enabled;
        state_["rtty_reverse"] = enabled;
        dirty_ = true;
        reset_rtty();
    }
    text(client, {{"type", "ack"}, {"command", "rtty_reverse"}, {"id", id}, {"status", 0}});
}

void Bridge::publish_rtty(Clock::time_point now) {
    if (!rtty_active_ || stopping_) return;
    if (last_rtty_audio_ != Clock::time_point{} && now - last_rtty_audio_ > std::chrono::milliseconds(700)) {
        reset_rtty();
        return;
    }
    const auto output = rtty_receiver_.take_output();
    for (const auto& event : output.events) {
        if (event)
            send_rx2({{"type", "rtty_text"}, {"session", session_}, {"text", *event}}, &Client::rtty_rx);
        else
            send_rx2({{"type", "rtty_reset"}, {"session", session_}}, &Client::rtty_rx);
    }
    if (output.spectrum)
        send_rx2_spectrum(rtty_spectrum_type, *output.spectrum, &Client::rtty_rx);
}

}
