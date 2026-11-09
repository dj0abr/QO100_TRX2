// SPDX-License-Identifier: GPL-3.0-or-later
#include "pilot_afc.h"
#include <algorithm>
#include <cmath>

constexpr size_t PilotDetector::nfft;
constexpr size_t PilotDetector::capacity;

PilotDetector::PilotDetector()
    : ring(capacity), window(nfft), input(nfft), output(nfft) {
    for (size_t i = 0; i < nfft; ++i)
        window[i] = 0.5 - 0.5 * cos(2*M_PI*i/(nfft-1));
    plan = fft_create_plan(nfft, input.data(), output.data(), LIQUID_FFT_FORWARD, 0);
}

PilotDetector::~PilotDetector() { fft_destroy_plan(plan); }

bool PilotDetector::push(const float* data, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        ring[position] = std::isfinite(data[i]) ? data[i] : 0;
        position = (position + 1) % capacity;
    }
    available = std::min(capacity, available + count);
    since_scan += count;
    if (available < capacity || since_scan < 24000) return false;
    since_scan = 0;
    analyze();
    return true;
}

void PilotDetector::analyze() {
    // Three overlapping spectra suppress transient peaks in the data signal.
    const size_t bins = size_t((max_hz+40)*nfft/48000)+2;
    std::vector<double> power(bins, 0);
    for (size_t start = 0; start <= capacity-nfft; start += nfft/2) {
        for (size_t i = 0; i < nfft; ++i) {
            input[i].real = ring[(position + start + i) % capacity] * window[i];
            input[i].imag = 0;
        }
        fft_execute(plan);
        for (size_t i = 0; i < bins; ++i)
            power[i] += double(output[i].real)*output[i].real + double(output[i].imag)*output[i].imag;
    }
    found.clear();
    for (size_t i = 1; i+1 < bins; ++i) {
        double hz = i * 48000.0/nfft;
        if (hz < min_hz || hz > max_hz || power[i] <= power[i-1] || power[i] <= power[i+1]) continue;
        std::vector<double> background;
        for (int j = int(i)-17; j <= int(i)+17; ++j)
            if (j >= 0 && j < int(bins) && abs(j-int(i)) >= 3)
                background.push_back(power[j]);
        auto middle = background.begin() + background.size()/2;
        std::nth_element(background.begin(), middle, background.end());
        double prominence = 10*log10(std::max(power[i],1e-30)/std::max(*middle,1e-30));
        // Amplitude gate: avoid candidates caused by FFT roundoff on silence.
        double amplitude_dbfs = 10*log10(std::max(power[i] / 3 * 16/(nfft*nfft),1e-30));
        if (prominence < 12 || amplitude_dbfs < -110) continue;
        double a = log(std::max(power[i-1],1e-30)), b = log(std::max(power[i],1e-30));
        double c = log(std::max(power[i+1],1e-30));
        double denominator = a-2*b+c;
        double delta = denominator ? 0.5*(a-c)/denominator : 0;
        hz = (i + std::max(-0.5,std::min(0.5,delta))) * 48000.0/nfft;
        if (hz >= min_hz && hz <= max_hz) found.push_back({hz, prominence});
    }
    std::sort(found.begin(), found.end(), [](const PilotCandidate& a,const PilotCandidate& b) {
        return a.prominence_db > b.prominence_db;
    });
}
