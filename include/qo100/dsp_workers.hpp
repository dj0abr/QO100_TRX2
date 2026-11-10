#pragma once
#include "qo100/rx_channel.hpp"
#include <array>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>

namespace qo100 {
// One persistent worker, one outstanding job, no unbounded queue. The caller
// must wait before changing anything referenced by the job or starting another.
class DspWorker {
public:
    explicit DspWorker(const char* name);
    ~DspWorker();
    DspWorker(const DspWorker&) = delete;
    DspWorker& operator=(const DspWorker&) = delete;
    void start(std::function<void()> job);
    void wait(); // Publishes results and propagates worker exceptions.
private:
    void run(const char* name);
    std::mutex mutex_;
    std::condition_variable work_, done_;
    std::function<void()> job_;
    std::exception_ptr error_;
    bool pending_ = false, completed_ = false, stopping_ = false;
    std::thread thread_;
};

// RX1 (including NR), RX2/RX3, RX4/RX5. FFT/network work runs on the caller.
// process() always joins the block, including on exceptions, so control changes
// between calls are safe and the caller can immediately reuse the IQ buffer.
class RxWorkers {
public:
    explicit RxWorkers(std::array<RxChannel*,5> channels);
    void process(const std::complex<float>* samples, size_t count,
                 const std::function<void()>& main_work,uint32_t enabled_mask=31);
private:
    std::array<RxChannel*,5> channels_;
    // Null channels are disabled. Entirely disabled groups create no thread.
    std::array<std::unique_ptr<DspWorker>,3> workers_;
};
}
