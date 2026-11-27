#pragma once
#include "wire.hpp"
#include "ft_audio.hpp"
#include "mm_monitor.hpp"
#include "qo100/channels.hpp"
#include "rtty_rx.hpp"
#include "rtty/rtty.hpp"
#include "sstv/receiver.hpp"
#include "sstv/sender.hpp"
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <variant>

namespace bridge {
namespace net=boost::asio;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
struct Config {
    std::string listen="0.0.0.0", web_root=QO100_WEB_ROOT;
    uint16_t port=8080,backend_port=40900;
    struct Decoder { uint16_t audio_port=0,text_port=0; }; // Both zero disables this link.
    std::array<Decoder,2> decoders;
    bool ft_enabled = false;
    unsigned ft8_tx_lead_ms = 80, ft_ptt_lead_ms = 450;
    int ft_rx_offset_ms = 0;
    std::string ft_log_path, ft_station_path, ft_qrz_api_key;
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
    struct Client {std::shared_ptr<Peer> peer; Clock::time_point activity;bool mm_diagnostics=false;std::optional<bool> rx2_audio{};bool sstv_rx=false, rtty_rx=false, ft8_rx=false;};
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
    RttyReceiver rtty_receiver_;
    bool rtty_active_ = false, rtty_reverse_ = false;
    Clock::time_point last_rtty_audio_{};
    std::array<uint8_t,1201> digital_buffer_{};
    struct DecoderLink {
        net::ip::udp::socket socket;
        std::array<char,4097> buffer{};
        explicit DecoderLink(net::io_context& io):socket(io) {}
    };
    std::array<std::unique_ptr<DecoderLink>,2> decoders_;
    ft8::Config ft_config_;
    unsigned ft_level_ = 25;
    std::unique_ptr<ft8::Transceiver> ft_;
    net::thread_pool ft_cleanup_{1};
    qo100::FtSampleClock ft_clock_;
    bool ft8_active_ = false, ft_authorized_ = false, ft_armed_ = false;
    ft8::State ft_state_ = ft8::State::Receiving;
    ft8::AutoMode ft_auto_ = ft8::AutoMode::Off;
    std::string ft_peer_;
    unsigned ft_retries_ = 0;
    uint64_t ft_uplink_ = 0, ft_downlink_ = 0;
    uint64_t ft_receive_revision_ = 1, ft_receive_reset_ = 0;
    uint64_t ft_next_decode_id_ = 0, ft_next_request_ = 0, ft_abort_request_ = 0;
    std::deque<std::pair<uint64_t, ft8::Decode>> ft_decodes_;
    std::optional<ft8::TxTiming> ft_timing_;
    Clock::time_point ft_next_clock_{};
    int64_t ft_utc_ms_ = 0;
    struct FtDownload {
        std::string token;
        uint64_t owner;
        Clock::time_point expires;
    };
    std::optional<FtDownload> ft_download_;
    enum class Tx2Mode { sstv, rtty, ft8, waterfall };
    static constexpr auto tx2_key = (uint64_t(6) << 32) | qo100::tx2_stream;
    struct SstvUpload {
        uint32_t id;
        uint64_t frequency;
        unsigned level;
        qo100::sstv::Mode mode;
        std::unique_ptr<qo100::sstv::Image> image;
        size_t uploaded = 0;
        Clock::time_point deadline;
    };
    struct Ft8Tx {
        ft8::Playback playback;
        Clock::time_point dispatch_at, ptt_deadline;
        std::size_t skip = 0;
    };
    struct WaterfallTx {
        std::vector<int16_t> pcm;
        uint32_t id;
    };
    using Tx2Source = std::variant<std::unique_ptr<qo100::sstv::Sender>,
                                   std::unique_ptr<qo100::rtty::Encoder>, Ft8Tx, WaterfallTx>;
    struct RttyTx {
        uint32_t id;
        std::optional<Clock::time_point> ptt_until{};
    };
    struct Tx2Job {
        enum class Phase { preparing, sending, draining };
        Tx2Mode mode;
        Tx2Source source;
        uint64_t frequency;
        unsigned level;
        Clock::time_point deadline;
        std::optional<RttyTx> rtty{};
        Phase phase = Phase::preparing;
        uint32_t epoch = 0;
        uint64_t sample_index = 0;
        bool epoch_requested = false;
        Clock::time_point next_audio{};
        unsigned tail = 0;
        std::optional<double> output_gain{};
    };
    std::optional<SstvUpload> sstv_upload_;
    std::optional<Tx2Job> tx2_job_;
    struct Tx2Release {
        uint32_t epoch;
        Tx2Mode mode;
        uint64_t ft_completed_id = 0;
        Clock::time_point ft_deadline{};
    };
    std::optional<Tx2Release> tx2_release_;
    uint64_t tx2_schedule_ = 0;
    Clock::time_point last_tx2_state_{};
    net::steady_timer timer_, tx2_timer_, ft_timer_;
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
    void receive_decoder(size_t index);
    void init_ft();
    void update_ft8_subscription();
    void reset_ft_receive(bool reset_clock = true);
    void receive_ft(const Bytes& packet);
    void command_ft(uint64_t client, const Json& id, const std::string& type, const Json& request);
    bool ft_operator() const;
    bool ft_tx_ready(Clock::time_point now) const;
    void disarm_ft(const char* reason, bool flush = true);
    void retire_ft();
    void process_ft_event(const ft8::Event& event);
    void process_ft_action(const ft8::RadioAction& action);
    void report_ft_radio(uint64_t id, ft8::RadioReport::Status status, ft8::Time start = {}, std::string detail = {});
    void refresh_ft_state();
    void poll_ft();
    void send_decoder(size_t index,Bytes p);
    void receive_digital();void send_digital(Bytes p);
    void update_mm_subscription();void update_digital_activity();
    void subscribe_sstv(uint64_t client, bool enabled);
    void update_sstv_subscription();
    void reset_sstv();
    void receive_rx2(const Bytes& packet);
    void publish_sstv(Clock::time_point now);
    void send_rx2(std::shared_ptr<const Bytes> bytes, bool binary, bool Client::*subscribed);
    void send_rx2(const Json& message, bool Client::*subscribed);
    void send_rx2_spectrum(uint8_t type, const qo100::audio::Spectrum& spectrum, bool Client::*subscribed);
    void set_sstv_mode(uint64_t client, const Json& request, const Json& id);
    Json sstv_begin() const;
    void subscribe_rtty(uint64_t client, bool enabled);
    void update_rtty_subscription();
    void reset_rtty();
    void set_rtty_reverse(uint64_t client, const Json& id, bool enabled);
    void publish_rtty(Clock::time_point now);

