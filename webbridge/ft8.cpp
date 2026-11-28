#include "bridge.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <random>
#include <unistd.h>

namespace bridge {
namespace {
using namespace std::chrono;
using Command = ft8::Command;
using Type = Command::Type;
using RadioStatus = ft8::RadioReport::Status;
constexpr auto rx2_tune_key = (std::uint64_t(3) << 32) | 2;
constexpr std::size_t max_qrz_key_size = 256;

const char* mode_name(ft8::Mode mode) { return mode == ft8::Mode::FT4 ? "FT4" : "FT8"; }
int max_offset(ft8::Mode mode) { return mode == ft8::Mode::FT4 ? 3930 : 3950; }
int64_t utc_ms(ft8::Time time) { return duration_cast<milliseconds>(time.time_since_epoch()).count(); }

const char* auto_name(ft8::AutoMode mode)
{
    switch (mode) {
        case ft8::AutoMode::Off: return "off";
        case ft8::AutoMode::CallCQ: return "call-cq";
        case ft8::AutoMode::AnswerCQ: return "answer-cq";
    }
    return "off";
}

const char* state_name(ft8::State state)
{
    switch (state) {
        case ft8::State::Receiving: return "receiving";
        case ft8::State::CallingCQ: return "calling-cq";
        case ft8::State::WaitingForReport: return "report";
        case ft8::State::WaitingForRogerReport: return "roger-report";
        case ft8::State::WaitingForAcknowledgment: return "acknowledgment";
        case ft8::State::Closing: return "closing";
        case ft8::State::SendingFreeText: return "free-text";
    }
    return "receiving";
}

const char* outcome_name(ft8::Outcome outcome)
{
    switch (outcome) {
        case ft8::Outcome::Completed: return "completed";
        case ft8::Outcome::Aborted: return "aborted";
        case ft8::Outcome::Timeout: return "timeout";
        case ft8::Outcome::RadioError: return "radio-error";
        case ft8::Outcome::ClockChanged: return "clock-changed";
    }
    return "";
}

std::string station_field(const Json& request, const char* key)
{
    auto value = request.at(key).get<std::string>();
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    for (auto& ch : value) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return value;
}

int ft_number(const Json& request, const char* key, int min, int max)
{
    const auto& value = request.at(key);
    if (!value.is_number_integer()) throw std::invalid_argument(std::string("Expected integer: ") + key);
    const auto number = value.get<int64_t>();
    if (number < min || number > max) throw std::invalid_argument(std::string("Out of range: ") + key);
    return int(number);
}

ft8::Config station_config(ft8::Config config, const Json& request)
{
    config.callsign = station_field(request, "callsign");
    config.locator = station_field(request, "locator");
    if (config.callsign.empty() || config.locator.empty())
        throw std::invalid_argument("Enter a callsign and locator for FT TX");
    config.tx_hz = ft_number(request, "tx_hz", 200, max_offset(config.mode));
    config.validate();
    return config;
}

std::unique_ptr<ft8::Transceiver> make_ft_transceiver(const ft8::Config& config)
{
    auto radio = std::make_unique<ft8::Transceiver>(config);
    radio->set_decode_during_tx(true);
    return radio;
}

std::string download_token()
{
    constexpr char hex[] = "0123456789abcdef";
    std::random_device random;
    std::string token;
    for (unsigned i = 0; i < 6; ++i) {
        const auto word = random();
        for (unsigned shift = 0; shift < 32; shift += 4) token += hex[(word >> shift) & 15];
    }
    return token;
}

void prepare_log_directory(const std::filesystem::path& path)
{
    if (path.has_parent_path()) {
        std::error_code ignored;
        std::filesystem::create_directories(path.parent_path(), ignored);
    }
}

void save_station(const std::filesystem::path& path, const ft8::Config& config, unsigned level)
{
    if (path.empty()) return;
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path;
    temporary += ".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file << Json{{"callsign", config.callsign}, {"locator", config.locator},
                     {"tx_hz", config.tx_hz}, {"tx_level", level}}.dump(2) << '\n';
        file.close();
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::string qrz_key_value(std::string_view key)
{
    const auto first = key.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    key = key.substr(first, key.find_last_not_of(" \t") - first + 1);
    if (key.size() > max_qrz_key_size ||
        std::any_of(key.begin(), key.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }))
        throw std::invalid_argument("Invalid QRZ logbook API key");
    return std::string(key);
}

std::optional<std::string> load_qrz_key(const std::filesystem::path& path)
{
    const auto status = std::filesystem::symlink_status(path);
    if (!std::filesystem::exists(status)) return std::nullopt;
    if (!std::filesystem::is_regular_file(status))
        throw std::runtime_error("Expected a regular QRZ key file");

    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open QRZ key file");
    std::array<char, max_qrz_key_size + 2> buffer{};
    file.read(buffer.data(), buffer.size());
    if (!file.eof() || file.bad()) throw std::runtime_error("Cannot read QRZ key file");
    std::string_view key(buffer.data(), file.gcount());
    if (key.ends_with('\n')) key.remove_suffix(1);
    return qrz_key_value(key);
}

void save_qrz_key(const std::filesystem::path& path, const std::string& key)
{
    std::filesystem::create_directories(path.parent_path());
    auto temporary = path.string() + ".tmp-XXXXXX" + qrz_key_extension;
    const auto data = key + '\n';
    const int fd = ::mkostemps(temporary.data(), sizeof(qrz_key_extension) - 1, O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("Cannot create QRZ key file");

    bool closed = false;
    try {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto written = ::write(fd, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::runtime_error("Cannot write QRZ key file");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(fd) < 0) throw std::runtime_error("Cannot sync QRZ key file");
        closed = true;
        if (::close(fd) < 0) throw std::runtime_error("Cannot close QRZ key file");
        std::filesystem::rename(temporary, path);
    } catch (...) {
        if (!closed) ::close(fd);
        ::unlink(temporary.c_str());
        throw;
    }

    const int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory >= 0) {
        (void)::fsync(directory);
        ::close(directory);
    }
}
}

