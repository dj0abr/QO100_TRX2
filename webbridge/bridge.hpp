#pragma once
#include "wire.hpp"
#include "qo100/channels.hpp"
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <map>
#include <memory>

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
    unsigned idle_seconds=0,heartbeat_seconds=60,max_clients=16;
    size_t queue_bytes=524288;
};
struct Peer {
    virtual ~Peer()=default;
    virtual void send(std::shared_ptr<const Bytes> data,bool binary)=0;
    virtual void close(const std::string& reason)=0;
};
class Bridge {
    struct Client {std::shared_ptr<Peer> peer; Clock::time_point activity;};
    struct Pending {
        Bytes packet;uint8_t reply;uint64_t client=0;Json id;std::string command;
        Clock::time_point sent{},started=Clock::now();bool safety=false;
    };
    Config config_;
    net::ip::udp::socket udp_,digital_;
    std::array<uint8_t,1201> digital_buffer_{};
    struct DecoderLink {
        net::ip::udp::socket socket;
        std::array<char,4097> buffer{};
        explicit DecoderLink(net::io_context& io):socket(io) {}
    };
    std::array<std::unique_ptr<DecoderLink>,2> decoders_;
    net::steady_timer timer_;
    std::array<uint8_t,65536> receive_buffer_{};
    std::map<uint64_t,Client> clients_;
    std::map<uint64_t,uint32_t> counters_;
    std::map<uint64_t,Pending> pending_;
    std::map<uint64_t,Bytes> last_controls_;
    uint64_t next_client_=0,owner_=0;
    uint32_t session_=0;
    uint64_t digital_nonce_=0;
    Json state_;
    bool initialized_=false,barrier_=true,off_ack_=false,normal_ack_=false,stopping_=false,dirty_=true;
    bool ptt_requested_=false,hardware_change_=false;
    Clock::time_point last_rx_=Clock::now(),next_sub_{},next_settings_{},next_tx_{},next_rx_{};
    Clock::time_point ptt_until_{};
    void receive();void packet(Bytes p);void tick();void reset_backend();void bootstrap();
    void send_udp(Bytes p);
    void receive_decoder(size_t index);void send_decoder(size_t index,Bytes p);
    void receive_digital();void send_digital(Bytes p);
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
