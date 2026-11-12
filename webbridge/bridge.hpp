#pragma once
#include "wire.hpp"
#include "mm_monitor.hpp"
#include "qo100/channels.hpp"
#include "sstv/receiver.hpp"
#include "sstv/sender.hpp"
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>

namespace bridge {
namespace net=boost::asio;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct Config {
    std::string listen="0.0.0.0", web_root=QO100_WEB_ROOT;
    uint16_t port=8080,backend_port=40900;
    struct Decoder { uint16_t audio_port=0,text_port=0; }; // Both zero disables this link.
    std::array<Decoder,2> decoders;
    uint16_t digital_audio_port=0,digital_bridge_port=0;
    uint16_t rx5_decoder_audio_port=0,mm_http_port=0;
    unsigned idle_seconds=0,heartbeat_seconds=60,max_clients=16;
    size_t queue_bytes=524288;
};
struct Peer {
    virtual ~Peer()=default;
    virtual void send(std::shared_ptr<const Bytes> data,bool binary)=0;
    virtual void close(const std::string& reason)=0;
};
class Bridge {
    struct Client {std::shared_ptr<Peer> peer; Clock::time_point activity;bool mm_diagnostics=false;std::optional<bool> rx2_audio{};bool sstv_rx=false;};
    struct Pending {
        Bytes packet;uint8_t reply;uint64_t client=0;Json id;std::string command;
        Clock::time_point sent{},started=Clock::now();bool safety=false;
    };
    Config config_;
    net::ip::udp::socket udp_,digital_,mm_audio_;
    std::shared_ptr<MmMonitor> mm_monitor_;
    std::map<std::string,std::string> mm_text_;
    qo100::sstv::Receiver sstv_receiver_;
    bool sstv_active_ = false;
    std::optional<qo100::sstv::Mode> sstv_mode_;
    std::optional<qo100::sstv::Frame> sstv_frame_;
    std::uint32_t sstv_image_id_ = 0;
    Clock::time_point last_sstv_audio_{};
    std::array<uint8_t,1201> digital_buffer_{};
    struct DecoderLink {
        net::ip::udp::socket socket;
        std::array<char,4097> buffer{};
        explicit DecoderLink(net::io_context& io):socket(io) {}
    };
    std::array<std::unique_ptr<DecoderLink>,2> decoders_;
    struct SstvTx {
        enum class Phase { upload, preparing, sending, draining };
        Phase phase = Phase::upload;
        uint32_t upload_id = 0, epoch = 0;
        uint64_t frequency = 0, sample_index = 0;
        unsigned level = 0;
        qo100::sstv::Mode mode = qo100::sstv::Mode::martin_m1;
        std::unique_ptr<qo100::sstv::Image> image;
        std::unique_ptr<qo100::sstv::Sender> sender;
        size_t uploaded = 0;
        bool epoch_requested = false;
        Clock::time_point deadline{}, next_audio{};
    };
    std::optional<SstvTx> sstv_tx_;
    bool sstv_tx_releasing_ = false;
    Clock::time_point last_tx2_state_{};
    net::steady_timer timer_, sstv_tx_timer_;
    std::array<uint8_t,65536> receive_buffer_{};
    std::map<uint64_t,Client> clients_;
    std::map<uint64_t,uint32_t> counters_;
    std::map<uint64_t,Pending> pending_;
    std::map<uint64_t,Bytes> last_controls_;
    uint64_t next_client_=0,owner_=0;
    uint32_t session_=0;
    uint64_t digital_nonce_=0;
    unsigned digital_usage_=0,last_activity_=UINT32_MAX;uint32_t activity_sequence_=0;
    bool digital_usage_known_=false;
    Clock::time_point digital_seen_{},digital_tx_audio_at_{},next_activity_{};
    Json state_;
    bool initialized_=false,barrier_=true,off_ack_=false,normal_ack_=false,stopping_=false,dirty_=true;
    bool ptt_requested_=false,hardware_change_=false;
    Clock::time_point last_rx_=Clock::now(),next_sub_{},next_settings_{},next_tx_{},next_rx_{};
    Clock::time_point ptt_until_{};
    void receive();void packet(Bytes p);void tick();void reset_backend();void bootstrap();
    void send_udp(Bytes p);
    void receive_decoder(size_t index);void send_decoder(size_t index,Bytes p);
    void receive_digital();void send_digital(Bytes p);
    void update_mm_subscription();void update_digital_activity();
    void subscribe_sstv(uint64_t client, bool enabled);
    void update_sstv_subscription();
    void reset_sstv();
    void receive_sstv(const Bytes& packet);
    void publish_sstv(Clock::time_point now);
    void send_sstv(std::shared_ptr<const Bytes> bytes, bool binary);
    void set_sstv_mode(uint64_t client, const Json& request, const Json& id);
    Json sstv_begin() const;
    bool digital_tx_busy() const;
    bool sstv_holds_tx2() const;
    void begin_sstv_tx(uint64_t client, uint32_t upload_id, uint64_t frequency, unsigned level);
    void sstv_tx_chunk(const Bytes& bytes);
    void schedule_sstv_tx(Clock::time_point when);
    void step_sstv_tx();
    void end_sstv_tx(const char* result, bool flush = true);
    void reset_tx2_audio(uint64_t frequency);
    void send_sstv_tx_audio(SstvTx& job, const qo100::sstv::Sender::Pcm& pcm);
    void sstv_tx_state(const char* phase, const char* result = "");

    void control(Bytes p,uint8_t reply,uint64_t client=0,Json id=nullptr,std::string command="",bool safety=false);
    void discover();void safety();void tx_off();void roles();void broadcast(const Json& j);
    void text(uint64_t client,const Json& j);void error(uint64_t client,const Json& id,const std::string& code,const std::string& message);
    void update_state(const Bytes& p);void revoke(const std::string& reason);bool ready() const;
public:
    Bridge(net::io_context& io,Config config);
    const Config& config() const {return config_;}
    void start();void stop();
    uint64_t join(std::shared_ptr<Peer> peer);
    void leave(uint64_t id);
    void message(uint64_t id,const Bytes& bytes,bool binary);
};
void listen(net::io_context& io,Bridge& bridge);
}
