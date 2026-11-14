#include "bridge.hpp"
#include "ft8_bridge.h"

#include <algorithm>
#include <iostream>
#include <regex>
#include <sstream>

namespace bridge {
namespace {
using namespace std::chrono;
using Utc = system_clock;
const std::regex callsign(R"((?:[A-Z]|[A-Z][A-Z0-9]|[0-9][A-Z])[0-9][A-Z]{1,3})");
const std::regex grid(R"([A-R]{2}[0-9]{2}([A-X]{2})?)");
const std::regex report(R"([+-][0-9]{2})");
const std::regex cq(R"(^CQ +(?:[A-Z]{1,4} +|[0-9]{3} +)?<?([A-Z0-9/]*[0-9][A-Z0-9/]*)>?(?: |$))");
}

void Bridge::update_ft8_subscription() {
    const bool active = decoders_[Config::rx2_ft8] && std::any_of(clients_.begin(), clients_.end(),
        [](const auto& item) { return item.second.ft8_rx; });
    if (active == ft8_active_) return;
    ft8_active_ = active;
    if (++ft_mode_revision_ == 0) ++ft_mode_revision_;
    next_activity_ = {};
    update_digital_activity();
}

void Bridge::set_ft_mode(uint64_t client, const Json& id, const std::string& name) {
    if (!decoders_[Config::rx2_ft8]) throw std::runtime_error("RX2 FT4/8 is disabled");
    if (name != "FT4" && name != "FT8") throw std::runtime_error("Expected FT4 or FT8");
    if (tx2_active(Tx2Mode::ft8)) {
        error(client, id, "tx2_busy", "Finish or cancel the current FT4/8 transmission first.");
        return;
    }
    const auto mode = name == "FT4" ? FtMode::ft4 : FtMode::ft8;
    if (mode != ft_mode_) {
        ft_mode_ = mode;
        if (++ft_mode_revision_ == 0) ++ft_mode_revision_;
        state_["ft_mode"] = name;
        state_["ft8_tx"] = {{"phase", "idle"}};
        state_["ready"] = ready();
        broadcast(state_);
    }
    text(client, {{"type", "ack"}, {"command", "ft_mode"}, {"id", id}, {"status", 0}});
}

void Bridge::publish_ft8(const std::string& text) {
    if (!ft8_active_ || (state_["flags"].get<unsigned>() & 6) || state_["loop"].value("mode", 0)) return;
    auto decode = Json::parse(text, nullptr, false);
    const auto& receivers = state_["rx"];
    if (!decode.is_object() || !receivers.contains("2") ||
        decode.value("type", "") != "decode" || decode.value("session", 0u) != session_ ||
        decode.value("epoch", uint64_t(0)) != receivers["2"]["epoch"] ||
        decode.value("dial_hz", uint64_t(0)) != receivers["2"]["frequency_hz"] ||
        decode.value("mode_revision", 0u) != ft_mode_revision_ ||
        decode.value("mode", "") != ft_mode_name(ft_mode_))
        return;
    if (ft_logbook_) {
        const auto message = decode.value("message", "");
        std::smatch match;
        if (std::regex_search(message, match, cq)) {
            const auto call = match[1].str();
            decode["cq_call"] = call;
            decode["worked_recently"] = ft_logbook_->worked_recently(call, std::time(nullptr));
        }
    }
    decode["type"] = "ft8_decode";
    send_rx2(decode, &Client::ft8_rx);
}

void Bridge::save_ft_qso(uint64_t client, const Json& id, const Json& data) {
    if (config_.ft_log_path.empty()) {
        error(client, id, "ft_log_disabled", "FT4/8 logbook is disabled");
        return;
    }
    if (!ft_logbook_) {
        error(client, id, "ft_log_failed", "Cannot read FT4/8 logbook");
        return;
    }
    const FtQsoRecord qso{
        data.at("call").get<std::string>(), data.at("own_call").get<std::string>(),
        data.value("grid", ""), data.at("own_grid").get<std::string>(), data.at("mode").get<std::string>(),
        data.at("sent_report").get<std::string>(), data.at("received_report").get<std::string>(),
        data.at("start").get<std::time_t>(), data.at("end").get<std::time_t>(),
        data.at("tx_hz").get<uint64_t>(), data.at("rx_hz").get<uint64_t>()
    };
    const auto now = std::time(nullptr);
    if (!std::regex_match(qso.call, callsign) || !std::regex_match(qso.own_call, callsign) ||
        qso.call == qso.own_call || !std::regex_match(qso.own_grid, grid) ||
        (!qso.grid.empty() && !std::regex_match(qso.grid, grid)) ||
        !std::regex_match(qso.sent_report, report) || !std::regex_match(qso.received_report, report) ||
        (qso.mode != "FT4" && qso.mode != "FT8") ||
        qso.start < now - 86400 || qso.end < qso.start || qso.end > now + 2 ||
        qso.tx_hz < 2300000000 || qso.tx_hz > 2450000000 ||
        qso.rx_hz < 10000000000ULL || qso.rx_hz > 10500000000ULL)
        throw std::runtime_error("Invalid FT4/8 QSO record");

    try { ft_logbook_->save(qso); }
    catch (const std::exception& e) {
        std::cerr << "FT4/8 logbook: " << e.what() << '\n';
        error(client, id, "ft_log_failed", "Cannot write FT4/8 logbook");
        return;
    }
    text(client, {{"type", "ack"}, {"command", "ft_qso_log"}, {"id", id}, {"status", 0}});
    send_rx2({{"type", "ft_qso_logged"}, {"call", qso.call}}, &Client::ft8_rx);
}

void Bridge::begin_ft8_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level,
                          const std::string& message, unsigned audio_hz, unsigned parity, const std::string& mode) {
    if (!decoders_[Config::rx2_ft8]) throw std::runtime_error("RX2 FT4/8 is disabled");
    if (mode != ft_mode_name(ft_mode_)) throw std::runtime_error("FT4/8 mode changed; check the selected mode");
    if (!can_start_tx2(client, id, frequency, level)) return;
    const unsigned max_audio_hz = ft_mode_ == FtMode::ft4 ? 3930 : 3950;
    if (audio_hz < 100 || audio_hz > max_audio_hz)
        throw std::runtime_error("Audio frequency must be 100–" + std::to_string(max_audio_hz) + " Hz");
    if (message.size() > 37 || std::any_of(message.begin(), message.end(),
        [](unsigned char c) { return c < 32 || c > 126; }))
        throw std::runtime_error("Message must contain at most 37 printable ASCII characters");

    std::istringstream words(message);
    std::string word, normalized;
    while (words >> word) {
        if (!normalized.empty()) normalized += ' ';
        for (char c : word) normalized += c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c;
    }
    std::vector<int16_t> pcm(ft_wave_samples(ft_mode_));
    char encoded[37]{};
    const auto length = ft_encode_wave(normalized.data(), int(normalized.size()), float(audio_hz),
                                       static_cast<int>(ft_mode_), encoded, pcm.data());
    if (length <= 0 || normalized != std::string(encoded, length)) {
        if (normalized.size() > 13) {
            error(client, id, "ft8_free_text_length",
                  "Free text is limited to 13 characters including spaces. Longer messages need a standard message format.");
        } else {
            error(client, id, "ft8_invalid_message", "Cannot encode this message as entered.");
        }
        return;
    }

    const auto now = Clock::now();
    const auto utc = Utc::now();
    const auto lead = milliseconds(config_.ft8_tx_lead_ms);
    const auto signal_offset = milliseconds(500) - lead;
    const auto slot_ms = ft_slot_ms(ft_mode_);
    const auto earliest_dispatch = utc + Ft8Tx::minimum_preparation;
    auto slot = duration_cast<milliseconds>((earliest_dispatch - signal_offset).time_since_epoch()).count() / slot_ms;
    const auto dispatch = [&](int64_t index) { return Utc::time_point(milliseconds(index * slot_ms)) + signal_offset; };
    if (dispatch(slot) < earliest_dispatch) ++slot;
    if (slot % 2 != parity) ++slot;

    const auto dispatch_utc = dispatch(slot);
    const auto dispatch_at = now + (dispatch_utc - utc);
    const double slot_utc = slot * slot_ms / 1000.0;
    start_tx2(Tx2Mode::ft8, Ft8Tx{std::move(pcm), slot_utc, dispatch_utc, dispatch_at}, frequency, level);
    text(client, {{"type", "ack"}, {"command", "ft8_tx_start"}, {"id", id}, {"status", 0},
                  {"slot_utc", slot_utc}, {"message", normalized}});
}
}
