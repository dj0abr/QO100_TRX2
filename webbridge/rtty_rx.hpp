#pragma once

#include "audio_spectrum.hpp"

#include <string>
#include <vector>

namespace bridge {

class RttyReceiver {
public:
    struct Output {
        // A null entry signals a discontinuity between text chunks.
        std::vector<std::optional<std::string>> events;
        std::optional<qo100::audio::Spectrum> spectrum;
    };

    RttyReceiver();
    ~RttyReceiver();
    void push(qo100::audio::RxBlock block);
    void reset(bool reverse);
    Output take_output();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
