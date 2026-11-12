#include "receiver.hpp"
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#ifdef __linux__
#include <pthread.h>
#endif

namespace qo100::sstv {
namespace {

constexpr std::size_t max_audio_blocks = 32; // 640 ms. Recover at live audio after an overrun.
constexpr std::size_t max_events = 64;
constexpr std::size_t max_gap_samples = sample_rate / 10;
}

struct Receiver::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<AudioBlock> audio;
    Output output;
    std::optional<Mode> mode;
    std::uint64_t generation = 0;
    bool stopping = false;
    audio::AudioSpectrum spectrum;
    std::thread thread{[this] { run(); }};

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        thread.join();
    }

    void discard() {
        ++generation;
        audio.clear();
        output = {};
    }

    void publish(std::uint64_t current, Event event) {
        std::lock_guard lock(mutex);
        if (current != generation) return;
        if (std::holds_alternative<Discontinuity>(event)) output.spectrum.reset();
        if (output.events.size() == max_events) {
            discard();
            output.events.emplace_back(Discontinuity{});
        } else {
            output.events.push_back(std::move(event));
        }
    }

    void run() {
#ifdef __linux__
        pthread_setname_np(pthread_self(), "qo100-sstv-rx");
#endif
        std::unique_ptr<Decoder> decoder;
        std::uint64_t current = 0;
        std::optional<std::uint32_t> epoch;
        std::uint64_t next_index = 0, frequency = 0;
        bool resetting = false;
        const Decoder::Callbacks callbacks{
            [&](Frame frame) { publish(current, frame); },
            [&](const Line& line) { publish(current, line); },
            [&](bool complete) { if (!resetting) publish(current, End{complete}); }
        };
        const std::array<std::int16_t, pcm_block_size> silence{};
        while (true) {
            AudioBlock block;
            std::optional<Mode> forced_mode;
            std::uint64_t next_generation;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || !audio.empty(); });
                if (stopping) return;
                block = std::move(audio.front());
                audio.pop_front();
                next_generation = generation;
                forced_mode = mode;
            }

            if (!decoder || current != next_generation) {
                current = next_generation;
                decoder = std::make_unique<Decoder>(callbacks, forced_mode);
                epoch.reset();
                spectrum.reset();
            }

            if (epoch && *epoch == block.epoch && block.index < next_index) continue;
            if (epoch && (*epoch != block.epoch || block.index - next_index > max_gap_samples)) {
                resetting = true;
                decoder->reset();
                resetting = false;
                spectrum.reset();
                publish(current, Discontinuity{});
                epoch.reset();
            }

            if (epoch) {
                for (; next_index < block.index; next_index += pcm_block_size) {
                    decoder->push(silence.data(), silence.size());
                    spectrum.push(silence);
                }
                if (block.frequency_hz != frequency) {
                    decoder->retune(double(block.frequency_hz) - double(frequency));
                }
            }
            epoch = block.epoch;
            frequency = block.frequency_hz;
            next_index = block.index + pcm_block_size;
            decoder->push(block.pcm.data(), block.pcm.size());
            if (auto row = spectrum.push(block.pcm)) {
                std::lock_guard lock(mutex);
                if (current == generation) output.spectrum = std::move(row);
            }
        }
    }
};

Receiver::Receiver() : impl_(std::make_unique<Impl>()) {}
Receiver::~Receiver() = default;

bool Receiver::push(AudioBlock block) {
    if (block.index % pcm_block_size || block.index > std::numeric_limits<std::uint64_t>::max() - pcm_block_size)
        return false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->audio.size() == max_audio_blocks) {
            impl_->discard();
            impl_->output.events.emplace_back(Discontinuity{});
        }
        impl_->audio.push_back(std::move(block));
    }
    impl_->wake.notify_one();
    return true;
}

void Receiver::reset(std::optional<Mode> forced_mode) {
    std::lock_guard lock(impl_->mutex);
    impl_->mode = forced_mode;
    impl_->discard();
}

Receiver::Output Receiver::take_output() {
    std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->output, Output{});
}

}
