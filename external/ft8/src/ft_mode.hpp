#pragma once

#include <cstddef>

enum class FtMode : unsigned { ft4 = 4, ft8 = 8 };

constexpr const char* ft_mode_name(FtMode mode) { return mode == FtMode::ft4 ? "FT4" : "FT8"; }
constexpr int ft_slot_ms(FtMode mode) { return mode == FtMode::ft4 ? 7500 : 15000; }
constexpr std::size_t ft_slot_samples(FtMode mode) { return ft_slot_ms(mode) * 12; }
// WSJT-X FT4's decode window is NMAX from ft4_params.f90, shorter than the slot.
constexpr std::size_t ft_decode_samples(FtMode mode) { return mode == FtMode::ft4 ? 72576 : ft_slot_samples(mode); }
inline constexpr std::size_t ft8_early_samples = 41 * 3456;
constexpr std::size_t ft_wave_samples(FtMode mode) { return mode == FtMode::ft4 ? 105 * 576 : 79 * 1920; }