Bridge::~Bridge()
{
    ft_cleanup_.join();
}

void Bridge::init_ft()
{
    ft_config_.adif_log_path = config_.ft_log_path;
    ft_config_.qrz_api_key = config_.ft_qrz_api_key;
    ft_config_.ptt_lead_ms = int(config_.ft_ptt_lead_ms);
    ft_config_.prop_mode = "SAT";
    ft_config_.sat_name = "QO-100";
    if (config_.ft_enabled && !config_.ft_log_path.empty()) {
        prepare_log_directory(ft_config_.adif_log_path);
        try {
            if (auto saved_key = load_qrz_key(config_.ft_log_path + qrz_key_extension))
                ft_config_.qrz_api_key = std::move(*saved_key);
        } catch (const std::exception&) {
            ft_config_.qrz_api_key.clear();
            std::cerr << "Cannot load saved QRZ logbook API key\n";
        }
    }
    if (config_.ft_enabled && !config_.ft_station_path.empty()) {
        try {
            std::ifstream file(config_.ft_station_path);
            if (file) {
                const auto station = Json::parse(file);
                const auto level = ft_number(station, "tx_level", 1, 100);
                auto config = station_config(ft_config_, station);
                ft_ = make_ft_transceiver(config);
                ft_config_ = std::move(config);
                ft_level_ = level;
            } else if (std::filesystem::exists(config_.ft_station_path)) {
                throw std::runtime_error("Cannot read FT station settings");
            }
        } catch (const std::exception& e) {
            std::cerr << "FT station settings: " << e.what() << '\n';
        }
    }
    if (config_.ft_enabled && !ft_)
        ft_ = make_ft_transceiver(ft_config_);
    refresh_ft_state();
}

