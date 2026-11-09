// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <liquid/liquid.h>
#include <cstddef>
#include <vector>

struct PilotCandidate {
    double hz;
    double prominence_db;
};

// Pilot estimator independent of the decoder. All timing uses audio samples.
class PilotDetector {
public:
    static constexpr double min_hz = 20, max_hz = 900;
    PilotDetector();
    ~PilotDetector();
    PilotDetector(const PilotDetector&) = delete;
    PilotDetector& operator=(const PilotDetector&) = delete;
    bool push(const float* data, size_t count);
    const std::vector<PilotCandidate>& candidates() const { return found; }
private:
    static constexpr size_t nfft = 32768, capacity = 65536;
    std::vector<float> ring, window;
    std::vector<liquid_float_complex> input, output;
    std::vector<PilotCandidate> found;
    fftplan plan;
    size_t position = 0, available = 0, since_scan = 0;
    void analyze();
};
