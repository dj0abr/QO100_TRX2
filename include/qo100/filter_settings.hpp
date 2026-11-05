#pragma once
#include <cstdint>

namespace qo100 {
struct RxFilterSettings {
    uint32_t bandwidth_hz = 3900;
    uint32_t center_hz = 2050;
    bool valid() const {
        return bandwidth_hz >= 500 && bandwidth_hz <= 3900 && center_hz <= 4000 &&
            2*center_hz >= 200+bandwidth_hz && 2*center_hz+bandwidth_hz <= 8000;
    }
    double low_hz() const { return center_hz - bandwidth_hz/2.; }
    double high_hz() const { return center_hz + bandwidth_hz/2.; }
    bool operator==(const RxFilterSettings& other) const {
        return bandwidth_hz == other.bandwidth_hz && center_hz == other.center_hz;
    }
    bool operator!=(const RxFilterSettings& other) const { return !(*this == other); }
};
}
