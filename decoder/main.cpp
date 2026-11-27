#include "ft_audio.hpp"
#include "cpu_budget.hpp"
#include "qo100/channels.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace std::chrono;
using Steady = steady_clock;
constexpr auto mode = ft8::Mode::FT8;
constexpr std::size_t slot_samples = 15 * ft8::sample_rate;
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> running{true};
void stop(int) { running.store(false, std::memory_order_relaxed); }
struct Socket {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    ~Socket() { if (fd >= 0) close(fd); }
};

unsigned number(const std::string& text, unsigned maximum)
{
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("Expected positive integer: " + text);
    const auto value = std::stoul(text);
    if (!value || value > maximum) throw std::runtime_error("Number out of range: " + text);
    return unsigned(value);
}

nanoseconds time_offset(const std::string& text)
{
    std::size_t used = 0;
    const auto value = std::stod(text, &used);
    if (used != text.size() || !std::isfinite(value) || std::abs(value) > 5000)
        throw std::runtime_error("FT8 time offset must be within +/-5000 ms");
    return nanoseconds(std::llround(value * 1e6));
}

std::string decode_line(const ft8::Decode& row)
{
    const auto time = hh_mm_ss(floor<seconds>(row.slot - floor<days>(row.slot)));
    std::ostringstream text;
    text << std::setfill('0') << std::setw(2) << time.hours().count() << std::setw(2) << time.minutes().count()
         << std::setw(2) << time.seconds().count() << std::setfill(' ') << ' ' << std::setw(3) << row.snr_db
         << ' ' << std::fixed << std::setprecision(3) << std::setw(7) << row.dt_seconds
         << ' ' << std::setw(8) << row.frequency_hz << " ~ " << row.message.text << '\n';
    return text.str();
}

class Monitor {
public:
    Monitor(int socket, nanoseconds offset, milliseconds timeout)
        : socket_(socket), clock_(offset), timeout_(timeout), worker_([this] { decode(); }) {}
    ~Monitor()
    {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_one();
    }

    void push(const qo100::FtPcm& block, const sockaddr_in& from)
    {
        const bool peer_change = from.sin_port != peer_port_;
        if (peer_change) {
            clock_.reset();
            peer_port_ = from.sin_port;
        }
        const auto position = clock_.push(block);
        if (!position) return;
        if (peer_change || position->discontinuity) {
            partial_.clear();
            std::lock_guard lock(mutex_);
            ++generation_;
            pending_.reset();
        }

        for (std::size_t used = 0; used < block.samples.size();) {
            const auto first = position->first + std::int64_t(used);
            const auto slot = ft8::slot_start(mode, ft8::sample_time(first));
            const auto offset = std::size_t(first - ft8::sample_index(slot));
            const auto count = std::min(slot_samples - offset, block.samples.size() - used);
            if (!offset) {
                partial_.clear();
                partial_.reserve(slot_samples);
            }
            if (partial_.size() == offset) {
                const auto start = block.samples.begin() + used;
                partial_.insert(partial_.end(), start, start + count);
                if (partial_.size() == slot_samples) {
                    std::lock_guard lock(mutex_);
                    pending_ = Slot{slot, from, generation_, Steady::now() + timeout_, std::move(partial_)};
                    partial_.clear();
                    ready_.notify_one();
                }
            }
            used += count;
        }
    }

private:
    struct Slot {
        ft8::Time utc;
        sockaddr_in peer;
        std::uint64_t generation;
        Steady::time_point deadline;
        std::vector<ft8::Sample> audio;
    };

