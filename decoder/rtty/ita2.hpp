#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace qo100::rtty::detail {

inline constexpr double pi = 3.14159265358979323846;
// 45.4545… Bd (22 ms/bit) gives exact sample counts at 12 kHz.
inline constexpr unsigned bit_samples = 264;
inline constexpr unsigned data_bits = 5;
inline constexpr unsigned start_and_data_samples = (1 + data_bits) * bit_samples;
inline constexpr unsigned frame_samples = start_and_data_samples + 3 * bit_samples / 2;

inline constexpr std::uint8_t figures_shift = 27;
inline constexpr std::uint8_t letters_shift = 31;
inline constexpr std::uint8_t space = 4;

inline constexpr std::array<char, 32> letters{
    '\0', 'E', '\n', 'A', ' ', 'S', 'I', 'U',
    '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
    'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q',
    'O', 'B', 'G', '\0', 'M', 'X', 'V', '\0'
};

// US figures assignments, as used by fldigi's amateur RTTY mode.
inline constexpr std::array<char, 32> figures{
    '\0', '3', '\n', '-', ' ', '\a', '8', '7',
    '\r', '$', '4', '\'', ',', '!', ':', '(',
    '5', '"', ')', '2', '#', '6', '0', '1',
    '9', '?', '&', '\0', '.', '/', ';', '\0'
};

inline constexpr std::string_view letters_text{letters.data(), letters.size()};
inline constexpr std::string_view figures_text{figures.data(), figures.size()};

}