void Bridge::retire_ft()
{
    if (!ft_) return;
    ft_disarm_request_ = 0;
    boost::asio::post(ft_cleanup_, [radio = std::move(ft_)]() mutable {
        radio->stop();
        (void)radio->take_radio_actions(); // The bridge has already released TX2.
        radio.reset();
    });
}

void Bridge::replace_ft(std::unique_ptr<ft8::Transceiver> radio)
{
    disarm_ft("settings_changed");
    retire_ft();
    ft_ = std::move(radio);
    ft_state_ = ft8::State::Receiving;
    ft_peer_.clear();
    ft_retries_ = 0;
    reset_ft_receive();
}

bool Bridge::ft_operator() const
{
    const auto it = clients_.find(owner_);
    return it != clients_.end() && it->second.ft8_rx;
}

void Bridge::update_ft8_subscription()
{
    const bool active = config_.ft_enabled && std::any_of(clients_.begin(), clients_.end(),
        [](const auto& item) { return item.second.ft8_rx; });
    if (active == ft8_active_) return;
    ft8_active_ = active;
    reset_ft_receive();
    ft_next_clock_ = {};
    next_activity_ = {};
    update_digital_activity();
}

void Bridge::reset_ft_receive(bool reset_clock)
{
    if (reset_clock) ft_clock_.reset();
    ft_decodes_.clear();
    ++ft_receive_revision_;
    ft_receive_reset_ = ft_ && ft_->reset_receive(ft_receive_revision_) ? ft_receive_revision_ : 0;
}

void Bridge::receive_ft(const Bytes& packet)
{
    if (!ft_ || (state_["flags"].get<unsigned>() & 6) || state_["loop"].value("mode", 0) ||
        pending_.count(rx2_tune_key)) return;
    const auto block = qo100::ft_pcm(packet, 2);
    const auto& receivers = state_["rx"];
    if (!block || !receivers.contains("2") || receivers["2"]["epoch"] != block->epoch ||
        receivers["2"]["frequency_hz"] != block->frequency) return;
    const auto position = ft_clock_.push(*block);
    if (!position) return;
    if (position->discontinuity) {
        disarm_ft("rx_discontinuity");
        reset_ft_receive(false);
    }
    if (!ft_->submit_audio(position->first, block->samples)) {
        // A full input queue or a stale/future timestamp cannot feed a valid RX slot.
        disarm_ft("rx_unavailable");
        reset_ft_receive();
    }
}

bool Bridge::ft_tx_ready(Clock::time_point now) const
{
    const auto downlink = state_["rx"].value("2", Json::object()).value("frequency_hz", uint64_t(0));
    const auto uplink = state_["tx2"].value("frequency_hz", uint64_t(0));
    return ft_ && !ft_config_.callsign.empty() && downlink && uplink &&
           !ft_receive_reset_ && !ft_abort_request_ && !ft_disarm_request_ &&
           !pending_.count(rx2_tune_key) && !pending_.count(tx2_key) &&
           !sstv_upload_ && !tx2_holds_audio() && !digital_tx_busy() && tx2_ready(uplink, now);
}

void Bridge::maybe_arm_ft(Clock::time_point now)
{
    if (ft_authorized_ || !ft_operator() || !ft_tx_ready(now)) return;

    const auto uplink = state_["tx2"]["frequency_hz"].get<uint64_t>();
    const auto downlink = state_["rx"]["2"]["frequency_hz"].get<uint64_t>();
    Command dial(Type::SetDialFrequency);
    dial.dial_frequency_hz = uplink;
    dial.rx_dial_frequency_hz = downlink;
    if (!ft_->command(dial) || !ft_->command(Command(Type::ArmTx))) return;

    ft_uplink_ = uplink;
    ft_downlink_ = downlink;
    ft_authorized_ = true;
    next_activity_ = {};
    update_digital_activity();
}

