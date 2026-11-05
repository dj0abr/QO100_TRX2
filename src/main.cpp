#include "qo100/config.hpp"
#include "qo100/pluto_process.hpp"
#include "qo100/protocol.hpp"
#include "qo100/spectrum.hpp"
#include "qo100/rx_channel.hpp"
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <poll.h>
#include <random>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }
using Clock = std::chrono::steady_clock;
class Socket {
public:
    int fd = -1;
    Socket(const std::string& ip, uint16_t port) {
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0) throw std::runtime_error("Cannot create UDP socket");
        int buffer = 4 * 1024 * 1024;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        sockaddr_in a{};
        a.sin_family = AF_INET; a.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
            const std::string error = std::strerror(errno);
            close(fd); fd = -1;
            throw std::runtime_error("Cannot bind " + ip + ":" + std::to_string(port) + ": " + error);
        }
    }
    ~Socket() { if (fd >= 0) close(fd); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
};
double number(const char* text) {
    size_t count = 0;
    double n = std::stod(text, &count);
    if (count != std::strlen(text) || !std::isfinite(n) || n <= 0) throw std::runtime_error("Expected positive duration");
    return n;
}
}
int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        std::filesystem::path config = QO100_DEFAULT_CONFIG;
        std::string source = "pluto";
        double duration = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "qo100_backend [--config FILE] [--source pluto|synthetic] [--duration SECONDS]\n"
                          << "Pluto mode automatically starts the local RX-only driver. GUI UDP port: config/backend.conf.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::runtime_error("Missing argument for " + arg);
            if (arg == "--config") config = argv[++i];
            else if (arg == "--source") source = argv[++i];
            else if (arg == "--duration") duration = number(argv[++i]);
            else throw std::runtime_error("Unknown argument: " + arg);
        }
        if (source != "pluto" && source != "synthetic") throw std::runtime_error("source must be pluto or synthetic");
        const auto c = qo100::load_config(config);
        struct sigaction action{};
        action.sa_handler = stop;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr); sigaction(SIGTERM, &action, nullptr);
        Socket gui(c.gui_bind, c.gui_port);
        std::unique_ptr<Socket> iq;
        std::unique_ptr<qo100::PlutoProcess> pluto;
        sockaddr_in peer{};
        bool subscribed = false;
        Clock::time_point last_subscription;
        uint32_t frame_id = 0;
        const uint32_t session_id = std::random_device{}();
        uint64_t frames_sent = 0, send_errors = 0, malformed_iq = 0;
        struct ReceiverState {
            uint64_t frequency = 0;
            uint32_t epoch = 1, last_request = 0;
            bool have_request = false;
            std::unique_ptr<qo100::RxChannel> dsp;
        };
        std::array<ReceiverState,2> receivers;
        receivers[0].frequency=c.initial_rx_hz;
        receivers[1].frequency=c.initial_rx2_hz;
        uint32_t zoom_frame_id = 0;
        auto stamp = [] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
        auto active = [&] { return subscribed && std::chrono::duration<double>(Clock::now()-last_subscription).count() <= c.subscription_seconds; };
        auto send_packet = [&](const std::vector<uint8_t>& packet) {
            if (!active()) return;
            if (sendto(gui.fd,packet.data(),packet.size(),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer)) != static_cast<ssize_t>(packet.size())) ++send_errors;
        };
        auto make_zoom = [&] {
            return std::make_unique<qo100::Spectrum>(75000,8192,c.spectrum_fps,
                [&](const std::vector<int16_t>& bins,uint64_t index) {
                    if (!active()) return;
                    qo100::FrameInfo f{zoom_frame_id++,stamp(),index,receivers[0].frequency,75000,8192,session_id,1};
                    for(const auto& packet:qo100::spectrum_packets(f,bins)) send_packet(packet);
                });
        };
        auto zoom = make_zoom();
        for(size_t i=0;i<receivers.size();++i) {
            receivers[i].dsp=std::make_unique<qo100::RxChannel>(
                static_cast<int64_t>(receivers[i].frequency)-static_cast<int64_t>(c.rx_center_hz),
                [&,i](std::complex<float> x) { if(i==0) zoom->push(x); },
                [&,i](const std::vector<int16_t>& audio,uint64_t index) {
                    if(!active()) return;
                    const auto& rx=receivers[i];
                    qo100::FrameInfo f{rx.epoch,stamp(),index,rx.frequency,12000,0,session_id,static_cast<uint32_t>(i+1)};
                    send_packet(qo100::audio_packet(f,audio));
                });
        }
        qo100::Spectrum spectrum(c.iq_rate(), c.fft_size, c.spectrum_fps,
            [&](const std::vector<int16_t>& bins, uint64_t sample_index) {
                if (!active()) return;
                const qo100::FrameInfo info{frame_id++,stamp(),sample_index,c.rx_center_hz,c.iq_rate(),c.fft_size,session_id};
                for (const auto& packet : qo100::spectrum_packets(info,bins)) send_packet(packet);
                ++frames_sent;
            });
        auto feed = [&](std::complex<float> x) { spectrum.push(x); for(auto& rx:receivers) rx.dsp->push(x); };
        auto feed_iq = [&](const uint8_t* data,size_t size) {
            if (!size || size%4) return false;
            for(size_t i=0;i<size;i+=4) {
                const int r=data[i] | (int(data[i+1])<<8), q=data[i+2] | (int(data[i+3])<<8);
                feed({(r>=32768?r-65536:r)/32768.f,(q>=32768?q-65536:q)/32768.f});
            }
            return true;
        };
        if (source == "pluto") {
            // Bind first, before the child can send its first samples; only localhost.
            iq = std::make_unique<Socket>("127.0.0.1", qo100::pluto_iq_port);
            const auto binary = std::filesystem::canonical("/proc/self/exe").parent_path() / "pluto";
            pluto = std::make_unique<qo100::PlutoProcess>(c, binary);
            std::cout << "Started local RX-only Pluto driver; log: " << pluto->log_path() << '\n';
        }
        std::cout << "Source=" << source << "; RX Pluto=" << c.pluto_rx_hz << " Hz; RF center=" << c.rx_center_hz
                  << " Hz; IQ=" << c.iq_rate() << " complex samples/s; FFT=" << c.fft_size << '\n'
                  << "GUI subscribes via UDP " << c.gui_bind << ':' << c.gui_port << "\n";
        const auto start = Clock::now();
        auto next_synthetic = start, last_iq = start, last_stats = start;
        bool received_iq = false;
        uint64_t last_samples = 0;
        std::array<uint8_t, 65536> buffer{};
        while (running && (!duration || std::chrono::duration<double>(Clock::now() - start).count() < duration)) {
            pollfd fds[2]{{gui.fd, POLLIN, 0}, {iq ? iq->fd : -1, POLLIN, 0}};
            if (poll(fds, 2, 10) < 0 && errno != EINTR) throw std::runtime_error("poll failed");
            if (fds[0].revents & POLLIN) {
                for (int budget = 0; budget < 64; ++budget) {
                    sockaddr_in from{};
                    socklen_t len = sizeof(from);
                    auto n = recvfrom(gui.fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
                    if (n < 0) break;
                    if (qo100::is_subscription(buffer.data(), static_cast<size_t>(n))) {
                        if (!subscribed || from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port)
                            std::cout << "GUI subscriber " << inet_ntoa(from.sin_addr) << ':' << ntohs(from.sin_port) << '\n';
                        if (!subscribed || from.sin_addr.s_addr!=peer.sin_addr.s_addr || from.sin_port!=peer.sin_port) for(auto& rx:receivers) rx.have_request=false;
                        peer = from; subscribed = true; last_subscription = Clock::now();
                    } else if (active() && from.sin_addr.s_addr==peer.sin_addr.s_addr && from.sin_port==peer.sin_port) {
                        uint32_t stream_id=0, request=0; uint64_t frequency=0;
                        if(qo100::parse_tune(buffer.data(),static_cast<size_t>(n),stream_id,request,frequency)) {
                            auto& rx=receivers[stream_id-1];
                            if(rx.have_request && request!=rx.last_request && static_cast<int32_t>(request-rx.last_request)<0) continue;
                            const bool valid=frequency>=c.rx_center_hz-275000 && frequency<=c.rx_center_hz+275000;
                            if(valid && frequency!=rx.frequency) {
                                rx.frequency=frequency; ++rx.epoch;
                                rx.dsp->tune(static_cast<int64_t>(rx.frequency)-static_cast<int64_t>(c.rx_center_hz));
                                if(stream_id==1) zoom=make_zoom();
                                std::cout << "RX" << stream_id << " tuned to " << rx.frequency << " Hz; USB +100..4000 Hz; epoch=" << rx.epoch << '\n';
                            }
                            rx.last_request=request;rx.have_request=true;
                            send_packet(qo100::state_packet(request,rx.frequency,rx.epoch,session_id,valid?0:1,stream_id));
                        }
                    }
                }
            }
            if (iq && (fds[1].revents & POLLIN)) {
                for (int budget = 0; budget < 256; ++budget) {
                    sockaddr_in from{};
                    socklen_t len = sizeof(from);
                    auto n = recvfrom(iq->fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
                    if (n < 0) break;
                    if (ntohl(from.sin_addr.s_addr) != INADDR_LOOPBACK || !feed_iq(buffer.data(), static_cast<size_t>(n))) ++malformed_iq;
                    else { received_iq = true; last_iq = Clock::now(); }
                }
            }
            if (source == "synthetic" && Clock::now() >= next_synthetic) {
                // Exact-bin test tones at -150 kHz (-12.04 dBFS) and +75 kHz (-20 dBFS).
                for (uint32_t i = 0; i < c.iq_rate() / 100; ++i) {
                    const double t = double(spectrum.samples()) / c.iq_rate();
                    const auto a = std::polar(0.25f, static_cast<float>(std::remainder(-2 * std::acos(-1.) * 150000 * t, 2 * std::acos(-1.))));
                    const auto b = std::polar(0.1f, static_cast<float>(std::remainder(2 * std::acos(-1.) * 75000 * t, 2 * std::acos(-1.))));
                    const auto usb = std::polar(0.12f,float(std::remainder(-2*std::acos(-1.)*500*t,2*std::acos(-1.))));
                    const auto lsb = std::polar(0.18f,float(std::remainder(-2*std::acos(-1.)*3500*t,2*std::acos(-1.))));
                    const auto usb2=std::polar(0.08f,float(std::remainder(-2*std::acos(-1.)*148300*t,2*std::acos(-1.))));
                    const auto lsb2=std::polar(0.13f,float(std::remainder(-2*std::acos(-1.)*152200*t,2*std::acos(-1.))));
                    feed(a + b + usb + lsb + usb2 + lsb2);
                }
                next_synthetic += std::chrono::milliseconds(10);
            }
            if (pluto) {
                pluto->check();
                const auto idle = std::chrono::duration<double>(Clock::now() - last_iq).count();
                if (idle > (received_iq ? 5 : 20)) throw std::runtime_error("No Pluto IQ samples; see " + pluto->log_path().string());
            }
            const double stats_interval = std::chrono::duration<double>(Clock::now() - last_stats).count();
            if (stats_interval >= 2) {
                std::cout << "IQ samples=" << spectrum.samples() << "; rate=" << (spectrum.samples() - last_samples) / stats_interval
                          << "/s; spectrum frames sent=" << frames_sent << "; malformed IQ=" << malformed_iq << "; send errors=" << send_errors << '\n';
                last_samples = spectrum.samples(); last_stats = Clock::now();
            }
        }
        std::cout << "Stopping; IQ samples=" << spectrum.samples() << "; spectrum frames sent=" << frames_sent << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
