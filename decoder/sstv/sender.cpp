#include "sender.hpp"

#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#ifdef __linux__
#include <pthread.h>
#endif

namespace qo100::sstv {

struct Sender::Impl {
    Encoder encoder;
    mutable std::mutex mutex;
    std::condition_variable space;
    std::array<Block, 16> queue{};
    std::optional<double> output_gain;
    std::size_t next = 0, size = 0;
    bool stopped = false;
    std::thread worker;

    Impl(Mode mode, const Image& image)
        : encoder(mode, image), worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopped = true;
        }
        space.notify_one();
        worker.join();
    }

    void run() {
#ifdef __linux__
        pthread_setname_np(pthread_self(), "qo100-sstv-tx");
#endif
        // Leave enough silence for the backend audio FIFO, interpolator and
        // Pluto queue to play the entire final line before TX2 is released.
        unsigned tail = 0;
        while (true) {
            Block block;
            if (tail || encoder.read(block.pcm.data(), block.pcm.size()) < block.pcm.size())
                block.last = ++tail == 13;
            std::unique_lock lock(mutex);
            space.wait(lock, [this] { return stopped || size < queue.size(); });
            if (stopped) return;
            queue[(next + size++) % queue.size()] = block;
            if (block.last) return;
        }
    }
};

Sender::Sender(Mode mode, const Image& image)
    : impl_(std::make_unique<Impl>(mode, image)) {}

Sender::~Sender() = default;

std::optional<Sender::Block> Sender::take(unsigned level_percent) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->size) return {};
    auto block = impl_->queue[impl_->next];
    impl_->next = (impl_->next + 1) % impl_->queue.size();
    --impl_->size;
    impl_->space.notify_one();

    // Ramp across one block to avoid an abrupt change in the waveform.
    const double target_gain = level_percent / 100.0;
    double gain = impl_->output_gain.value_or(target_gain);
    const double gain_step = (target_gain - gain) / block.pcm.size();
    for (auto& sample : block.pcm) {
        gain += gain_step;
        sample = std::int16_t(std::lround(sample * gain));
    }
    impl_->output_gain = target_gain;
    return block;
}

std::size_t Sender::available() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->size;
}

}