    void decode()
    {
        for (;;) {
            std::optional<Slot> slot;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return stopping_ || pending_; });
                if (stopping_) return;
                slot = std::exchange(pending_, std::nullopt);
            }
            if (Steady::now() >= slot->deadline) continue;
            try {
                const auto rows = ft8::decode(mode, slot->audio, {200, 4000, 1}, slot->utc);
                std::lock_guard lock(mutex_);
                if (stopping_ || slot->generation != generation_ || Steady::now() >= slot->deadline) continue;
                for (const auto& row : rows) {
                    const auto line = decode_line(row);
                    if (sendto(socket_, line.data(), line.size(), MSG_DONTWAIT,
                               reinterpret_cast<const sockaddr*>(&slot->peer), sizeof(slot->peer)) < 0)
                        std::cerr << "[FT8] Could not send decoder text\n";
                }
            } catch (const std::exception& error) {
                std::cerr << "[FT8] " << error.what() << '\n';
            }
        }
    }

    const int socket_;
    qo100::FtSampleClock clock_;
    const milliseconds timeout_;
    in_port_t peer_port_ = 0;
    std::vector<ft8::Sample> partial_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::uint64_t generation_ = 0;
    std::optional<Slot> pending_;
    bool stopping_ = false;
    std::jthread worker_;
};
}

int main(int argc, char** argv)
{
    try {
        unsigned port = 40910, timeout_ms = 45000, cpu_percent = 10;
        nanoseconds offset{};
        if (const char* value = std::getenv("FT8_TIME_OFFSET_MS")) offset = time_offset(value);
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                std::cout << "qo100_ft8_decoder [--port PORT] [--time-offset-ms 0]\n"
                             "                  [--decode-timeout-ms 45000] [--cpu-percent 10]\n"
                             "Decodes UTC-aligned RX3 audio on loopback UDP using libft8.\n";
                return 0;
            }
            if (++i == argc) throw std::runtime_error("Missing value for " + option);
            if (option == "--port") port = number(argv[i], 65535);
            else if (option == "--time-offset-ms") offset = time_offset(argv[i]);
            else if (option == "--cpu-percent") cpu_percent = number(argv[i], 100);
            else if (option == "--decode-timeout-ms") timeout_ms = number(argv[i], 120000);
            else throw std::runtime_error("Unknown argument: " + option);
        }
        if (auto exit = qo100::decoder_cpu_budget(cpu_percent)) return *exit;
        Socket audio;
        if (audio.fd < 0) throw std::runtime_error("Cannot create FT8 UDP socket");
        const int capacity = 4 * 1024 * 1024;
        (void)setsockopt(audio.fd, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity));
        if (fcntl(audio.fd, F_SETFL, O_NONBLOCK) < 0) throw std::runtime_error("Cannot set FT8 UDP nonblocking mode");
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(port);
        if (bind(audio.fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0)
            throw std::runtime_error("Cannot bind FT8 UDP port " + std::to_string(port));
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        Monitor monitor(audio.fd, offset, milliseconds(timeout_ms));
        std::cerr << "FT8 receiver: RX3 UDP 127.0.0.1:" << port << "; libft8; UTC offset "
                  << duration<double, std::milli>(offset).count() << " ms\n";
        std::array<std::uint8_t, 1201> buffer{};
        while (running.load(std::memory_order_relaxed)) {
            pollfd descriptor{audio.fd, POLLIN, 0};
            const auto polled = poll(&descriptor, 1, 100);
            if (polled < 0 && errno != EINTR) throw std::runtime_error("FT8 UDP poll failed");
            if (polled <= 0 || !(descriptor.revents & POLLIN)) continue;
            for (unsigned budget = 0; budget < 256 && running.load(std::memory_order_relaxed); ++budget) {
                sockaddr_in from{};
                socklen_t size = sizeof(from);
                const auto count = recvfrom(audio.fd, buffer.data(), buffer.size(), 0,
                                            reinterpret_cast<sockaddr*>(&from), &size);
                if (count < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
                    throw std::runtime_error("FT8 UDP receive failed");
                }
                if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
                const auto block = qo100::ft_pcm(std::span(buffer).first(count), qo100::rx3_stream);
                if (block) monitor.push(*block, from);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FT8 receiver: " << error.what() << '\n';
        return 1;
    }
}
