#include "qo100/config.hpp"
#include "qo100/settings.hpp"
#include "qo100/pluto_process.hpp"
#include "qo100/protocol.hpp"
#include "qo100/spectrum.hpp"
#include "qo100/rx_channel.hpp"
#include "qo100/tx_channel.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <poll.h>
#include <random>
#include <set>
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
        std::filesystem::path settings_path;
        std::string source = "pluto";
        double duration = 0;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "qo100_backend [--config FILE] [--settings FILE] [--source pluto|synthetic] [--duration SECONDS]\n"
                          << "Pluto mode automatically starts the local full-duplex driver. GUI UDP port: config/backend.conf.\n";
                return 0;
            }
            if (i + 1 >= argc) throw std::runtime_error("Missing argument for " + arg);
            if (arg == "--config") config = argv[++i];
            else if (arg == "--settings") settings_path = argv[++i];
            else if (arg == "--source") source = argv[++i];
            else if (arg == "--duration") duration = number(argv[++i]);
            else throw std::runtime_error("Unknown argument: " + arg);
        }
        if (source != "pluto" && source != "synthetic") throw std::runtime_error("source must be pluto or synthetic");
        if(settings_path.empty()) { settings_path=config;settings_path.replace_extension(".settings"); }
        qo100::SettingsStore settings(qo100::load_config(config),settings_path);
        auto& c=settings.config();
        std::cout << "Persistent settings: " << std::filesystem::absolute(settings_path) << '\n';
        if (liquid_libversion_number()!=LIQUID_VERSION_NUMBER) throw std::runtime_error("Liquid header/library mismatch");
        std::cout << "DSP: liquid-dsp " << liquid_libversion() << " (bundled, static)\n";
        struct sigaction action{};
        action.sa_handler = stop;
        sigemptyset(&action.sa_mask);
        sigaction(SIGINT, &action, nullptr); sigaction(SIGTERM, &action, nullptr);
        Socket gui(c.gui_bind, c.gui_port);
        std::unique_ptr<Socket> iq, tx_status;
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
            uint16_t command_status = 0;
            std::unique_ptr<qo100::RxChannel> dsp;
            std::unique_ptr<qo100::Spectrum> zoom;
            uint32_t zoom_frame_id = 0;
        };
        std::array<ReceiverState,2> receivers;
        receivers[0].frequency=c.initial_rx_hz;
        receivers[1].frequency=c.initial_rx2_hz;
        qo100::TxChannel tx(150000,c.tx1_compressor);
        uint64_t tx_frequency=c.initial_tx_hz, tx_blocks=0;
        uint32_t tx_epoch=1,tx_request=0,tx_block_id=0,status_sequence=0;
        bool tx_enabled=false,tx_have_request=false,have_status=false;
        std::set<uint64_t> echo_seen;
        uint64_t echo_next=0;
        int pluto_fill=0;
        uint16_t tx_command_status=0;
        auto last_tx_audio=Clock::now()-std::chrono::seconds(10),last_tx_control=last_tx_audio;
        auto last_tx_feedback=Clock::now(),last_tx_state=last_tx_audio;
        auto mute_tx=[&] { if(tx_enabled) { tx_enabled=false;++tx_epoch;echo_seen.clear();echo_next=0;tx.reset(int64_t(tx_frequency)-int64_t(c.tx_center_hz)); } };
        tx.reset(int64_t(tx_frequency)-int64_t(c.tx_center_hz));
        uint32_t filter_request=0;
        uint16_t filter_status=0;
        bool have_filter_request=false;
        uint32_t nr_request=0;
        uint16_t nr_status=0;
        bool have_nr_request=false;
        uint32_t compressor_request=0;
        uint16_t compressor_status=0;
        bool have_compressor_request=false;
        uint32_t settings_request=0;
        bool have_settings_request=false;
        uint16_t settings_status=0;
        std::string settings_error,driver_error;
        std::optional<qo100::Config> pending_settings;
        bool settings_rollback=false,hardware_online=source=="synthetic",hardware_recovering=false;
        using TestLoop=qo100::TestLoop;
        TestLoop loop_mode=TestLoop::normal,loop_target=TestLoop::normal;
        uint32_t loop_request=0,loop_generation=0;
        uint16_t loop_status=0;
        bool have_loop_request=false,loop_pending=false;
        uint64_t current_rx_lo=c.pluto_rx_hz,rx_lo_target=c.pluto_rx_hz;
        auto rx_frequency=[&](size_t i) {
            return i==0 && loop_mode==TestLoop::rf
                ? uint64_t(int64_t(c.rx_center_hz)+int64_t(tx_frequency)-int64_t(c.tx_center_hz))
                : receivers[i].frequency;
        };
        auto stamp = [] { return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); };
        auto active = [&] { return subscribed && std::chrono::duration<double>(Clock::now()-last_subscription).count() <= c.subscription_seconds; };
        auto send_packet = [&](const std::vector<uint8_t>& packet) {
            if (!active()) return;
            if (sendto(gui.fd,packet.data(),packet.size(),0,reinterpret_cast<const sockaddr*>(&peer),sizeof(peer)) != static_cast<ssize_t>(packet.size())) ++send_errors;
        };
        auto send_tx_state=[&]() {
            send_packet(qo100::tx_state_packet(tx_request,tx_frequency,tx_epoch,session_id,tx_command_status,
                tx_enabled,tx.level(),pluto_fill,int32_t(std::lround(tx.ppm()))));
            last_tx_state=Clock::now();
        };
        auto send_rx_state=[&](size_t i,uint16_t status=0) {
            const auto& rx=receivers[i];
            if(rx.have_request) send_packet(qo100::state_packet(rx.last_request,rx_frequency(i),rx.epoch,session_id,status,i+1));
        };
        auto send_loop_state=[&]() {
            send_packet(qo100::loop_state_packet(loop_request,loop_mode,loop_target,current_rx_lo,
                loop_generation,session_id,loop_status,stamp()));
        };
        auto send_settings=[&](std::optional<uint32_t> reply_request=std::nullopt,std::optional<uint16_t> reply_status=std::nullopt) {
            auto values=qo100::settings_values(c);
            values["settings_schema"]="1";values["pluto_rx_hz"]=std::to_string(c.pluto_rx_hz);
            values["pluto_tx_hz"]=std::to_string(c.pluto_tx_hz);values["rx_center_hz"]=std::to_string(c.rx_center_hz);
            values["tx_center_hz"]=std::to_string(c.tx_center_hz);values["hardware_online"]=hardware_online?"1":"0";
            const auto error=settings_error.empty()?settings.error():settings_error;
            if(!error.empty()) values["error"]=error.substr(0,160);
            else if(!driver_error.empty()) values["hardware_error"]=driver_error.substr(0,160);
            send_packet(qo100::settings_state_packet(reply_request.value_or(settings_request),session_id,settings.revision(),
                reply_status.value_or(settings_status), uint32_t(settings.dirty())|(pending_settings?2:0)|(!hardware_online?4:0),qo100::encode_settings(values)));
        };
        sockaddr_in driver{};driver.sin_family=AF_INET;driver.sin_port=htons(qo100::pluto_tx_port);
        driver.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        auto render_tx=[&] {
            const bool recent=tx_enabled && active() && loop_mode!=TestLoop::audio && !loop_pending
                && Clock::now()-last_tx_audio<std::chrono::milliseconds(200);
            const auto samples=tx.render(recent);
            if(tx_status) {
                constexpr size_t chunk=12000;
                for(size_t start=0;start<samples.size();start+=chunk) {
                    const size_t count=std::min(chunk,samples.size()-start);
                    std::vector<uint8_t> p(16+count*4);
                    const uint32_t h[4]={htonl(0x50545849),htonl(tx_block_id),htonl(start*4),htonl(samples.size()*4)};
                    std::memcpy(p.data(),h,16);
                    for(size_t i=0;i<count;++i) {
                        const auto x=samples[start+i];
                        const uint16_t v[2]={uint16_t(int16_t(std::lround(std::clamp(x.real(),-1.f,1.f)*32767))),
                                             uint16_t(int16_t(std::lround(std::clamp(x.imag(),-1.f,1.f)*32767)))};
                        for(size_t k=0;k<2;++k) { p[16+4*i+2*k]=v[k]&255;p[17+4*i+2*k]=v[k]>>8; }
                    }
                    if(sendto(tx_status->fd,p.data(),p.size(),0,reinterpret_cast<sockaddr*>(&driver),sizeof(driver))!=ssize_t(p.size())) ++send_errors;
                }
            }
            ++tx_block_id;++tx_blocks;
        };
        auto make_zoom = [&](size_t i) {
            return std::make_unique<qo100::Spectrum>(75000,8192,c.spectrum_fps,
                [&,i](const std::vector<int16_t>& bins,uint64_t index) {
                    if (!active()) return;
                    qo100::FrameInfo f{receivers[i].zoom_frame_id++,stamp(),index,rx_frequency(i),75000,8192,session_id,static_cast<uint32_t>(i+1)};
                    for(const auto& packet:qo100::spectrum_packets(f,bins)) send_packet(packet);
                });
        };
        for(size_t i=0;i<receivers.size();++i) {
            receivers[i].zoom=make_zoom(i);
            receivers[i].dsp=std::make_unique<qo100::RxChannel>(
                static_cast<int64_t>(receivers[i].frequency)-static_cast<int64_t>(c.rx_center_hz),
                [&,i](std::complex<float> x) { receivers[i].zoom->push(x); },
                [&,i](const std::vector<int16_t>& audio,uint64_t index) {
                    if(!active()) return;
                    const auto& rx=receivers[i];
                    qo100::FrameInfo f{rx.epoch,stamp(),index,rx_frequency(i),12000,0,session_id,static_cast<uint32_t>(i+1)};
                    send_packet(qo100::audio_packet(f,audio));
                }, i==0 ? c.rx1_filter : qo100::RxFilterSettings{},
                i==0 ? std::optional<uint32_t>(c.rx1_nr_percent) : std::nullopt);
        }
        std::cout << "RX1 NR: SpeexDSP 1.2.1 (bundled, static); strength=" << c.rx1_nr_percent << " percent\n";
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
            tx_status = std::make_unique<Socket>("127.0.0.1",qo100::pluto_status_port);
            iq = std::make_unique<Socket>("127.0.0.1", qo100::pluto_iq_port);
            const auto binary = std::filesystem::canonical("/proc/self/exe").parent_path() / "pluto";
            pluto = std::make_unique<qo100::PlutoProcess>(c, binary);
            std::cout << "Started local full-duplex Pluto driver; log: " << pluto->log_path() << '\n';
        }
        std::cout << "Source=" << source << "; RX Pluto=" << c.pluto_rx_hz << " Hz; RF center=" << c.rx_center_hz
                  << " Hz; IQ=" << c.iq_rate() << " complex samples/s; FFT=" << c.fft_size << '\n'
                  << "GUI subscribes via UDP " << c.gui_bind << ':' << c.gui_port << "\n";
        const auto start = Clock::now();
        auto next_synthetic = start, next_tx=start, last_iq = start, last_stats = start;
        auto next_hardware_retry=start;
        bool received_iq = false;
        uint64_t last_samples = 0;
        std::array<uint8_t, 65536> buffer{};
        auto reset_rx=[&](size_t i) {
            auto& rx=receivers[i];++rx.epoch;
            rx.dsp->tune(int64_t(rx_frequency(i))-int64_t(c.rx_center_hz));
            rx.zoom=make_zoom(i);
            send_rx_state(i);
        };
        auto retune_rx=[&](size_t i) {
            // A dial change preserves the PCM timeline and the receiver's DSP state.
            // Changing the epoch here would force GUI jitter/ALSA buffers to flush.
            receivers[i].dsp->retune(int64_t(rx_frequency(i))-int64_t(c.rx_center_hz));
        };
        auto apply_runtime=[&](const qo100::Config& old,bool reset_all) {
            for(size_t i=0;i<receivers.size();++i) {
                const auto hz=i?c.initial_rx2_hz:c.initial_rx_hz;
                const bool changed=hz!=receivers[i].frequency;
                receivers[i].frequency=hz;
                if(reset_all) reset_rx(i);
                else if(changed) {retune_rx(i);send_rx_state(i);}
            }
            if(old.rx1_filter!=c.rx1_filter) receivers[0].dsp->set_filter(c.rx1_filter);
            if(old.rx1_nr_percent!=c.rx1_nr_percent) receivers[0].dsp->set_nr(c.rx1_nr_percent);
            if(old.tx1_compressor.amount!=c.tx1_compressor.amount) tx.set_compressor(c.tx1_compressor);
            if(tx_frequency!=c.initial_tx_hz) {
                tx_frequency=c.initial_tx_hz;tx_enabled=false;++tx_epoch;
                echo_seen.clear();echo_next=0;
                tx.reset(int64_t(tx_frequency)-int64_t(c.tx_center_hz));send_tx_state();
                if(loop_mode==TestLoop::rf) {retune_rx(0);send_rx_state(0);}
            }
            if(reset_all) spectrum.reset();
        };
        auto restart_driver=[&](const qo100::Config& next) {
            tx_enabled=false;++tx_epoch;echo_seen.clear();echo_next=0;
            tx.reset(int64_t(tx_frequency)-int64_t(c.tx_center_hz));send_tx_state();
            pluto.reset();hardware_online=false;
            hardware_recovering=!pending_settings.has_value();
            while(recv(iq->fd,buffer.data(),buffer.size(),0)>=0) {}
            while(recv(tx_status->fd,buffer.data(),buffer.size(),0)>=0) {}
            have_status=false;pluto_fill=0;received_iq=false;
            last_iq=last_tx_feedback=Clock::now();
            const auto binary=std::filesystem::canonical("/proc/self/exe").parent_path()/"pluto";
            pluto=std::make_unique<qo100::PlutoProcess>(next,binary);
        };
        auto fail_settings=[&](const std::string& error) {
            settings_error=error;settings_status=5;
            if(!settings_rollback) {
                settings_rollback=true;
                try {restart_driver(c);return;} catch(const std::exception& e) {driver_error=e.what();}
            }
            pending_settings.reset();settings_rollback=false;pluto.reset();hardware_online=false;
            next_hardware_retry=Clock::now()+std::chrono::seconds(5);send_settings();
        };
        auto commit_loop=[&]() {
            const bool restarted=loop_pending;
            const bool routing_changed=(loop_mode==TestLoop::rf)!=(loop_target==TestLoop::rf);
            loop_mode=loop_target;loop_pending=false;if(loop_status==3) loop_status=0;++loop_generation;
            const bool hardware_changed=restarted || current_rx_lo!=rx_lo_target;
            current_rx_lo=rx_lo_target;
            send_loop_state();
            if(hardware_changed) {
                spectrum.reset();
                for(size_t i=0;i<receivers.size();++i) reset_rx(i);
            } else if(routing_changed) reset_rx(0);
            std::cout << "Test loop=" << uint32_t(loop_mode) << "; Pluto RX LO=" << current_rx_lo << " Hz\n";
        };
        auto change_loop=[&](TestLoop target) {
            if(loop_target==target && (loop_pending || loop_mode==target)) return;
            loop_target=target;
            // Changing a test path always requires a fresh manual PTT press.
            tx_enabled=false;++tx_epoch;echo_seen.clear();echo_next=0;tx_command_status=0;
            tx.reset(int64_t(tx_frequency)-int64_t(c.tx_center_hz));send_tx_state();
            const uint64_t wanted_lo=target==TestLoop::rf ? c.pluto_tx_hz : c.pluto_rx_hz;
            if(wanted_lo!=rx_lo_target) {
                loop_pending=true;loop_status=3;send_loop_state();
                pluto.reset(); // Driver has startup-only LO configuration.
                // Remove old IQ and FIFO reports before starting the new child.
                while(recv(iq->fd,buffer.data(),buffer.size(),0)>=0) {}
                while(recv(tx_status->fd,buffer.data(),buffer.size(),0)>=0) {}
                auto settings=c;settings.pluto_rx_hz=wanted_lo;
                const auto binary=std::filesystem::canonical("/proc/self/exe").parent_path()/"pluto";
                pluto=std::make_unique<qo100::PlutoProcess>(settings,binary);
                rx_lo_target=wanted_lo;have_status=false;pluto_fill=0;
                last_iq=last_tx_feedback=Clock::now();received_iq=false;
                // Commit only after fresh IQ proves the restarted driver is streaming.
            } else if(!loop_pending) commit_loop();
        };
        while (running && (!duration || std::chrono::duration<double>(Clock::now() - start).count() < duration)) {
            pollfd fds[3]{{gui.fd, POLLIN, 0}, {iq ? iq->fd : -1, POLLIN, 0}, {tx_status ? tx_status->fd : -1,POLLIN,0}};
            if (poll(fds, 3, 5) < 0 && errno != EINTR) throw std::runtime_error("poll failed");
            if (fds[0].revents & POLLIN) {
                for (int budget = 0; budget < 64; ++budget) {
                    sockaddr_in from{};
                    socklen_t len = sizeof(from);
                    auto n = recvfrom(gui.fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
                    if (n < 0) break;
                    if (qo100::is_subscription(buffer.data(), static_cast<size_t>(n))) {
                        const bool changed_peer=!active() || from.sin_addr.s_addr!=peer.sin_addr.s_addr || from.sin_port!=peer.sin_port;
                        if (!subscribed || from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port)
                            std::cout << "GUI subscriber " << inet_ntoa(from.sin_addr) << ':' << ntohs(from.sin_port) << '\n';
                        if (!active() || from.sin_addr.s_addr!=peer.sin_addr.s_addr || from.sin_port!=peer.sin_port) {
                            for(auto& rx:receivers) rx.have_request=false;
                            mute_tx();tx_have_request=false;have_filter_request=false;have_compressor_request=false;have_nr_request=false;
                            have_settings_request=false;
                        }
                        peer = from; subscribed = true; last_subscription = Clock::now();
                        if(changed_peer) {
                            have_loop_request=false;
                            change_loop(TestLoop::normal);
                        }
                    } else if (active() && from.sin_addr.s_addr==peer.sin_addr.s_addr && from.sin_port==peer.sin_port) {
                        qo100::SettingsControl control_settings{};
                        if(qo100::parse_settings_control(buffer.data(),size_t(n),control_settings)) {
                            if((control_settings.set && control_settings.session!=session_id) ||
                               (!control_settings.set && control_settings.session && control_settings.session!=session_id) ||
                               (have_settings_request && control_settings.request!=settings_request &&
                                int32_t(control_settings.request-settings_request)<0)) continue;
                            if(pending_settings && control_settings.request!=settings_request) {
                                send_settings(control_settings.request,control_settings.set?6:4);continue;
                            }
                            if(!have_settings_request || control_settings.request!=settings_request) {
                                settings_request=control_settings.request;have_settings_request=true;
                                if(!pending_settings) {settings_status=0;settings_error.clear();}
                                if(control_settings.set) {
                                    if(pending_settings) settings_status=4;
                                    else if(control_settings.revision!=settings.revision()) settings_status=2;
                                    else {
                                        try {
                                            const auto patch=qo100::parse_settings(control_settings.payload);
                                            auto next=qo100::patch_settings(c,patch);
                                            if(source=="pluto" && hardware_settings_changed(c,next)) {
                                                pending_settings=next;settings_status=4;settings_rollback=false;
                                                loop_mode=loop_target=TestLoop::normal;loop_pending=false;
                                                restart_driver(next);
                                            } else {
                                                const auto old=c;
                                                try {settings.commit(next);} catch(const std::exception& e) {
                                                    settings_status=3;settings_error=e.what();
                                                }
                                                if(!settings_status) apply_runtime(old,false);
                                            }
                                        } catch(const std::exception& e) {
                                            if(pending_settings) fail_settings(e.what());
                                            else {settings_status=1;settings_error=e.what();}
                                        }
                                    }
                                }
                            }
                            send_settings();continue;
                        }
                        // During a hardware transaction only settings/subscription traffic is applied.
                        if(pending_settings) continue;
                        qo100::LoopControl control_loop{};
                        if(qo100::parse_loop_control(buffer.data(),size_t(n),control_loop)) {
                            if(control_loop.session!=session_id || (have_loop_request && control_loop.request!=loop_request &&
                               int32_t(control_loop.request-loop_request)<0)) continue;
                            if(!have_loop_request || control_loop.request!=loop_request) {
                                loop_request=control_loop.request;have_loop_request=true;loop_status=loop_pending?3:0;
                                if(control_loop.set) {
                                    if(control_loop.mode>2) loop_status=1;
                                    else if(control_loop.mode==2 && source!="pluto") loop_status=2;
                                    else change_loop(TestLoop(control_loop.mode));
                                }
                            }
                            send_loop_state();continue;
                        }
                        qo100::CompressorControl compressor_control{};
                        if(qo100::parse_compressor_control(buffer.data(),size_t(n),compressor_control)) {
                            if(compressor_control.session!=session_id || (have_compressor_request &&
                               compressor_control.request!=compressor_request && int32_t(compressor_control.request-compressor_request)<0)) continue;
                            if(!have_compressor_request || compressor_control.request!=compressor_request) {
                                compressor_request=compressor_control.request;have_compressor_request=true;compressor_status=0;
                                if(compressor_control.set) {
                                    const qo100::CompressorSettings requested{compressor_control.amount};
                                    if(!requested.valid()) compressor_status=1;
                                    else if(requested.amount!=tx.compressor_settings().amount) {
                                        tx.set_compressor(requested);
                                        c.tx1_compressor=requested;settings.changed();
                                        std::cout << "TX1 compressor=" << requested.amount/10. << " percent\n";
                                    }
                                }
                            }
                            send_packet(qo100::compressor_state_packet(compressor_request,tx.compressor_settings().amount,
                                tx_epoch,session_id,compressor_status,uint32_t(std::lround(tx.compression_db()*1000)),
                                uint32_t(std::lround(tx.limiting_db()*1000))));
                            continue;
                        }
                        qo100::NrControl nr_control{};
                        if(qo100::parse_nr_control(buffer.data(),size_t(n),nr_control)) {
                            if(nr_control.session!=session_id || (have_nr_request && nr_control.request!=nr_request &&
                               int32_t(nr_control.request-nr_request)<0)) continue;
                            if(!have_nr_request || nr_control.request!=nr_request) {
                                nr_request=nr_control.request;have_nr_request=true;nr_status=0;
                                if(nr_control.set) {
                                    if(nr_control.percent>100) nr_status=1;
                                    else if(nr_control.percent!=receivers[0].dsp->nr_percent()) {
                                        receivers[0].dsp->set_nr(nr_control.percent);
                                        c.rx1_nr_percent=nr_control.percent;
                                        if(nr_control.percent) c.gui_nr_strength=nr_control.percent;
                                        settings.changed();
                                        std::cout << "RX1 NR=" << nr_control.percent << " percent\n";
                                    }
                                }
                            }
                            send_packet(qo100::nr_state_packet(nr_request,receivers[0].dsp->nr_percent(),
                                receivers[0].epoch,session_id,nr_status));
                            continue;
                        }
                        qo100::FilterControl filter_control{};
                        if(qo100::parse_filter_control(buffer.data(),size_t(n),filter_control)) {
                            if(filter_control.session!=session_id || (have_filter_request &&
                               filter_control.request!=filter_request && int32_t(filter_control.request-filter_request)<0)) continue;
                            if(!have_filter_request || filter_control.request!=filter_request) {
                                filter_request=filter_control.request;have_filter_request=true;filter_status=0;
                                if(filter_control.set) {
                                    const qo100::RxFilterSettings requested{filter_control.bandwidth_hz,filter_control.center_hz};
                                    if(!requested.valid()) filter_status=1;
                                    else if(requested!=receivers[0].dsp->filter_settings()) {
                                        receivers[0].dsp->set_filter(requested);
                                        c.rx1_filter=requested;settings.changed();
                                        std::cout << "RX1 filter BW=" << requested.bandwidth_hz << " Hz; center=" << requested.center_hz
                                                  << " Hz; passband=" << requested.low_hz() << ".." << requested.high_hz() << " Hz\n";
                                    }
                                }
                            }
                            const auto actual=receivers[0].dsp->filter_settings();
                            send_packet(qo100::filter_state_packet(filter_request,actual.bandwidth_hz,actual.center_hz,
                                receivers[0].epoch,session_id,filter_status,filter_control.edges));
                            continue;
                        }
                        qo100::TxControl control{};qo100::TxAudio audio{};
                        if(qo100::parse_tx_control(buffer.data(),size_t(n),control)) {
                            if(control.session!=session_id || (tx_have_request && control.request!=tx_request && int32_t(control.request-tx_request)<0)) continue;
                            const bool valid=control.frequency>=c.tx_center_hz-275000 && control.frequency<=c.tx_center_hz+275000;
                            if(tx_have_request && control.request==tx_request &&
                               (control.frequency!=tx_frequency || control.enabled!=tx_enabled)) { send_tx_state();continue; }
                            tx_request=control.request;tx_have_request=true;tx_command_status=valid?0:1;
                            if((loop_pending || !hardware_online) && control.enabled) { tx_command_status=2;send_tx_state();continue; }
                            if(valid) {
                                if(control.frequency!=tx_frequency || control.enabled!=tx_enabled) {
                                    const bool retuned=control.frequency!=tx_frequency;
                                    tx_frequency=control.frequency;tx_enabled=control.enabled;++tx_epoch;echo_seen.clear();echo_next=0;
                                    if(retuned) {c.initial_tx_hz=tx_frequency;settings.changed();}
                                    tx.reset(int64_t(tx_frequency)-int64_t(c.tx_center_hz));
                                    if(retuned && loop_mode==TestLoop::rf && !loop_pending) {retune_rx(0);send_rx_state(0);}
                                    last_tx_audio=Clock::now()-std::chrono::seconds(10);
                                    std::cout << "TX " << tx_frequency << " Hz; software PTT=" << tx_enabled << '\n';
                                }
                                last_tx_control=Clock::now();
                            }
                            send_tx_state();continue;
                        }
                        if(qo100::parse_tx_audio(buffer.data(),size_t(n),audio)) {
                            if(!tx_enabled || loop_pending || audio.session!=session_id || audio.epoch!=tx_epoch || audio.frequency!=tx_frequency) continue;
                            if(loop_mode==TestLoop::audio) {
                                if(audio.index%240 || audio.index>std::numeric_limits<uint64_t>::max()-240 ||
                                   (audio.index<echo_next && echo_next-audio.index>64*240) || !echo_seen.insert(audio.index).second) continue;
                                echo_next=std::max(echo_next,audio.index+240);
                                while(!echo_seen.empty() && echo_next-*echo_seen.begin()>64*240) echo_seen.erase(echo_seen.begin());
                                qo100::FrameInfo f{tx_epoch,stamp(),audio.index,tx_frequency,12000,0,session_id,4};
                                send_packet(qo100::audio_packet(f,audio.samples));
                            } else if(tx.add(audio.index,audio.samples)) last_tx_audio=Clock::now();
                            continue;
                        }
                        uint32_t stream_id=0, request=0,tune_session=0; uint64_t frequency=0;
                        if(qo100::parse_tune(buffer.data(),static_cast<size_t>(n),stream_id,request,frequency,&tune_session)) {
                            if(tune_session && tune_session!=session_id) continue;
                            auto& rx=receivers[stream_id-1];
                            if(rx.have_request && request!=rx.last_request && static_cast<int32_t>(request-rx.last_request)<0) continue;
                            if(rx.have_request && request==rx.last_request) {send_rx_state(stream_id-1,rx.command_status);continue;}
                            const bool valid=frequency>=c.rx_center_hz-275000 && frequency<=c.rx_center_hz+275000;
                            rx.command_status=valid?0:1;
                            if(valid && frequency!=rx.frequency) {
                                rx.frequency=frequency;
                                if(stream_id==1) c.initial_rx_hz=frequency;else c.initial_rx2_hz=frequency;
                                settings.changed();
                                retune_rx(stream_id-1);
                                std::cout << "RX" << stream_id << " tuned to " << rx.frequency << " Hz; USB; epoch=" << rx.epoch << '\n';
                            }
                            rx.last_request=request;rx.have_request=true;
                            send_rx_state(stream_id-1,valid?0:1);
                        }
                    }
                }
            }
            if (iq && (fds[1].revents & POLLIN)) {
                for (int budget = 0; budget < 4; ++budget) {
                    sockaddr_in from{};
                    socklen_t len = sizeof(from);
                    auto n = recvfrom(iq->fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
                    if (n < 0) break;
                    if (ntohl(from.sin_addr.s_addr) != INADDR_LOOPBACK || !n || n%4) ++malformed_iq;
                    else {
                        const bool recovered=hardware_recovering && !pending_settings && !loop_pending;
                        hardware_online=true;driver_error.clear();
                        if(recovered) {
                            hardware_recovering=false;
                            current_rx_lo=rx_lo_target=c.pluto_rx_hz;++loop_generation;
                            spectrum.reset();for(size_t i=0;i<receivers.size();++i) reset_rx(i);
                            send_loop_state();
                        }
                        if(pending_settings) {
                            if(settings_rollback) {
                                pending_settings.reset();settings_rollback=false;
                                current_rx_lo=rx_lo_target=c.pluto_rx_hz;++loop_generation;
                                spectrum.reset();for(size_t i=0;i<receivers.size();++i) reset_rx(i);
                                send_loop_state();send_settings();
                            } else {
                                const auto old=c;
                                try {
                                    settings.commit(*pending_settings);pending_settings.reset();
                                    current_rx_lo=rx_lo_target=c.pluto_rx_hz;++loop_generation;
                                    apply_runtime(old,true);settings_status=0;settings_error.clear();
                                    send_loop_state();send_settings();
                                } catch(const std::exception& e) {fail_settings(e.what());continue;}
                            }
                        }
                        if(loop_pending) commit_loop();
                        feed_iq(buffer.data(),size_t(n));received_iq=true;last_iq=Clock::now();
                    }
                }
            }
            if(tx_enabled && (!active() || Clock::now()-last_tx_control>std::chrono::milliseconds(1500))) {
                mute_tx();send_tx_state();
            }
            if(!active() && !pending_settings) change_loop(TestLoop::normal);
            if(tx_status && (fds[2].revents&POLLIN)) {
                bool update=false;
                for(int budget=0;budget<128;++budget) {
                    sockaddr_in from{};socklen_t len=sizeof(from);
                    const auto n=recvfrom(tx_status->fd,buffer.data(),buffer.size(),0,reinterpret_cast<sockaddr*>(&from),&len);
                    if(n<0) break;
                    if(n!=16 || ntohl(from.sin_addr.s_addr)!=INADDR_LOOPBACK || std::memcmp(buffer.data(),"PTXS",4)) continue;
                    uint32_t h[4];std::memcpy(h,buffer.data(),16);
                    const auto seq=ntohl(h[1]),fill=ntohl(h[2]),capacity=ntohl(h[3]);
                    if(capacity!=8 || fill>capacity || (have_status && int32_t(seq-status_sequence)<=0)) continue;
                    have_status=true;status_sequence=seq;pluto_fill=fill;update=true;last_tx_feedback=Clock::now();
                }
                if(update) while(pluto_fill<4) { render_tx();++pluto_fill; }
            }
            if(source=="synthetic" && Clock::now()>=next_tx) {
                render_tx();next_tx+=std::chrono::milliseconds(20);
            }
            if(tx_have_request && active() && Clock::now()-last_tx_state>std::chrono::milliseconds(250)) send_tx_state();
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
                try {
                    pluto->check();
                    if(Clock::now()-last_tx_feedback>std::chrono::seconds(received_iq?5:8) ||
                       Clock::now()-last_iq>std::chrono::seconds(received_iq?5:8))
                        throw std::runtime_error("No Pluto IQ/FIFO data");
                } catch(const std::exception& e) {
                    if(pending_settings) fail_settings(e.what());
                    else {
                        driver_error=e.what();std::cerr<<"Pluto offline: "<<driver_error<<'\n';
                        pluto.reset();hardware_online=false;mute_tx();
                        loop_mode=loop_target=TestLoop::normal;loop_pending=false;loop_status=0;
                        next_hardware_retry=Clock::now()+std::chrono::seconds(5);
                    }
                }
            }
            if(source=="pluto" && !pluto && !pending_settings && Clock::now()>=next_hardware_retry) {
                try {restart_driver(c);} catch(const std::exception& e) {driver_error=e.what();next_hardware_retry=Clock::now()+std::chrono::seconds(5);}
            }
            settings.flush_due();
            const double stats_interval = std::chrono::duration<double>(Clock::now() - last_stats).count();
            if (stats_interval >= 2) {
                std::cout << "IQ samples=" << spectrum.samples() << "; rate=" << (spectrum.samples() - last_samples) / stats_interval
                          << "/s; spectrum frames sent=" << frames_sent << "; TX blocks=" << tx_blocks << "; TX audio FIFO=" << tx.level() << "; ASRC=" << tx.ppm() << " ppm; TX underruns=" << tx.underruns << "; malformed IQ=" << malformed_iq << "; send errors=" << send_errors << '\n';
                last_samples = spectrum.samples(); last_stats = Clock::now();
            }
        }
        settings.flush();
        std::cout << "Stopping; IQ samples=" << spectrum.samples() << "; spectrum frames sent=" << frames_sent << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
