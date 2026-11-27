#include "ft_audio.hpp"
#include "wire.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono;
constexpr auto utc = ft8::Time(seconds(1700000010));
constexpr uint64_t frequency = 10489610000;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

qo100::FtPcm block(uint64_t i)
{
    const auto timestamp = utc + (ft8::sample_time(i + 240) - ft8::Time{});
    return {9, 7, i, frequency, duration_cast<nanoseconds>(timestamp.time_since_epoch()).count(), {}};
}

void sample_clock()
{
    qo100::FtSampleClock clock(7ms, 1200);
    const auto zero = ft8::sample_index(utc + 7ms);
    const auto a = clock.push(block(0));
    require(a && a->first == zero && !a->discontinuity, "Invalid first packet anchor");
    auto later = block(960);
    later.packet_ns += 5'000'000;
    const auto b = clock.push(later);
    require(b && b->first == zero + 960 && !b->discontinuity, "Jitter or short gap moved the clock");
    require(!clock.push(block(240)), "Older packet advanced the clock");
    auto retune = block(1200);
    retune.frequency += 5000;
    require(clock.push(retune)->discontinuity, "Retune with same epoch was ignored");
    auto step = retune;
    step.index += 240;
    step.packet_ns += 1'020'000'000;
    require(clock.push(step)->discontinuity, "UTC step did not re-anchor");
    auto epoch = block(0);
    ++epoch.epoch;
    require(clock.push(epoch)->discontinuity, "New epoch did not reset the sample index");
    require(!clock.push(block(5000)), "Old epoch undid reset");
    qo100::FtSampleClock monitor;
    monitor.push(block(0));
    require(monitor.push(block(480))->discontinuity, "RX3 must discard slots after a gap");
}
void wire()
{
    auto packet = bridge::header(5, 5, 7, 9);
    packet.resize(544);
    bridge::put(packet, 16, block(0).packet_ns, 8);
    bridge::put(packet, 32, frequency, 8);
    bridge::put(packet, 40, 12000, 4);
    bridge::put(packet, 52, 240, 2);
    bridge::put(packet, 54, 2, 2);
    bridge::put(packet, 56, 240, 4);
    bridge::put(packet, 64, 32768, 2);
    bridge::put(packet, 66, 65535, 2);
    bridge::put(packet, 70, 32767, 2);
    const auto block = qo100::ft_pcm(packet, 5);
    require(block && block->frequency == frequency && block->samples[0] == -32768 &&
            block->samples[1] == -1 && block->samples[2] == 0 && block->samples[3] == 32767,
            "Big-endian PCM16 conversion failed");
    require(!qo100::ft_pcm(packet, 2), "Invalid RX stream accepted");
    bridge::put(packet, 24, UINT64_MAX, 8);
    require(!qo100::ft_pcm(packet, 5), "Invalid sample index accepted");
    packet.pop_back();
    require(!qo100::ft_pcm(packet, 5), "Short packet accepted");
}
}

int main() try
{
    sample_clock();
    wire();
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