void Bridge::disarm_ft(const char* reason, bool flush)
{
    if (!ft_authorized_ && !ft_armed_ && !ft_abort_request_ && !tx2_active(Tx2Mode::ft8) &&
        !(tx2_release_ && tx2_release_->ft_completed_id)) return;
    ft_authorized_ = ft_armed_ = false;
    ft_abort_request_ = 0;
    ft_timing_.reset();
    ft_auto_ = ft8::AutoMode::Off;
    if (ft_) {
        Command command(Type::DisarmTx);
        command.request_id = ++ft_next_request_;
        // Wait for the scheduler to discard the old TX actions before arming again.
        if (ft_->command(command)) ft_disarm_request_ = command.request_id;
    }
    if (tx2_active(Tx2Mode::ft8)) end_tx2(reason, flush);
    if (tx2_release_) tx2_release_->ft_completed_id = 0;
    next_activity_ = {};
    update_digital_activity();
}

void Bridge::command_ft(uint64_t client, const Json& id, const std::string& type, const Json& request)
{
    if (!config_.ft_enabled) throw std::runtime_error("RX2 FT4/8 is disabled");
    const auto success = [&] { text(client, {{"type", "ack"}, {"command", type}, {"id", id}, {"status", 0}}); };
    const auto queue = [&](Command command) {
        if (!ft_->command(std::move(command))) throw std::runtime_error("FT command queue is unavailable");
    };
    if (type == "ft_adif") {
        if (config_.ft_log_path.empty()) throw std::runtime_error("FT logbook is disabled");
        ft_download_ = FtDownload{download_token(), client, Clock::now() + seconds(30)};
        text(client, {{"type", "ack"}, {"command", type}, {"id", id}, {"status", 0},
                      {"url", "/ft4-ft8.adi?token=" + ft_download_->token}});
        return;
    } else if (type == "ft_qrz_key") {
        if (config_.ft_log_path.empty()) throw std::runtime_error("FT logbook is disabled");
        const auto key = request.find("key");
        if (key == request.end() || !key->is_string())
            throw std::invalid_argument("Expected a QRZ logbook API key");
        auto config = ft_config_;
        config.qrz_api_key = qrz_key_value(key->get_ref<const std::string&>());
        auto new_radio = config.qrz_api_key != ft_config_.qrz_api_key ? make_ft_transceiver(config) : nullptr;
        save_qrz_key(config_.ft_log_path + qrz_key_extension, config.qrz_api_key);
        if (new_radio) replace_ft(std::move(new_radio));
        ft_config_.qrz_api_key = std::move(config.qrz_api_key);
    } else if (type == "ft_config") {
        const auto level = ft_number(request, "tx_level", 1, 100);
        auto config = station_config(ft_config_, request);
        if (ft_ && config.callsign == ft_config_.callsign && config.locator == ft_config_.locator) {
            save_station(config_.ft_station_path, config, level);
            if (config.tx_hz != ft_config_.tx_hz) {
                disarm_ft("settings_changed");
                Command command(Type::SetTxFrequency);
                command.tx_hz = config.tx_hz;
                queue(command);
            }
        } else {
            auto new_radio = make_ft_transceiver(config);
            save_station(config_.ft_station_path, config, level);
            replace_ft(std::move(new_radio));
        }
        ft_config_ = std::move(config);
        ft_level_ = level;
        if (tx2_active(Tx2Mode::ft8)) tx2_job_->level = level;
    } else if (type == "ft_mode") {
        const auto name = request.at("mode").get<std::string>();
        if (name != "FT4" && name != "FT8") throw std::invalid_argument("Expected FT4 or FT8");
        const auto mode = name == "FT4" ? ft8::Mode::FT4 : ft8::Mode::FT8;
        if (mode != ft_config_.mode) {
            disarm_ft("mode_changed");
            if (ft_) {
                Command command(Type::SetMode);
                command.mode = mode;
                queue(command);
                if (ft_config_.tx_hz > max_offset(mode)) {
                    Command offset(Type::SetTxFrequency);
                    offset.tx_hz = max_offset(mode);
                    queue(offset);
                }
            }
            ft_config_.mode = mode;
            ft_config_.tx_hz = std::min(ft_config_.tx_hz, max_offset(mode));
            reset_ft_receive();
        }
    } else if (type == "ft_arm" && !request.at("armed").get<bool>()) {
        disarm_ft("disarmed");
    } else if (type == "ft_abort") {
        if (ft_armed_ && !ft_abort_request_) {
            Command command(Type::Abort);
            command.request_id = ++ft_next_request_;
            queue(command);
            ft_abort_request_ = command.request_id;
        }
        if (tx2_active(Tx2Mode::ft8)) end_tx2("stopped");
        if (tx2_release_) tx2_release_->ft_completed_id = 0;
    } else if (type == "ft_auto" && request.at("auto") == "off") {
        if (ft_) queue(Command(Type::SetAuto));
    } else {
        if (!ft_operator() || !ft_ || ft_config_.callsign.empty())
            throw std::runtime_error("Open FT4/8 and save a callsign and locator to enable TX");
        if (!ft_armed_ || !ft_tx_ready(Clock::now())) {
            const bool busy = sstv_upload_ || tx2_holds_audio() || digital_tx_busy() || pending_.count(tx2_key);
            error(client, id, busy ? "tx2_busy" : "not_ready", busy ? "TX2 is busy" : "TX2 is not ready");
            return;
        }
        if (type == "ft_auto") {
            const auto automatic = request.at("auto").get<std::string>();
            Command command(Type::SetAuto);
            if (automatic == "call-cq") command.automatic = ft8::AutoMode::CallCQ;
            else if (automatic == "answer-cq") command.automatic = ft8::AutoMode::AnswerCQ;
            else throw std::invalid_argument("Unknown FT auto mode");
            queue(command);
        } else if (type == "ft_free_text") {
            if (!ft_->send_free_text(request.at("text").get<std::string>()))
                throw std::runtime_error("FT command queue is unavailable");
        } else if (type == "ft_cq") {
            Command command(Type::CallCQ);
            if (request.contains("parity")) command.tx_parity = ft_number(request, "parity", 0, 1);
            queue(std::move(command));
        } else if (type == "ft_start_qso") {
            const auto selected = request.at("decode_id").get<std::string>();
            const auto row = std::find_if(ft_decodes_.begin(), ft_decodes_.end(), [&](const auto& entry) {
                return selected == std::to_string(entry.first);
            });
            if (row == ft_decodes_.end() || row->second.message.source.empty() ||
                row->second.message.source == ft_config_.callsign)
                throw std::invalid_argument("The selected FT decode is no longer available");
            Command command(Type::StartQso);
            command.peer = row->second.message.source;
            command.selected = row->second;
            queue(std::move(command));
        } else if (type != "ft_arm") {
            throw std::invalid_argument("Unknown FT command: " + type);
        }
    }
    refresh_ft_state();
    success();
}

