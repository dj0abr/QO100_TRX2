#pragma once

#include "sstv.hpp"

namespace qo100::sstv {

class Sender {
public:
    static constexpr std::size_t block_size = 240;
    using Pcm = std::array<std::int16_t, block_size>;
    struct Block {
        Pcm pcm{};
        bool last = false;
    };

    Sender(Mode mode, const Image& image);
    ~Sender();
    // Apply the current level (0..100%) when the consumer takes a block.
    std::optional<Block> take(unsigned level_percent);
    std::size_t available() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
