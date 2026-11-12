#include "bridge.hpp"

#include <algorithm>

namespace bridge {
namespace {

using qo100::sstv::Image;
using qo100::sstv::Sender;

constexpr size_t upload_chunk = 1024;
constexpr size_t raster_bytes = qo100::sstv::width * qo100::sstv::height * 3;
static_assert(sizeof(Image) == raster_bytes && raster_bytes % upload_chunk == 0);

}

void Bridge::begin_sstv_tx(uint64_t client, uint32_t upload_id, uint64_t frequency, unsigned level) {
    if (!can_start_tx2(client, upload_id, frequency, level)) return;

    sstv_upload_ = SstvUpload{upload_id, frequency, level,
        sstv_mode_.value_or(qo100::sstv::Mode::martin_m1), std::make_unique<Image>(), 0,
        Clock::now() + std::chrono::seconds(6)};
    tx2_state(Tx2Mode::sstv, "upload");
    schedule_tx2(sstv_upload_->deadline);
    text(client, {{"type", "ack"}, {"command", "sstv_tx_start"}, {"id", upload_id}, {"status", 0}});
}

void Bridge::sstv_tx_chunk(const Bytes& bytes) {
    if (!sstv_upload_) return;
    auto& job = *sstv_upload_;
    if (!valid(bytes) || bytes.size() != 64 + upload_chunk || get(bytes, 8, 4) != qo100::tx2_stream ||
        get(bytes, 12, 4) != job.id || get(bytes, 60, 4) != session_ ||
        get(bytes, 24, 8) != job.uploaded || Clock::now() > job.deadline) {
        end_tx2("upload_error");
        return;
    }
    auto* raster = reinterpret_cast<unsigned char*>(job.image.get());
    std::copy(bytes.begin() + 64, bytes.end(), raster + job.uploaded);
    job.uploaded += upload_chunk;
    if (job.uploaded != raster_bytes) return;

    if (digital_tx_busy() || pending_.count(tx2_key)) {
        end_tx2("tx2_busy");
        return;
    }
    auto upload = std::move(job);
    std::unique_ptr<Sender> sender;
    try {
        sender = std::make_unique<Sender>(upload.mode, *upload.image);
    } catch (const std::exception&) {
        end_tx2("unavailable");
        return;
    }
    sstv_upload_.reset();
    start_tx2(Tx2Mode::sstv, std::move(sender), upload.frequency, upload.level);
}

}
