#pragma once
#include <cstdint>

namespace qo100 {
// Per-modulator amount: 0 = bypass, 1000 = normal (100%), maximum 1400.
struct CompressorSettings {
    uint32_t amount = 0;
    bool valid() const { return amount <= 1400; }
};
}
