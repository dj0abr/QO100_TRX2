#include "bridge.hpp"

#include <cmath>

namespace bridge {
namespace audio = qo100::audio;
namespace {
using Milliseconds = std::chrono::milliseconds;
using qo100::rtty::Encoder;
using qo100::sstv::Sender;

constexpr auto audio_period = Milliseconds(20);
constexpr unsigned prefill = 8;
constexpr unsigned rtty_tail_blocks = 13;
constexpr std::size_t rtty_text_limit = 512;
constexpr std::size_t rtty_pending_frames = 512;
constexpr auto rtty_ptt_lease = Milliseconds(1000);
static_assert(qo100::rtty::sample_rate == audio::sample_rate && qo100::sstv::sample_rate == audio::sample_rate);
}

bool Bridge::digital_tx_busy() const {
    const auto now = Clock::now();
    return now - digital_tx_audio_at_ < Milliseconds(250) ||
        (digital_nonce_ && now - digital_seen_ < std::chrono::seconds(2) &&
         (!digital_usage_known_ || (digital_usage_ & 2)));
}

bool Bridge::tx2_holds_audio() const {
    return tx2_job_.has_value() || tx2_releasing_epoch_.has_value();
}

bool Bridge::tx2_active(Tx2Mode mode) const {
    return (mode == Tx2Mode::sstv && sstv_upload_) || (tx2_job_ && tx2_job_->mode == mode);
}

void Bridge::tx2_state(Tx2Mode mode, const char* phase, const char* result, std::optional<uint32_t> id) {
    auto& tx = state_[mode == Tx2Mode::sstv ? "sstv_tx" : "rtty_tx"];
    tx = {{"phase", phase}, {"result", result}};
    if (mode == Tx2Mode::rtty && tx2_job_ && tx2_job_->rtty) id = tx2_job_->rtty->id;
    if (id) tx["tx_id"] = *id;
    dirty_ = true;
}

bool Bridge::tx2_ready(uint64_t frequency, Clock::time_point now) const {
    return ready() && !(state_["flags"].get<unsigned>() & 6) && !state_["loop"].value("mode", 0) &&
        frequency == state_["tx2"].value("frequency_hz", uint64_t(0)) &&
        now - last_tx2_state_ <= std::chrono::seconds(1);
}

bool Bridge::can_start_tx2(uint64_t client, uint32_t id, uint64_t frequency, unsigned level) {
    if (sstv_upload_ || tx2_holds_audio() || digital_tx_busy() || pending_.count(tx2_key)) {
        error(client, id, "tx2_busy", "TX2 is busy");
        return false;
    }
    if (!level) throw std::runtime_error("Set a TX level above zero");
    if (!tx2_ready(frequency, Clock::now())) {
        error(client, id, "not_ready", "TX2 is not ready");
        return false;
    }
    return true;
}

void Bridge::begin_rtty_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level, const std::string& message) {
    if (!can_start_tx2(client, id, frequency, level)) return;
    if (message.size() > rtty_text_limit * 2)
        throw std::runtime_error("RTTY text exceeds 512 characters after normalization");
    const auto normalized = qo100::rtty::normalize_text(message);
    if (normalized.size() > rtty_text_limit)
        throw std::runtime_error("RTTY text exceeds 512 characters after normalization");

    start_tx2(Tx2Mode::rtty, std::make_unique<Encoder>(normalized), frequency, level, RttyTx{id});
    text(client, {{"type", "ack"}, {"command", "rtty_tx_start"}, {"id", id},
                  {"status", 0}, {"text", normalized}});
}

void Bridge::open_rtty_tx(uint64_t client, uint32_t id, uint64_t frequency, unsigned level) {
    if (!can_start_tx2(client, id, frequency, level)) return;
    start_tx2(Tx2Mode::rtty, std::make_unique<Encoder>(), frequency, level,
              RttyTx{id, Clock::now() + rtty_ptt_lease});
    text(client, {{"type", "ack"}, {"command", "rtty_tx_open"}, {"id", id}, {"status", 0}});
}

bool Bridge::rtty_tx_matches(uint32_t id) const {
    return tx2_job_ && tx2_job_->rtty && tx2_job_->rtty->id == id;
}

void Bridge::keep_rtty_tx(uint32_t id) {
    if (!rtty_tx_matches(id)) return;
    const auto now = Clock::now();
    auto& until = tx2_job_->rtty->ptt_until;
    if (until && now < *until) until = now + rtty_ptt_lease;
}

void Bridge::write_rtty_tx(uint64_t client, const Json& id, uint32_t tx_id, const std::string& message, bool finish) {
    if (!rtty_tx_matches(tx_id) || !tx2_job_->rtty->ptt_until ||
        Clock::now() >= *tx2_job_->rtty->ptt_until) {
        error(client, id, "not_ready", "RTTY PTT is not active");
        return;
    }
    if (message.size() > rtty_pending_frames * 2) {
        error(client, id, "tx_queue_full", "RTTY send queue is full");
        return;
    }
    auto& encoder = *std::get<std::unique_ptr<Encoder>>(tx2_job_->source);
    const auto normalized = encoder.enqueue(message, rtty_pending_frames);
    if (!normalized) {
        error(client, id, "tx_queue_full", "RTTY send queue is full");
        return;
    }
    if (finish) {
        encoder.finish();
        tx2_job_->rtty->ptt_until.reset();
    }
    text(client, {{"type", "ack"}, {"command", "rtty_tx_write"}, {"id", id},
                  {"status", 0}, {"text", *normalized}});
}

