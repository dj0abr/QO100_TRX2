#include "timing.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace qo100::sstv {
namespace {

using namespace detail;
constexpr int filter_delay = 20;
constexpr std::size_t filter_length = 2 * filter_delay + 1;
constexpr std::size_t history_size = 32768;
constexpr int detector_step = 6;
constexpr double acquisition_hz = 350;
constexpr double afc_time_seconds = 6;
constexpr double afc_max_error_hz = 30;
constexpr unsigned line_clock_acquisition = 4;
constexpr double line_phase_gain = 0.25;
constexpr double line_period_gain = 0.12;
constexpr double line_sync_lock_us = 2500;
constexpr double line_sync_confirmation_us = 1500;
constexpr double line_sync_confirmation_hz = 90;
constexpr unsigned estimated_lines = 3;
constexpr unsigned partial_sync_timeout_lines = 16;

struct Measurement {
    double hz = 0;
    double power = 0;
};

struct ToneRun {
    std::int64_t start = 0;
    double hz = 0;
    bool valid = false;
    bool falling_edge = false;
};

struct Pulse {
    double start;
    double length;
    double offset;
};

constexpr std::array<Mode, 2> modes{Mode::martin_m1, Mode::scottie_1};

}

struct Decoder::Impl {
    Callbacks callbacks;
    std::optional<Mode> forced_mode;
    std::array<double, filter_delay + 1> in_phase{}, quadrature{};
    std::array<double, filter_length> audio{};
    std::array<double, filter_length> dial{};
    std::array<Measurement, history_size> history{};
    struct Analytic { float i, q, dial_hz; };
    std::array<Analytic, history_size> analytic{};
    double previous_i = 0;
    double previous_q = 0;
    double dial_shift = 0;
    std::int64_t clock = 0;

    ToneRun run;
    struct Vis {
        std::int64_t start;
        double offset;
    };
    std::optional<Vis> vis;
    std::array<std::optional<Pulse>, 2> acquisition{};
    std::array<unsigned, 2> pulse_count{};

    struct Reception {
        Timing format;
        unsigned row = 0;
        double offset;
        double period;
        double predicted_sync;
        std::optional<Pulse> matched;
        std::optional<Pulse> following;
        std::optional<double> last_sync;
        unsigned last_sync_row = 0;
        unsigned missing = 0;
        bool partial = false;
        bool has_gaps = false;
        std::optional<Pulse> off_phase_sync{};
    };
    std::optional<Reception> frame;

    Impl(Callbacks cb, std::optional<Mode> mode) : callbacks(std::move(cb)), forced_mode(mode) {
        // Complex band-pass FIR for the SSTV tones and the AFC capture range.
        // Rejecting the rest of the RX2 passband before differentiating phase
        // keeps out-of-band noise from breaking short sync pulses.
        constexpr double low = 2 * pi * (sync_hz - acquisition_hz - 100) / sample_rate;
        constexpr double high = 2 * pi * (white_hz + acquisition_hz) / sample_rate;
        in_phase[0] = (high - low) / pi;
        for (int k = 1; k <= filter_delay; ++k) {
            const double window = 0.54 + 0.46 * std::cos(pi * k / filter_delay);
            in_phase[k] = window * (std::sin(high * k) - std::sin(low * k)) / (pi * k);
            quadrature[k] = window * (std::cos(low * k) - std::cos(high * k)) / (pi * k);
        }
    }

    void push_one(std::int16_t pcm) {
        const auto pos = std::size_t(clock) % filter_length;
        audio[pos] = pcm / 32768.0;
        dial[pos] = dial_shift;
        const auto delayed = (pos + filter_length - filter_delay) % filter_length;
        double i = in_phase[0] * audio[delayed];
        double q = 0;
        for (int k = 1; k <= filter_delay; ++k) {
            const auto newer = audio[(delayed + k) % filter_length];
            const auto older = audio[(delayed + filter_length - k) % filter_length];
            i += in_phase[k] * (older + newer);
            q += quadrature[k] * (older - newer);
        }
        analytic[std::size_t(clock) % history_size] = {float(i), float(q), float(dial[delayed])};
        const auto power = std::min(i * i + q * q, previous_i * previous_i + previous_q * previous_q);
        auto& value = history[std::size_t(clock) % history_size];
        value = {};
        if (power > 0.000001) {
            const double phase = std::atan2(q * previous_i - i * previous_q, i * previous_i + q * previous_q);
            // Keep the tone coordinate continuous across manual USB tuning.
            value = {phase * sample_rate / (2 * pi) + dial[delayed], power};
        }
        previous_i = i;
        previous_q = q;
        ++clock;
        if (clock >= 48 && clock % detector_step == 0) tick(clock - 36);
    }

