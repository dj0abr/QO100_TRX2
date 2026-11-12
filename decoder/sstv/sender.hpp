#pragma once

#include "sstv.hpp"
#include "../audio.hpp"

namespace qo100::sstv {

class Sender {
public:
    static constexpr auto block_size = audio::block_size;
    using Pcm = audio::Pcm;
    using Block = audio::TxBlock;

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