bool Bridge::take_ft_download(std::string_view token)
{
    if (!ft_download_ || ft_download_->owner != owner_ || Clock::now() >= ft_download_->expires ||
        token != ft_download_->token) return false;
    ft_download_.reset();
    return true;
}

void Bridge::process_ft_event(const ft8::Event& event)
{
    using Event = ft8::Event::Type;
    if (event.type == Event::ReceiveReset) {
        if (event.request_id == ft_receive_reset_) ft_receive_reset_ = 0;
        return;
    }
    if (event.type == Event::CommandApplied) {
        if (event.request_id == ft_abort_request_) ft_abort_request_ = 0;
        if (event.request_id == ft_disarm_request_) ft_disarm_request_ = 0;
        return;
    }
    if (event.type == Event::StateChanged) {
        const bool lost_arm = ft_armed_ && !event.armed;
        if (lost_arm) disarm_ft("unavailable");
        ft_armed_ = ft_authorized_ && event.armed;
        ft_auto_ = ft_authorized_ ? event.automatic : ft8::AutoMode::Off;
        ft_state_ = event.state;
        ft_peer_ = event.peer;
        ft_retries_ = event.retries;
        return;
    }
    if (event.type == Event::Rx || event.type == Event::Tx) {
        if (!ft8_active_ || ft_receive_reset_ || event.mode != ft_config_.mode ||
            (state_["flags"].get<unsigned>() & 6) || state_["loop"].value("mode", 0)) return;
        const auto& message = event.rx ? event.rx->message : *event.tx;
        Json row{{"type", event.rx ? "ft_decode" : "ft_tx"}, {"mode", mode_name(event.mode)},
                 {"session", session_}, {"receive_revision", ft_receive_revision_}, {"slot_utc_ms", utc_ms(event.utc)},
                 {"audio_hz", event.frequency_hz}, {"text", message.text}, {"source", message.source},
                 {"destination", message.destination}, {"value", message.value}, {"cq", message.kind == ft8::Kind::CQ}};
        if (event.rx) {
            const auto decode_id = ++ft_next_decode_id_;
            ft_decodes_.emplace_back(decode_id, *event.rx);
            if (ft_decodes_.size() > 512) ft_decodes_.pop_front();
            row["decode_id"] = std::to_string(decode_id);
            row["snr_db"] = event.rx->snr_db;
            row["dt_s"] = event.rx->dt_seconds;
            if (event.rx->worked_recently) row["worked_recently"] = *event.rx->worked_recently;
        } else if (event.audio_start) {
            row["audio_start_ms"] = utc_ms(*event.audio_start);
        }
        send_rx2(row, &Client::ft8_rx);
        return;
    }

    const char* kind = "error", *result = "error";
    switch (event.type) {
        case Event::Finished: kind = "qso"; break;
        case Event::FreeTextFinished: kind = "free-text"; break;
        case Event::SlotMissed: kind = "slot"; break;
        case Event::Radio: std::cerr << "FT radio: " << event.detail << '\n'; break;
        case Event::LogSaved: kind = "adif"; result = "completed"; break;
        case Event::LogError: kind = "adif"; break;
        case Event::QrzUploaded: kind = "qrz"; result = "completed"; break;
        case Event::QrzError: kind = "qrz"; break;
        case Event::CommandRejected:
            if (event.request_id && event.request_id == ft_abort_request_) ft_abort_request_ = 0;
            if (event.request_id && event.request_id == ft_disarm_request_) ft_disarm_request_ = 0;
            break;
        default: break;
    }
    if (event.outcome) result = outcome_name(*event.outcome);
    send_rx2({{"type", "ft_notice"}, {"kind", kind}, {"outcome", result},
              {"peer", event.peer}, {"text", event.detail}}, &Client::ft8_rx);
}