    Measurement at(std::int64_t index) const {
        if (index < 0 || index >= clock || clock - index > std::int64_t(history_size)) return {};
        return history[std::size_t(index) % history_size];
    }

    Measurement mean(double center, int radius) const {
        const auto middle = std::llround(center);
        double frequency = 0;
        double weight = 0;
        int valid = 0;
        for (auto n = middle - radius; n <= middle + radius; ++n) {
            const auto sample = at(n);
            if (sample.power == 0) continue;
            frequency += sample.hz * sample.power;
            weight += sample.power;
            ++valid;
        }
        if (valid <= radius) return {};
        return {frequency / weight, weight / valid};
    }

    double correlated_frequency(double center, double estimate, int radius,
                                std::int64_t first, std::int64_t last) const {
        // Integrate complex correlations before estimating phase. The
        // triangular window smooths the video without crossing color scans.
        constexpr int half_lag = 4;
        const auto middle = std::llround(center);
        const auto from = std::max<std::int64_t>(first + half_lag, middle - radius);
        const auto to = std::min<std::int64_t>(last - half_lag, middle + radius);
        double re = 0, im = 0;
        for (auto n = from; n <= to; ++n) {
            const auto a = analytic[std::size_t(n - half_lag) % history_size];
            const auto b = analytic[std::size_t(n + half_lag) % history_size];
            const auto weight = radius + 1 - std::abs(n - middle);
            re += weight * (a.i * b.i + a.q * b.q);
            im += weight * (a.i * b.q - a.q * b.i);
        }
        if (re == 0 && im == 0) return estimate;
        constexpr double wrap_hz = double(sample_rate) / (2 * half_lag);
        const auto tuned = analytic[std::size_t(middle) % history_size].dial_hz;
        const double hz = std::atan2(im, re) * wrap_hz / (2 * pi) + tuned;
        return hz + wrap_hz * std::round((estimate - hz) / wrap_hz);
    }

    Measurement tone_at(double center, int radius) const {
        auto tone = mean(center, radius);
        if (tone.power) tone.hz = correlated_frequency(center, tone.hz, radius,
            std::max<std::int64_t>(0, clock - std::int64_t(history_size)), clock - 1);
        return tone;
    }

    bool near_tone(double center, double hz, double tolerance) const {
        const auto tone = tone_at(center, 36);
        return tone.power > 0 && std::abs(tone.hz - hz) < tolerance;
    }

    void tick(std::int64_t now) {
        const auto tone = tone_at(now, 12);
        const bool valid = tone.power > 0;
        if (valid != run.valid || (valid && std::abs(tone.hz - run.hz) > 150)) {
            if (run.valid) end_tone(now);
            const bool falling = run.valid && run.hz - tone.hz > 155;
            run = {now, tone.hz, valid, falling};
        } else if (valid) {
            run.hz += 0.10 * (tone.hz - run.hz);
        }
        if (vis && now >= vis->start + samples(vis_data_us - 5000)) check_vis();
        if (frame) decode_ready_line(now);
    }

    double rising_edge(std::int64_t around, double before, double threshold) const {
        for (auto n = around - 10; n <= around + 10; ++n) {
            const auto previous = at(n - 1);
            const auto next = at(n);
            if (previous.power && next.power && previous.hz < before + threshold &&
                next.hz >= before + threshold) {
                return n - 1 + (before + threshold - previous.hz) / (next.hz - previous.hz);
            }
        }
        return around;
    }

    void end_tone(std::int64_t end) {
        const auto duration = end - run.start;
        const auto after = tone_at(end + 18, 9);
        if (duration >= samples(240000) && duration <= samples(365000)) {
            const auto leader = tone_at(run.start + duration * 0.5, 96);
            const double offset = leader.hz - leader_hz;
            if (leader.power && std::abs(offset - dial_shift) <= acquisition_hz &&
                after.power && std::abs(after.hz - (sync_hz + offset)) < 170 &&
                near_tone(end - samples(450000), leader_hz + offset, 55)) {
                vis = Vis{end, offset};
            }
        }

        // The first Martin pulse can run directly on from the VIS stop bit.
        const bool first_martin_sync = frame && frame->row == 0 && frame->format.sync_start_us == 0 &&
            std::abs(end - frame->predicted_sync - samples(frame->format.sync_us)) < samples(16000) &&
            duration >= samples(24000) && duration <= samples(42000);
        if (!first_martin_sync && (duration < samples(2300) || duration > samples(12500))) return;
        const auto middle = tone_at(run.start + duration * 0.5, 12);
        if (!middle.power || !after.power || after.hz - middle.hz < 145) return;
        const double offset = middle.hz - sync_hz;
        if (std::abs(offset - dial_shift) > acquisition_hz && !frame) return;
        const double edge = rising_edge(end, middle.hz, 150);
        pulse({edge - duration, first_martin_sync ? samples(frame->format.sync_us) : double(duration), offset},
            edge, first_martin_sync, tone_at(run.start - 20, 10));
    }

