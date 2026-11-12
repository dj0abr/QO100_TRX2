#include "bridge.hpp"

#include <algorithm>

namespace bridge {
namespace {

using qo100::sstv::Image;
using qo100::sstv::Sender;
using Milliseconds = std::chrono::milliseconds;

constexpr auto audio_period = Milliseconds(20);
constexpr auto tx2_key = (uint64_t(6) << 32) | qo100::tx2_stream;
constexpr size_t upload_chunk = 1024;
constexpr size_t raster_bytes = qo100::sstv::width * qo100::sstv::height * 3;
constexpr unsigned prefill = 8;
static_assert(sizeof(Image) == raster_bytes && raster_bytes % upload_chunk == 0);

}

bool Bridge::digital_tx_busy() const {
    const auto now = Clock::now();
    return now - digital_tx_audio_at_ < Milliseconds(250) ||
        (digital_nonce_ && now - digital_seen_ < std::chrono::seconds(2) &&
         (!digital_usage_known_ || (digital_usage_ & 2)));
}

bool Bridge::sstv_holds_tx2() const {
    return sstv_tx_releasing_ || (sstv_tx_ && sstv_tx_->phase != SstvTx::Phase::upload);
}

void Bridge::sstv_tx_state(const char* phase, const char* result) {
    state_["sstv_tx"] = {{"phase", phase}, {"result", result}};
    dirty_ = true;
}

void Bridge::begin_sstv_tx(uint64_t client, uint32_t upload_id, uint64_t frequency, unsigned level) {
    if (sstv_tx_ || sstv_tx_releasing_ || digital_tx_busy() || pending_.count(tx2_key)) {
        error(client, upload_id, "tx2_busy", "TX2 is busy");
        return;
    }
    if (!level) throw std::runtime_error("Set a TX level above zero");
    if ((state_["flags"].get<unsigned>() & 6) || state_["loop"].value("mode", 0) ||
        frequency != state_["tx2"].value("frequency_hz", uint64_t(0)) ||
        Clock::now() - last_tx2_state_ > std::chrono::seconds(1)) {
        error(client, upload_id, "not_ready", "TX2 is not ready");
        return;
    }

    auto image = std::make_unique<Image>();
    auto& job = sstv_tx_.emplace();
    job.upload_id = upload_id;
    job.frequency = frequency;
    job.level = level;
    job.mode = sstv_mode_.value_or(qo100::sstv::Mode::martin_m1);
    job.image = std::move(image);
    job.deadline = Clock::now() + std::chrono::seconds(6);
    sstv_tx_state("upload");
    schedule_sstv_tx(job.deadline);
    text(client, {{"type", "ack"}, {"command", "sstv_tx_start"}, {"id", upload_id}, {"status", 0}});
}

void Bridge::sstv_tx_chunk(const Bytes& bytes) {
    if (!sstv_tx_ || sstv_tx_->phase != SstvTx::Phase::upload) return;
    auto& job = *sstv_tx_;
    if (!valid(bytes) || bytes.size() != 64 + upload_chunk || get(bytes, 8, 4) != qo100::tx2_stream ||
        get(bytes, 12, 4) != job.upload_id || get(bytes, 60, 4) != session_ ||
        get(bytes, 24, 8) != job.uploaded || Clock::now() > job.deadline) {
        end_sstv_tx("upload_error");
        return;
    }
    auto* raster = reinterpret_cast<unsigned char*>(job.image.get());
    std::copy(bytes.begin() + 64, bytes.end(), raster + job.uploaded);
    job.uploaded += upload_chunk;
    if (job.uploaded != raster_bytes) return;

    if (digital_tx_busy() || pending_.count(tx2_key)) {
        end_sstv_tx("tx2_busy");
        return;
    }
    try {
        job.sender = std::make_unique<Sender>(job.mode, *job.image);
    } catch (const std::exception&) {
        end_sstv_tx("unavailable");
        return;
    }
    job.image.reset();
    job.phase = SstvTx::Phase::preparing;
    job.deadline = Clock::now() + std::chrono::seconds(3);
    sstv_tx_state("preparing");
    next_activity_ = {};
    update_digital_activity();
    schedule_sstv_tx(Clock::now());
}

void Bridge::reset_tx2_audio(uint64_t frequency) {
    auto packet = header(6, qo100::tx2_stream);
    put(packet, 32, frequency, 8);
    put(packet, 54, 1, 2); // Reset the TX2 audio epoch and discard its FIFO.
    control(std::move(packet), 7);
}

void Bridge::send_sstv_tx_audio(SstvTx& job, const Sender::Pcm& pcm) {
    auto packet = header(8, qo100::tx2_stream, job.epoch, session_);
    packet.resize(64 + pcm.size() * 2);
    put(packet, 24, job.sample_index, 8);
    put(packet, 32, job.frequency, 8);
    put(packet, 40, qo100::sstv::sample_rate, 4);
    put(packet, 52, pcm.size(), 2);
    put(packet, 54, 2, 2);
    put(packet, 56, pcm.size(), 4);
    for (size_t i = 0; i < pcm.size(); ++i) put(packet, 64 + i * 2, uint16_t(pcm[i]), 2);
    send_udp(std::move(packet));
    job.sample_index += pcm.size();
}

void Bridge::schedule_sstv_tx(Clock::time_point when) {
    sstv_tx_timer_.expires_at(when);
    sstv_tx_timer_.async_wait([this](auto error) { if (!error) step_sstv_tx(); });
}

void Bridge::step_sstv_tx() {
    if (!sstv_tx_) return;
    auto& job = *sstv_tx_;
    const auto now = Clock::now();
    const auto& tx = state_["tx2"];
    if (job.phase == SstvTx::Phase::upload) {
        end_sstv_tx("upload_error");
        return;
    }
    if (!ready() || !owner_ || (state_["flags"].get<unsigned>() & 6) ||
        state_["loop"].value("mode", 0) || tx.value("frequency_hz", uint64_t(0)) != job.frequency ||
        now - last_tx2_state_ > std::chrono::seconds(1)) {
        end_sstv_tx("unavailable");
        return;
    }

    const auto epoch = tx.at("epoch").get<uint32_t>();
    const auto available = tx.value("ptt", false);
    if (job.phase == SstvTx::Phase::preparing) {
        if (now >= job.deadline) {
            end_sstv_tx("unavailable");
            return;
        }
        if (!job.epoch_requested && available && !pending_.count(tx2_key)) {
            job.epoch_requested = true;
            job.epoch = epoch;
            reset_tx2_audio(job.frequency);
        } else if (job.epoch_requested && available && !pending_.count(tx2_key) &&
                   epoch != job.epoch && !tx.value("status", 0) && job.sender->available() >= prefill) {
            job.epoch = epoch;
            for (unsigned i = 0; i < prefill; ++i)
                send_sstv_tx_audio(job, job.sender->take(job.level)->pcm);
            job.phase = SstvTx::Phase::sending;
            job.next_audio = now + audio_period;
            sstv_tx_state("sending");
        }
        schedule_sstv_tx(now + audio_period);
        return;
    }

    if (!available || job.epoch != epoch) {
        end_sstv_tx("unavailable");
        return;
    }
    if (job.phase == SstvTx::Phase::draining) {
        end_sstv_tx("complete");
        return;
    }
    const auto block = job.sender->take(job.level);
    if (!block || now - job.next_audio > Milliseconds(100)) {
        end_sstv_tx("underrun");
        return;
    }
    send_sstv_tx_audio(job, block->pcm);
    if (block->last) {
        job.phase = SstvTx::Phase::draining;
        schedule_sstv_tx(now + Milliseconds(220));
    } else {
        job.next_audio += audio_period;
        schedule_sstv_tx(job.next_audio);
    }
}

void Bridge::end_sstv_tx(const char* result, bool flush) {
    if (!sstv_tx_) return;
    sstv_tx_timer_.cancel();
    const auto frequency = sstv_tx_->frequency;
    const bool owned = sstv_tx_->phase != SstvTx::Phase::upload;
    sstv_tx_.reset();
    if (flush && owned && session_ && state_["tx2"].value("frequency_hz", uint64_t(0)) == frequency) {
        sstv_tx_releasing_ = true;
        reset_tx2_audio(frequency);
    }
    next_activity_ = {};
    sstv_tx_state("idle", result);
}

}
