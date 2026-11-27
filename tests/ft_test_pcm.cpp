#include <libft8/libft8.hpp>

#include <algorithm>
#include <bit>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) try
{
    if (argc < 4) throw std::invalid_argument("Usage: ft-test-pcm decode|encode FT4|FT8 FILE [MESSAGE]");
    const std::string_view operation = argv[1], name = argv[2];
    if (name != "FT4" && name != "FT8") throw std::invalid_argument("Invalid mode");
    const auto mode = name == "FT4" ? ft8::Mode::FT4 : ft8::Mode::FT8;
    const auto slot_samples = ft8::slot_duration(mode).count() * ft8::sample_rate / 1000;
    if (operation == "encode" && argc == 5) {
        const auto wave = ft8::encode(mode, argv[4]);
        std::vector<ft8::Sample> slot(slot_samples);
        const auto offset = mode == ft8::Mode::FT4 ? 3600 : 6000;
        std::copy(wave.samples.begin(), wave.samples.end(), slot.begin() + offset);
        std::ofstream out(argv[3], std::ios::binary);
        for (const auto sample : slot) {
            const auto bits = std::bit_cast<std::uint16_t>(sample);
            out.put(char(bits & 255)).put(char(bits >> 8));
        }
        if (!out) throw std::runtime_error("Cannot write PCM");
    } else if (operation == "decode" && argc == 4) {
        std::ifstream in(argv[3], std::ios::binary);
        if (!in) throw std::runtime_error("Cannot read PCM");
        std::vector<ft8::Sample> pcm;
        for (int lo = in.get(); lo != EOF; lo = in.get()) {
            const int hi = in.get();
            if (hi == EOF) throw std::runtime_error("Truncated PCM");
            pcm.push_back(std::bit_cast<ft8::Sample>(std::uint16_t(lo | hi << 8)));
        }
        for (const auto& row : ft8::decode(mode, pcm)) std::cout << row.message.text << '\n';
    } else {
        throw std::invalid_argument("Expected decode or encode with a message");
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