    void check_vis() {
        const auto header = *vis;
        vis.reset();
        const auto bit_center = [&](unsigned bit) {
            return header.start + samples((bit + 0.5) * vis_bit_us);
        };
        if (!near_tone(bit_center(0), sync_hz + header.offset, 60) ||
            !near_tone(bit_center(9), sync_hz + header.offset, 60)) return;
        unsigned code = 0;
        unsigned parity = 0;
        for (unsigned i = 0; i < 8; ++i) {
            const auto tone = tone_at(bit_center(i + 1), 60);
            const double hz = tone.hz - header.offset;
            if (!tone.power || std::min(std::abs(hz - 1100), std::abs(hz - 1300)) > 65) return;
            const unsigned bit = hz < sync_hz;
            parity ^= bit;
            code |= bit << i;
        }
        if (parity) return;
        for (const auto mode : modes) {
            if (code % 128 != timing(mode).vis || (forced_mode && *forced_mode != mode)) continue;
            const auto format = timing(mode);
            const auto anchor = header.start + samples(vis_data_us + format.initial_sync_us + format.sync_start_us);
            begin(mode, false, anchor, header.offset, samples(format.line_us));
            // For Martin, the first sync is contiguous with the VIS stop bit.
            if (mode == Mode::martin_m1) frame->matched = Pulse{anchor, samples(format.sync_us), header.offset};
        }
    }

    void pulse(const Pulse& found, double end, bool replace, Measurement lead) {
        if (frame) {
            auto& current = *frame;
            const auto expected_length = samples(current.format.sync_us);
            Pulse candidate{end - expected_length, found.length, found.offset};
            // Keep a tight gate while locked. Uncertainty grows while the
            // receiver extrapolates the line clock without valid sync.
            const auto window = current.last_sync
                ? samples(5500 + 2000 * std::min(current.missing, 12u))
                : samples(20000);
            if (std::abs(found.length - expected_length) > samples(2900) ||
                std::abs(candidate.offset - current.offset) > 145) return;
            const auto match = [&](double predicted, std::optional<Pulse>& slot) {
                const auto error = std::abs(candidate.start - predicted);
                if (error <= window && (replace || !slot || error < std::abs(slot->start - predicted)))
                    slot = candidate;
            };
            match(current.predicted_sync, current.matched);
            // The next pulse can arrive before the current line is emitted,
            // particularly when the VIS-derived timing was a little late.
            match(current.predicted_sync + current.period, current.following);
            return;
        }

        // An edge spread across several detector ticks by noise or filtering
        // can still be identified from the tone preceding the pulse.
        if (!run.falling_edge && (!lead.power || lead.hz - (sync_hz + found.offset) < 125)) return;
        for (std::size_t i = 0; i < modes.size(); ++i) {
            const auto mode = modes[i];
            if (forced_mode && *forced_mode != mode) continue;
            const auto format = timing(mode);
            const auto length = samples(format.sync_us);
            if (std::abs(found.length - length) > samples(2300)) continue;
            const double start = end - length;
            const double period = samples(format.line_us);
            const auto previous = acquisition[i];
            const double interval = previous ? start - previous->start : 0;
            if (previous && interval < period * 0.97) continue;
            acquisition[i] = Pulse{start, found.length, found.offset};
            if (!previous || std::abs(interval - period) > period * 0.03 ||
                std::abs(found.offset - previous->offset) > 100) {
                pulse_count[i] = 1;
            } else if (++pulse_count[i] >= 4) {
                begin(mode, true, start, (found.offset + previous->offset) * 0.5, interval);
                frame->matched = acquisition[i];
                return;
            }
        }
    }

    void begin(Mode mode, bool partial, double sync, double offset, double period) {
        end_frame(false);
        frame.emplace(Reception{timing(mode), 0, offset, period, sync, {}, {}, {}, 0, 0, partial});
        if (callbacks.begin) callbacks.begin({mode, partial});
    }