void Bridge::start_tx2(Tx2Mode mode, Tx2Source source, uint64_t frequency, unsigned level,
                       std::optional<RttyTx> rtty) {
    tx2_job_ = Tx2Job{mode, std::move(source), frequency, level, Clock::now() + std::chrono::seconds(3), rtty};
    tx2_state(mode, "preparing");
    next_activity_ = {};
    update_digital_activity();
    schedule_tx2(Clock::now());
}

void Bridge::reset_tx2_audio(uint64_t frequency) {
    auto packet = header(6, qo100::tx2_stream);
    put(packet, 32, frequency, 8);
    put(packet, 54, 1, 2); // Reset the TX2 audio epoch and discard its FIFO.
    control(std::move(packet), 7);
}

std::optional<audio::TxBlock> Bridge::take_tx2_block(Tx2Job& job) {
    if (const auto* sstv = std::get_if<std::unique_ptr<Sender>>(&job.source))
        return (*sstv)->take(100);

    audio::TxBlock block;
    auto& encoder = *std::get<std::unique_ptr<Encoder>>(job.source);
    // Flush the backend FIFO and interpolator with the same tail as SSTV.
    if (job.tail || encoder.read(block.pcm.data(), block.pcm.size()) < block.pcm.size())
        block.last = ++job.tail == rtty_tail_blocks;
    return block;
}

void Bridge::send_tx2_audio(Tx2Job& job, const audio::Pcm& pcm) {
    auto packet = header(8, qo100::tx2_stream, job.epoch, session_);
    packet.resize(64 + pcm.size() * 2);
    put(packet, 24, job.sample_index, 8);
    put(packet, 32, job.frequency, 8);
    put(packet, 40, audio::sample_rate, 4);
    put(packet, 52, pcm.size(), 2);
    put(packet, 54, 2, 2);
    put(packet, 56, pcm.size(), 4);
    // Apply a live level change over a whole block to avoid an amplitude step.
    const double target_gain = job.level / 100.0;
    double gain = job.output_gain.value_or(target_gain);
    const double gain_step = (target_gain - gain) / pcm.size();
    for (size_t i = 0; i < pcm.size(); ++i) {
        gain += gain_step;
        put(packet, 64 + i * 2, uint16_t(std::lround(pcm[i] * gain)), 2);
    }
    job.output_gain = target_gain;
    send_udp(std::move(packet));
    job.sample_index += pcm.size();
}

void Bridge::schedule_tx2(Clock::time_point when) {
    const auto schedule = ++tx2_schedule_;
    tx2_timer_.expires_at(when);
    tx2_timer_.async_wait([this, schedule](auto error) {
        if (!error && schedule == tx2_schedule_) step_tx2();
    });
}

void Bridge::step_tx2() {
    if (sstv_upload_) {
        end_tx2("upload_error");
        return;
    }
    if (!tx2_job_) return;
    auto& job = *tx2_job_;
    const auto now = Clock::now();
    if (job.rtty && job.rtty->ptt_until && now >= *job.rtty->ptt_until) {
        end_tx2("ptt_timeout");
        return;
    }
    if (!owner_ || !tx2_ready(job.frequency, now)) {
        end_tx2("unavailable");
        return;
    }

    const auto& tx = state_["tx2"];
    const auto epoch = tx.at("epoch").get<uint32_t>();
    const auto available = tx.value("ptt", false);
    if (job.phase == Tx2Job::Phase::preparing) {
        if (now >= job.deadline) {
            end_tx2("unavailable");
            return;
        }
        const auto* sstv = std::get_if<std::unique_ptr<Sender>>(&job.source);
        if (!job.epoch_requested && available && !pending_.count(tx2_key)) {
            job.epoch_requested = true;
            job.epoch = epoch;
            reset_tx2_audio(job.frequency);
        } else if (job.epoch_requested && available && !pending_.count(tx2_key) &&
                   epoch != job.epoch && !tx.value("status", 0) && (!sstv || (*sstv)->available() >= prefill)) {
            job.epoch = epoch;
            // Both sources include a silence tail longer than the prefill.
            for (unsigned i = 0; i < prefill; ++i)
                send_tx2_audio(job, take_tx2_block(job)->pcm);
            job.phase = Tx2Job::Phase::sending;
            job.next_audio = now + audio_period;
            tx2_state(job.mode, "sending");
        }
        schedule_tx2(now + audio_period);
        return;
    }

    if (!available || job.epoch != epoch) {
        end_tx2("unavailable");
        return;
    }
    if (job.phase == Tx2Job::Phase::draining) {
        end_tx2("complete");
        return;
    }
    const auto block = take_tx2_block(job);
    if (!block || now - job.next_audio > Milliseconds(100)) {
        end_tx2("underrun");
        return;
    }
    send_tx2_audio(job, block->pcm);
    if (block->last) {
        job.phase = Tx2Job::Phase::draining;
        schedule_tx2(now + Milliseconds(220));
    } else {
        job.next_audio += audio_period;
        schedule_tx2(job.next_audio);
    }
}

void Bridge::end_tx2(const char* result, bool flush) {
    if (!sstv_upload_ && !tx2_job_) return;
    ++tx2_schedule_;
    tx2_timer_.cancel();
    if (sstv_upload_) {
        sstv_upload_.reset();
        tx2_state(Tx2Mode::sstv, "idle", result);
        return;
    }

    const auto mode = tx2_job_->mode;
    const auto frequency = tx2_job_->frequency;
    const auto rtty = tx2_job_->rtty;
    tx2_job_.reset();
    if (flush && session_ && state_["tx2"].value("frequency_hz", uint64_t(0)) == frequency) {
        tx2_releasing_epoch_ = state_["tx2"].at("epoch").get<uint32_t>();
        reset_tx2_audio(frequency);
    }
    next_activity_ = {};
    tx2_state(mode, "idle", result, rtty ? std::optional{rtty->id} : std::nullopt);
}

}
