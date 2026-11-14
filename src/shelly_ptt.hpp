#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <string>
#include <thread>

namespace qo100 {
class ShellyPtt {
public:
    using Clock = std::chrono::steady_clock;

    explicit ShellyPtt(const std::string& ip);
    ~ShellyPtt();

    void configure(const std::string& ip);
    void request(bool on);
    bool ready(Clock::time_point now, std::chrono::milliseconds lead) const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::string ip_;
    uint64_t revision_ = 0;
    bool requested_ = false, stopping_ = false;
    Clock::time_point on_since_{}, valid_until_{};
    std::thread worker_;
};
}