    void decode_ready_line(std::int64_t now) {
        auto& current = *frame;
        const auto& format = current.format;
        const auto nominal_period = samples(format.line_us);
        const bool established = current.last_sync && current.row >= line_clock_acquisition;
        const auto candidate = current.matched;
        // Once locked, a pulse far from the row clock can be interference.
        // Re-anchor only after two consecutive off-phase pulses agree.
        const bool off_phase = established && candidate &&
            std::abs(candidate->start - current.predicted_sync) > samples(line_sync_lock_us);
        const bool confirmed = off_phase && current.off_phase_sync &&
            std::abs(candidate->start - current.off_phase_sync->start - current.period) <=
                samples(line_sync_confirmation_us) &&
            std::abs(candidate->offset - current.off_phase_sync->offset) <= line_sync_confirmation_hz;
        const bool synchronized = candidate && (!off_phase || confirmed);
        const bool tracking = established && !confirmed && current.missing < estimated_lines;
        const auto observed_sync = synchronized ? candidate->start : current.predicted_sync;
        const double sync = current.predicted_sync +
            (tracking ? line_phase_gain : 1.0) * (observed_sync - current.predicted_sync);
        const double scale = current.period / nominal_period;
        const double line_start = sync - samples(format.sync_start_us) * scale;
        if (now < line_start + current.period + 8) return;

        current.off_phase_sync = off_phase && !confirmed ? candidate : std::nullopt;
        if (synchronized) {
            // Average the tone frequency independently of the line clock.
            // Limit the influence of a single noisy pulse, including after
            // a gap. Manual tuning is already accounted for by dial_shift.
            const double error = std::clamp(candidate->offset - current.offset,
                                            -afc_max_error_hz, afc_max_error_hz);
            const double gain = -std::expm1(-current.period / (sample_rate * afc_time_seconds));
            current.offset += gain * error;
            if (current.last_sync) {
                const double interval = (observed_sync - *current.last_sync) / (current.row - current.last_sync_row);
                if (std::abs(interval - nominal_period) < nominal_period * 0.03)
                    current.period += (tracking ? line_period_gain : 0.24) * (interval - current.period);
            }
            current.last_sync = observed_sync;
            current.last_sync_row = current.row;
            current.missing = 0;
        } else {
            ++current.missing;
            // A VIS start gives us an absolute row clock through the end of
            // the picture. A late entry has no known end and can time out.
            if (current.partial && current.missing >= partial_sync_timeout_lines) {
                end_frame(false);
                return;
            }
        }

        if (current.missing <= estimated_lines) {
            Line line{current.row, {}, synchronized, current.offset - dial_shift};
            const double pixel = samples(format.scan_us) * scale / width;
            for (unsigned color = 0; color < 3; ++color) {
                const double start = line_start + samples(format.color_start_us[color]) * scale;
                const auto first = std::max<std::int64_t>(std::ceil(start), 0);
                const auto last = std::min<std::int64_t>(std::ceil(start + width * pixel) - 1, clock - 1);
                for (unsigned x = 0; x < width; ++x) {
                    const auto center = start + (x + 0.5) * pixel;
                    const auto tone = mean(center, 3);
                    if (tone.power) {
                        const double hz = correlated_frequency(center, tone.hz, 11, first, last);
                        const double brightness = (hz - current.offset - black_hz) / (white_hz - black_hz);
                        line.rgb[3 * x + color] = std::uint8_t(std::lround(255 * std::clamp(brightness, 0.0, 1.0)));
                    }
                }
            }
            if (callbacks.line) callbacks.line(line);
        } else {
            // Leave a gap in the raster while retaining the absolute row.
            current.has_gaps = true;
        }

        if (++current.row == height) {
            end_frame(!current.has_gaps);
        } else {
            current.predicted_sync = sync + current.period;
            current.matched = std::exchange(current.following, std::nullopt);
        }
    }

    void end_frame(bool complete) {
        if (!frame) return;
        frame.reset();
        acquisition = {};
        pulse_count = {};
        if (callbacks.end) callbacks.end(complete);
    }
};

Decoder::Decoder(Callbacks callbacks, std::optional<Mode> forced_mode)
    : impl_(std::make_unique<Impl>(std::move(callbacks), forced_mode)) {}
Decoder::~Decoder() = default;

void Decoder::push(const std::int16_t* pcm, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) impl_->push_one(pcm[i]);
}

void Decoder::retune(double rx_delta_hz) {
    if (std::isfinite(rx_delta_hz)) impl_->dial_shift += rx_delta_hz;
}

void Decoder::finish() {
    std::array<std::int16_t, 96> silence{};
    push(silence.data(), silence.size());
    reset();
}

void Decoder::reset() {
    impl_->end_frame(false);
    auto callbacks = std::move(impl_->callbacks);
    const auto mode = impl_->forced_mode;
    impl_ = std::make_unique<Impl>(std::move(callbacks), mode);
}

}