void Bridge::process_ft_action(const ft8::RadioAction& action)
{
    const auto& playback = action.tx;
    if (action.type == ft8::RadioAction::Type::Cancel) {
        if (tx2_active(Tx2Mode::ft8) && std::get<Ft8Tx>(tx2_job_->source).playback.id == playback.id)
            end_tx2("stopped");
        if (tx2_release_ && tx2_release_->ft_completed_id == playback.id)
            tx2_release_->ft_completed_id = 0;
        return;
    }
    if (!ft_authorized_ || ft_abort_request_) return;
    const auto now = Clock::now();
    const auto utc = ft8::Time::clock::now();
    const auto latency = milliseconds(config_.ft8_tx_lead_ms);
    const auto center = std::stoull(state_["settings"].value("tx_center_hz", std::string("0")));
    if (!ft_operator() || !ft_tx_ready(now) || !playback.frame || playback.frame->samples.empty() ||
        playback.mode != ft_config_.mode || !std::isfinite(playback.frequency_hz) ||
        playback.frequency_hz < 200 || playback.frequency_hz > max_offset(playback.mode) ||
        double(ft_uplink_) + playback.frequency_hz + 70 > double(center) + 275000 ||
        utc + latency > playback.latest_audio_start) {
        report_ft_radio(playback.id, RadioStatus::Failed, {}, "TX2 is busy, unavailable or outside the FT transmit window");
        return;
    }
    start_tx2(Tx2Mode::ft8, Ft8Tx{playback, now + (playback.audio_start - utc) - latency,
                                 now + (playback.ptt_deadline - utc)}, ft_uplink_, ft_level_);
}

