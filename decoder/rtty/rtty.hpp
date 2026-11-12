#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace qo100::rtty {

inline constexpr unsigned sample_rate = 12000;
inline constexpr unsigned mark_hz = 1000;
inline constexpr unsigned space_hz = 830;

// Convert UTF-8 to the supported ITA2 text, using uppercase and LF.
// German umlauts and sharp S are transliterated. Unsupported input throws.
std::string normalize_text(std::string_view text);

// Each instance belongs to one thread, like the SSTV codec.
class Encoder {
public:
    // Continuous encoder. An empty queue emits repeated ITA2 LTRS frames.
    Encoder();
    explicit Encoder(std::string_view text, unsigned level_percent = 100);
    ~Encoder();

    // Queue a complete input atomically, returning its normalized text.
    // A full queue returns nullopt. The limit counts pending ITA2 frames.
    std::optional<std::string> enqueue(std::string_view text, std::size_t frame_limit);

    // Finish the current frame and queued text, then end the stream.
    void finish();

    // Write up to capacity samples. A short read marks the end of the message.
    std::size_t read(std::int16_t* pcm, std::size_t capacity);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Decoder {
public:
    struct Options {
        bool reverse = false;
        bool unshift_on_space = true;
    };

    // Callbacks run synchronously in push() and receive CR and LF as sent.
    using Character = std::function<void(char)>;
    explicit Decoder(Character character);
    Decoder(Character character, Options options);
    ~Decoder();

    void push(const std::int16_t* pcm, std::size_t count);
    // Drop a partial character, filter history and the current letters/figures shift.
    void reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
