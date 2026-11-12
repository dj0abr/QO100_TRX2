#include "bridge.hpp"

#include <algorithm>
#include <cmath>

namespace bridge {
namespace sstv = qo100::sstv;
namespace {

const char* mode_name(sstv::Mode mode) {
    return mode == sstv::Mode::martin_m1 ? "M1" : "S1";
}

}

Json Bridge::sstv_begin() const {
    return {{"type", "sstv_begin"}, {"session", session_}, {"image", sstv_image_id_},
        {"mode", mode_name(sstv_frame_->mode)}, {"partial", sstv_frame_->partial}};
}

void Bridge::subscribe_sstv(uint64_t client, bool enabled) {
    auto& subscribed = clients_.at(client).sstv_rx;
    if (subscribed == enabled) return;
    subscribed = enabled;
    update_sstv_subscription();
    if (enabled && sstv_frame_) text(client, sstv_begin());
}

void Bridge::update_sstv_subscription() {
    const bool active = std::any_of(clients_.begin(), clients_.end(),
        [](const auto& item) { return item.second.sstv_rx; });
    if (active == sstv_active_) return;
    sstv_active_ = active;
    next_activity_ = {};
    reset_sstv();
}

void Bridge::reset_sstv() {
    sstv_receiver_.reset(sstv_mode_);
    sstv_frame_.reset();
    last_sstv_audio_ = {};
    if (sstv_active_ && !stopping_) {
        send_rx2({{"type", "sstv_reset"}, {"session", session_}}, &Client::sstv_rx);
    }
}

void Bridge::set_sstv_mode(uint64_t client, const Json& request, const Json& id) {
    const auto name = request.at("mode").get<std::string>();
    std::optional<sstv::Mode> mode;
    if (name == "M1") mode = sstv::Mode::martin_m1;
    else if (name == "S1") mode = sstv::Mode::scottie_1;
    else if (name != "auto") throw std::runtime_error("Expected SSTV mode auto, M1 or S1");
    if (mode != sstv_mode_) {
        sstv_mode_ = mode;
        state_["sstv_mode"] = name;
        dirty_ = true;
        reset_sstv();
    }
    text(client, {{"type", "ack"}, {"command", "sstv_mode"}, {"id", id}, {"status", 0}});
}

void Bridge::publish_sstv(Clock::time_point now) {
    if (!sstv_active_ || stopping_) return;
    if (last_sstv_audio_ != Clock::time_point{} && now - last_sstv_audio_ > std::chrono::milliseconds(700)) {
        reset_sstv();
        return;
    }
    const auto output = sstv_receiver_.take_output();
    for (const auto& event : output.events) {
        if (std::holds_alternative<sstv::Receiver::Discontinuity>(event)) {
            sstv_frame_.reset();
            send_rx2({{"type", "sstv_reset"}, {"session", session_}}, &Client::sstv_rx);
        } else if (const auto* frame = std::get_if<sstv::Frame>(&event)) {
            sstv_frame_ = *frame;
            ++sstv_image_id_;
            send_rx2(sstv_begin(), &Client::sstv_rx);
        } else if (const auto* line = std::get_if<sstv::Line>(&event); line && sstv_frame_) {
            auto packet = header(sstv_line_type, 2, sstv_image_id_, session_);
            put(packet, 24, line->index, 8);
            put(packet, 44, uint32_t(std::lround(line->frequency_offset_hz)), 4);
            put(packet, 48, line->synchronized ? 1 : 0, 4);
            put(packet, 52, sstv::width, 2);
            packet.insert(packet.end(), line->rgb.begin(), line->rgb.end());
            send_rx2(std::make_shared<const Bytes>(std::move(packet)), true, &Client::sstv_rx);
        } else if (const auto* end = std::get_if<sstv::Receiver::End>(&event); end && sstv_frame_) {
            send_rx2({{"type", "sstv_end"}, {"session", session_},
                {"image", sstv_image_id_}, {"complete", end->complete && !sstv_frame_->partial}}, &Client::sstv_rx);
            sstv_frame_.reset();
        }
    }
    if (output.spectrum)
        send_rx2_spectrum(sstv_spectrum_type, *output.spectrum, &Client::sstv_rx);
}

}