void Bridge::report_ft_radio(uint64_t id, RadioStatus status, ft8::Time start, std::string detail)
{
    if (ft_ && !ft_->report_radio({id, status, start, std::move(detail)}))
        std::cerr << "FT radio report queue unavailable\n";
}

void Bridge::refresh_ft_state()
{
    const auto now = Clock::now();
    if (ft8_active_ && now >= ft_next_clock_) {
        ft_utc_ms_ = utc_ms(ft8::Time::clock::now());
        ft_next_clock_ = now + seconds(1);
    }
    Json snapshot{{"available", config_.ft_enabled}, {"configured", !ft_config_.callsign.empty()},
                  {"mode", mode_name(ft_config_.mode)}, {"callsign", ft_config_.callsign},
                  {"locator", ft_config_.locator}, {"tx_hz", ft_config_.tx_hz}, {"tx_level", ft_level_},
                  {"armed", ft_armed_}, {"auto", auto_name(ft_auto_)}, {"state", state_name(ft_state_)},
                  {"peer", ft_peer_}, {"retries", ft_retries_}, {"log_enabled", !config_.ft_log_path.empty()},
                  {"qrz_configured", !ft_config_.qrz_api_key.empty()},
                  {"busy", ft_state_ != ft8::State::Receiving || ft_abort_request_ || tx2_active(Tx2Mode::ft8) ||
                           (tx2_release_ && tx2_release_->ft_completed_id)},
                  {"tx_ready", ft_tx_ready(now)}, {"receive_revision", ft_receive_revision_},
                  {"slot_ms", ft8::slot_duration(ft_config_.mode).count()}, {"utc_ms", ft_utc_ms_}};
    if (ft_authorized_ && ft_timing_) {
        snapshot["next_tx_ms"] = utc_ms(ft_timing_->audio_start);
        snapshot["next_tx_planned"] = ft_timing_->planned;
    }
    if (state_["ft"] != snapshot) {
        state_["ft"] = std::move(snapshot);
        dirty_ = true;
    }
}

void Bridge::poll_ft()
{
    if (stopping_) return;
    const auto now = Clock::now();
    if (ft_authorized_ && (!ft_operator() || !tx2_ready(ft_uplink_, now) ||
        state_["rx"].value("2", Json::object()).value("frequency_hz", uint64_t(0)) != ft_downlink_))
        disarm_ft("unavailable");
    if (tx2_release_ && tx2_release_->ft_completed_id && now >= tx2_release_->ft_deadline) {
        report_ft_radio(std::exchange(tx2_release_->ft_completed_id, 0), RadioStatus::Failed, {}, "TX2 release timed out");
    }
    if (ft_) {
        for (const auto& action : ft_->take_radio_actions()) process_ft_action(action);
        auto [events, timing] = ft_->take_events_with_timing();
        ft_timing_ = timing;
        for (const auto& event : events) process_ft_event(event);
    }
    maybe_arm_ft(now);
    refresh_ft_state();
    ft_timer_.expires_after(ft8_active_ || ft_authorized_ ? milliseconds(5) : milliseconds(100));
    ft_timer_.async_wait([this](auto error) { if (!error) poll_ft(); });
}

} // namespace bridge
