#include "qo100/config.hpp"
#include "qo100/protocol.hpp"
#include "qo100/spectrum.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<typename F> void rejects(F action, const char* message) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
int main() {
    try {
        constexpr size_t n = 8192;
        unsigned frames = 0;
        qo100::Spectrum s(600000, n, 20, [&](const auto& db, uint64_t index) {
            ++frames;
            const auto peak = size_t(std::max_element(db.begin(), db.end()) - db.begin());
            require(peak == n / 2 + 1024, "Wrong signed frequency / FFT shift");
            require(std::abs(db[peak] + 602) <= 2, "Hann coherent gain normalization is wrong");
            require(index >= 30000, "FFT frame published too soon");
        });
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i < 90000; ++i) {
            double phase = 2 * std::acos(-1.) * 1024 * i / n;
            for (const double x : {std::cos(phase), std::sin(phase)}) {
                auto value = static_cast<uint16_t>(static_cast<int16_t>(std::lround(x * 16384)));
                bytes.push_back(value & 255); bytes.push_back(value >> 8);
            }
        }
        // The upstream interface permits a short final datagram. Packet edges must not reset FFT state.
        for (size_t i = 0; i < bytes.size(); i += 32768)
            require(s.push_iq(bytes.data() + i, std::min<size_t>(32768, bytes.size() - i)), "Valid IQ rejected");
        require(frames >= 2 && s.samples() == 90000, "Lost samples across datagrams");
        require(!s.push_iq(bytes.data(), 3) && s.samples() == 90000, "Malformed IQ accepted");
        unsigned zero_frames = 0;
        qo100::Spectrum zero(600000, n, 20, [&](const auto& db, uint64_t) {
            ++zero_frames;
            require(std::all_of(db.begin(), db.end(), [](auto x) { return x == -16000; }), "Invalid zero-input floor");
        });
        for (int i = 0; i < 40000; ++i) zero.push({0, 0});
        require(zero_frames > 0, "Zero input produced no FFT frame");
        qo100::Spectrum reset_spectrum(600000,n,20,[&](const auto& db,uint64_t) {
            require(std::all_of(db.begin(),db.end(),[](auto x){return x==-16000;}),"Spectrum reset leaked preceding RF band");
        });
        for(int i=0;i<10000;++i) reset_spectrum.push({.5f,0});
        reset_spectrum.reset();
        for(int i=0;i<40000;++i) reset_spectrum.push({});
        require(reset_spectrum.samples()==50000,"Spectrum reset restarted sample clock");
        qo100::Config c;
        c.validate();
        require(c.tx1_compressor.amount==1000,"Default speech compressor must be 100 percent");
        c.gui_port = qo100::pluto_iq_port;
        rejects([&] { c.validate(); }, "Port conflict accepted");
        c.gui_port = 40900; c.fft_size = 8193;
        rejects([&] { c.validate(); }, "Invalid FFT size accepted");
        auto path = std::filesystem::temp_directory_path() / ("qo100-test-" + std::to_string(getpid()) + ".conf");
        { std::ofstream file(path); file << "gui_port=70000\n"; }
        rejects([&] { qo100::load_config(path); }, "Out-of-range UDP port wrapped");
        { std::ofstream file(path); file << "rx_center_hz=10489750000\n"; }
        require(qo100::load_config(path).rx_center_hz == 10489750000ULL, "RF frequency overflow");
        { std::ofstream file(path); file << "fft_size=8192\nfft_size=4096\n"; }
        rejects([&] { qo100::load_config(path); }, "Duplicate config accepted");
        { std::ofstream file(path); file << "rx1_filter_bandwidth_hz=500\nrx1_filter_center_hz=700\n"; }
        const auto filtered_config=qo100::load_config(path);
        require(filtered_config.rx1_filter==qo100::RxFilterSettings{500,700},"RX1 filter config not parsed");
        { std::ofstream file(path); file << "rx1_filter_bandwidth_hz=3900\nrx1_filter_center_hz=700\n"; }
        rejects([&] { qo100::load_config(path); }, "Invalid filter config accepted");
        { std::ofstream file(path); file << "tx1_compressor_percent=137.5\n"; }
        require(qo100::load_config(path).tx1_compressor.amount==1375,"Fractional compressor amount not parsed");
        for(const char* bad:{"-1","140.1","nan","inf","100percent"}) {
            { std::ofstream file(path); file << "tx1_compressor_percent="<<bad<<'\n'; }
            rejects([&] { qo100::load_config(path); },"Invalid compressor config accepted");
        }
        std::filesystem::remove(path);
        std::vector<uint8_t> subscription(64, 0);
        subscription[0]='Q'; subscription[1]='T'; subscription[2]='R'; subscription[3]='2';
        subscription[4]=1; subscription[5]=1; subscription[7]=64;
        require(qo100::is_subscription(subscription.data(), subscription.size()), "Valid subscription rejected");
        subscription[8]=1;
        require(!qo100::is_subscription(subscription.data(), subscription.size()), "Unknown subscribe fields accepted");
        qo100::FrameInfo info{0x12345678, 123, 456, 10489750000ULL, 600000, 8192, 99};
        const auto packets = qo100::spectrum_packets(info, std::vector<int16_t>(8192, -602));
        require(packets.size() == 15, "Unexpected fragment count");
        require(packets.front().size() == 1200 && packets.back().size() <= 1200, "Oversized UDP packet");
        require(packets[0][12] == 0x12 && packets[0][15] == 0x78, "Wrong network byte order");
        require(packets[0][64] == 0xfd && packets[0][65] == 0xa6, "Wrong signed dB encoding");
        require(packets[0][63] == 99, "Session ID missing");
        std::cout << "Core: tone frequency, dBFS, packet boundaries, zero input, config and wire format passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
