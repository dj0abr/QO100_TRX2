#include "rtty/rtty.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace qo100::rtty;

namespace {
constexpr double pi = 3.14159265358979323846;
constexpr unsigned bit = 264;
constexpr unsigned frame = 1980;
using Pcm = std::vector<std::int16_t>;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

template<class Action>
void rejects(Action action) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Expected invalid_argument");
}

Pcm encode(std::string_view text, unsigned block = 240, unsigned level = 100) {
    Encoder encoder(text, level);
    Pcm pcm;
    std::vector<std::int16_t> buffer(block);
    for (;;) {
        const auto size = encoder.read(buffer.data(), buffer.size());
        pcm.insert(pcm.end(), buffer.begin(), buffer.begin() + size);
        if (size < buffer.size()) break;
    }
    require(encoder.read(buffer.data(), buffer.size()) == 0, "Encoder did not stay at EOF");
    return pcm;
}

void feed(Decoder& decoder, const Pcm& pcm, unsigned block = 240) {
    for (std::size_t i = 0; i < pcm.size(); i += block) {
        decoder.push(pcm.data() + i, std::min<std::size_t>(block, pcm.size() - i));
    }
}

double power(const Pcm& pcm, std::size_t start, unsigned count, unsigned hz) {
    double i = 0, q = 0;
    for (unsigned n = 0; n < count; ++n) {
        const auto phase = 2 * pi * hz * n / 12000;
        i += pcm.at(start + n) * std::cos(phase);
        q += pcm.at(start + n) * std::sin(phase);
    }
    return i * i + q * q;
}

void check_tone(const Pcm& pcm, std::size_t start, unsigned count, bool mark) {
    require(power(pcm, start, count, mark ? 1000 : 830) >
                10 * power(pcm, start, count, mark ? 830 : 1000),
            "Incorrect RTTY TX tone, bit order or frame duration");
}

void test_encoder() {
    require(normalize_text(u8"Füße ÄÖÜẞ\r\na\rb!?") == "FUESSE AEOEUESS\nA\nB!?",
            "Incorrect normalization");
    require(normalize_text("PA\\\n2m Dish\\\r\nAdalm Pluto") == "PA\n2M DISH\nADALM PLUTO",
            "Incorrect normalization of pasted line breaks");
    rejects([] { normalize_text("TEST @"); });
    rejects([] { normalize_text(std::string_view("A\0B", 3)); });
    rejects([] { normalize_text("\xc3"); });
    rejects([] { Encoder encoder(" \r\n"); });
    rejects([] { Encoder encoder("A", 101); });

    const auto pcm = encode("a1 b");
    require(pcm == encode("A1 B", 17), "Encoder depends on the output block size");
    constexpr std::array<unsigned, 8> codes{31, 31, 3, 27, 23, 4, 31, 25};
    require(pcm.size() == 15 * bit + codes.size() * frame, "Incorrect TX duration");
    check_tone(pcm, 0, 12 * bit, true);
    std::size_t start = 12 * bit;
    for (const unsigned code : codes) {
        check_tone(pcm, start, bit, false);
        for (unsigned data_bit = 0; data_bit < 5; ++data_bit)
            check_tone(pcm, start + (data_bit + 1) * bit, bit, (code >> data_bit) & 1);
        check_tone(pcm, start + 6 * bit, 3 * bit / 2, true);
        start += frame;
    }
    check_tone(pcm, start, 3 * bit, true);
    require(*std::max_element(pcm.begin(), pcm.end()) == 24576, "Incorrect nominal TX amplitude");
    const auto half = encode("A1 B", 240, 50);
    require(half.size() == pcm.size() && *std::max_element(half.begin(), half.end()) == 12288,
            "Incorrect reduced TX amplitude");
}

void test_live_encoder() {
    Encoder encoder;
    std::string text;
    Decoder decoder([&](char c) { text += c; });
    const auto read = [&](std::size_t samples) {
        Pcm pcm(samples);
        require(encoder.read(pcm.data(), pcm.size()) == pcm.size(), "Live encoder reached EOF");
        feed(decoder, pcm);
        return pcm;
    };

    constexpr unsigned leader = 12 * bit;
    const auto idle = read(leader + 3 * frame + bit / 2);
    check_tone(idle, 0, leader, true);
    for (unsigned i = 0; i < 3; ++i) {
        const auto start = leader + i * frame;
        check_tone(idle, start, bit, false);
        check_tone(idle, start + bit, frame - bit, true);
    }
    require(text.empty(), "Initial LTRS diddles produced text");

    require(encoder.enqueue("1", 2) == "1", "Live encoder rejected initial digit");
    require(!encoder.enqueue("AA", 3), "Live encoder accepted more than its queue limit");
    const auto resumed = read(frame - bit / 2 + 2 * frame);
    check_tone(resumed, 0, bit / 2, false);
    check_tone(resumed, bit / 2, frame - bit, true);
    require(text == "1", "Live encoder failed to finish the diddle before sending text");

    require(encoder.enqueue("2", 1) == "2", "Consecutive digits require only one FIGS shift");
    read(frame + bit / 2); // Send the second digit and start a new LTRS diddle.
    require(!encoder.enqueue("3", 1), "A digit after an LTRS diddle requires FIGS again");
    require(encoder.enqueue("3", 2) == "3", "Live encoder rejected a digit after idle");
    read(4 * frame);
    require(encoder.enqueue(u8" ä", 5) == " AE", "Live encoder failed to normalize input");
    require(encoder.enqueue("\n", 5) == "\n", "Live encoder rejected newline");
    encoder.finish();
    Pcm tail(8 * frame);
    const auto samples = encoder.read(tail.data(), tail.size());
    require(samples > 0 && samples < tail.size(), "Finished live encoder did not reach EOF");
    tail.resize(samples);
    feed(decoder, tail);
    require(text == "123 AE\r\n", "Finish did not transmit all queued characters");
    require(encoder.read(tail.data(), tail.size()) == 0, "Finished live encoder resumed sending");
}

