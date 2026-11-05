# DSP migration to bundled liquid-dsp 1.8.3 (v0.5)

The UDP protocols, sample rates, independent RX1/RX2 state, software PTT,
clock-drift controller, FIFO limits, timeout silence and 0 dB configured TX
attenuation are retained.

## Processing blocks

- RX frequency translation: `nco_crcf`, `LIQUID_VCO_INTERP`, positive oscillator
  frequency plus explicit mixing direction. This avoids coarse NCO lookup
  quantization and loss of precision when wrapping small negative frequencies.
- RX 600 kS/s -> 75 kS/s: `firdecim_crcf`, factor 8, existing 321 Kaiser taps.
- RX USB filter: `firfilt_cccf`, existing 513 complex taps selecting +100..4000 Hz.
- TX USB filter: same liquid FIR with factor-two analytic-signal normalization.
- TX 12 kS/s -> 1.2 MS/s: `firinterp_crcf`, exact factor 100, Kaiser prototype,
  semi-length 16, design stop-band attenuation 100 dB. Always 24000 IQs per
  240 audio samples, including after reset.
- TX frequency translation: interpolated liquid oscillator, block processing.
- FFT and calibrated dBFS normalization remain FFTW-based.
- RX 75 -> 12 kS/s and adaptive TX clock correction remain libsamplerate-based.
  The default soundcard capture/playback code is unchanged.

The Kaiser passbands and coefficients of the existing channel/USB filters are
preserved, with shared coefficient construction. Liquid objects own their state
through RAII, including partial-construction error cleanup. The RX decimator
uses liquid's first-sample phase within each eight-sample group. Compared with
v0.4's last-sample phase this adds 7/600000 s (11.67 us) of relative delay while
retaining the same magnitude response and output sample count.

The generic `msresamp_crcf` was evaluated for TX. Its default finite filter-bank
phase resolution produced a roughly -70 dBc spur in the 1 kHz test, despite a
90 dB filter design setting. Exact integer polyphase interpolation avoids this
fractional-phase artifact. Filter design attenuation alone is not a bound on
all system spurs; the tests measure the complete modulated output.

## Regression checks

The existing seven tests cover RX sideband/alias rejection, independent retuning,
12 kHz PCM, TX FIFO/clock correction, +/-1500 ppm over five simulated minutes,
packet gaps, stale controls, audio absence, PTT release and lease expiration.
Two additional CTest cases cover:

1. `liquid_dsp_quality`: linked/header version match; positive/negative mixer
   offsets at both sample rates; sample vs block equivalence; frequency error
   <0.05 Hz; oscillator residual < -106 dBc after removing frequency offset;
   USB passband/edge/stopband response; full TX spectrum at 200, 1000, 3000 and
   3900 Hz with carrier offset -273457 Hz. Distant spurs must stay below -90 dBc.
   These are numerical synthetic tests, not measurements of RF hardware noise.
2. `bundled_liquid`: SHA-256 integrity, shared-library version, and absence of a
   dynamic liquid dependency in both executables.

The binary bundle has no FFTW dependency and contains no implementation sources.
See [bundle provenance and portability](../external/liquid-dsp-1.8.3/README.md).

The migrated backend also passed a QO-100 transmit/receive listening test
with the configured 0 dB TX attenuation.

## Measured DSP cost

On Intel Core Ultra 9 185H, GCC 13.3, -O2, median of seven runs, using the same
sample buffers and active USB audio for both v0.4 and v0.5:

| Processing per signal second | v0.4 | liquid v0.5 |
|---|---:|---:|
| Two RX channels | 95.5 ms CPU | 59.5 ms CPU |
| One TX channel | 99.5 ms CPU | 19.0 ms CPU |

These are CPU times of the DSP paths, excluding network, GUI, driver and FFT.
They do not predict total application CPU utilization on another machine.
To measure the current version without touching hardware:

```sh
cmake --build build --target benchmark_dsp
./build/bin/benchmark_dsp
```