    bool digital_tx_busy() const;
    bool tx2_holds_audio() const;
    bool tx2_active(Tx2Mode mode) const;
    bool can_start_tx2(uint64_t client, uint32_t id, uint64_t frequency, unsigned level);
    bool tx2_ready(uint64_t frequency, Clock::time_point now) const;
    void begin_waterfall_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level,
                            const std::string& message, unsigned row_ms);
    void begin_sstv_tx(uint64_t client, uint32_t upload_id, uint64_t frequency, unsigned level);
    void sstv_tx_chunk(const Bytes& bytes);
    void begin_rtty_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level, const std::string& message);
    void open_rtty_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level);
    void write_rtty_tx(uint64_t client, const Json& id, uint32_t tx_id, const std::string& message, bool finish);
    bool rtty_tx_matches(uint32_t id) const;
    void keep_rtty_tx(uint32_t id);
    void start_tx2(Tx2Mode mode, Tx2Source source, uint64_t frequency, unsigned level,
                   std::optional<RttyTx> rtty = {});
    void schedule_tx2(Clock::time_point when);
    void step_tx2();
    void end_tx2(const char* result, bool flush = true);
    void reset_tx2_audio(uint64_t frequency, ft8::Time ft_deadline = {});
    std::optional<qo100::audio::TxBlock> take_tx2_block(Tx2Job& job);
    void send_tx2_audio(Tx2Job& job, const qo100::audio::Pcm& pcm);
    void tx2_state(Tx2Mode mode, const char* phase, const char* result = "", std::optional<uint32_t> id = {});

    void control(Bytes p,uint8_t reply,uint64_t client=0,Json id=nullptr,std::string command="",bool safety=false);
    void discover();void safety();void tx_off();void roles();void broadcast(const Json& j);
    void text(uint64_t client,const Json& j);void error(uint64_t client,const Json& id,const std::string& code,const std::string& message);
    void update_state(const Bytes& p);void revoke(const std::string& reason);bool ready() const;
public:
    Bridge(net::io_context& io,Config config);
    ~Bridge();
    const Config& config() const {return config_;}
    bool take_ft_download(std::string_view token);
    void start();void stop();
    uint64_t join(std::shared_ptr<Peer> peer);
    void leave(uint64_t id);
    void message(uint64_t id,const Bytes& bytes,bool binary);
};
void listen(net::io_context& io,Bridge& bridge);
}