struct Signal {
    Pcm pcm;
    double phase = 0;
    bool reverse = false;
    double offset = 0;

    void tone(bool mark, unsigned samples) {
        const double hz = (mark != reverse ? 1000 : 830) + offset;
        const double step = 2 * pi * hz / 12000;
        for (unsigned n = 0; n < samples; ++n) {
            pcm.push_back(static_cast<std::int16_t>(std::lround(3500 * std::sin(phase))));
            phase = std::remainder(phase + step, 2 * pi);
        }
    }

    void word(unsigned code, bool stop = true) {
        tone(false, bit);
        for (unsigned i = 0; i < 5; ++i) tone((code >> i) & 1, bit);
        tone(stop, 3 * bit / 2);
    }
};

void test_decoder(bool reverse, double offset) {
    Signal signal{{}, 0.7, reverse, offset};
    signal.pcm.resize(731);
    signal.tone(true, 4 * bit);
    // Independent on-air codes for CQ 59 and CR/LF, including a damaged frame.
    for (const unsigned code : {31, 14, 23, 4, 27, 16, 24, 8, 2, 31})
        signal.word(code);
    signal.word(25, false);
    signal.tone(true, 3 * bit);
    signal.word(3);
    signal.tone(true, 2 * bit);
    std::mt19937 random(17);
    std::normal_distribution<double> noise(0, 1300);
    for (auto& sample : signal.pcm) sample += static_cast<std::int16_t>(std::lround(noise(random)));

    std::string text;
    Decoder decoder([&](char c) { text += c; }, {reverse, true});
    feed(decoder, signal.pcm, 137);
    require(text == "CQ 59\r\nA", "RX misdecoded known RTTY words, reverse polarity or damaged frame");
}

void test_reset_and_squelch() {
    std::string text;
    Decoder decoder([&](char c) { text += c; });
    Signal first;
    first.tone(true, 4 * bit);
    first.word(27); // FIGS
    first.tone(false, bit); // Start of an incomplete character.
    first.tone(true, bit);
    feed(decoder, first.pcm);
    require(text.empty(), "An incomplete character was emitted");
    decoder.reset();

    std::mt19937 random(81);
    std::normal_distribution<double> noise(0, 1500);
    Pcm background(5 * 12000);
    for (auto& sample : background) sample = static_cast<std::int16_t>(std::lround(noise(random)));
    feed(decoder, background);
    feed(decoder, Pcm(12000));
    Signal carrier;
    carrier.tone(true, 12000);
    feed(decoder, carrier.pcm);
    require(text.empty(), "Noise, silence or a steady Mark produced text");

    Signal next;
    next.tone(true, 4 * bit);
    next.word(3); // A in letters, '-' in figures.
    next.tone(true, 2 * bit);
    feed(decoder, next.pcm);
    require(text == "A", "Reset did not restore letter shift");
}

void test_unshift_on_space() {
    Signal signal;
    signal.tone(true, 4 * bit);
    for (const unsigned code : {27, 23, 4, 23})
        signal.word(code);
    signal.tone(true, 2 * bit);
    std::string text;
    Decoder decoder([&](char c) { text += c; }, {false, false});
    feed(decoder, signal.pcm, 1);
    require(text == "1 1", "FIGS must persist through spaces with USOS off");
    text.clear();
    Decoder usos([&](char c) { text += c; });
    feed(usos, signal.pcm);
    require(text == "1 Q", "Space must restore LTRS with USOS on");
}
}

int main() {
    try {
        test_encoder();
        test_live_encoder();
        test_decoder(false, 22);
        test_decoder(true, -22);
        test_reset_and_squelch();
        test_unshift_on_space();
        const auto pcm = encode("CQ CQ DE DJ0ABR\n599 73!");
        for (const bool usos : {false, true}) {
            std::string text;
            Decoder decoder([&](char c) { text += c; }, {false, usos});
            feed(decoder, pcm);
            require(text == "CQ CQ DE DJ0ABR\r\n599 73!", "TX/RX round trip failed");
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
