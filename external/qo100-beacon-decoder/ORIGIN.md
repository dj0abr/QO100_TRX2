# QO-100 beacon reference (GPL-3.0)

Source: https://github.com/ta2ldv/qo100-beacon-decoder
Revision: e5aeb3e47d0dc169f00d774f5de60cf4a11a5b80
Retrieved 2026-11-07. README.md explicitly licenses the repository GPL-3.0;
COPYING contains the full GPL v3 text. Attribution: Levent, TA2LDV;
reference informed by Daniel Estevez/gr-satellites and Phil Karn/libfec.
No upstream copyright notices have been removed.

The following upstream files are copied **unchanged**:

| File | SHA-256 |
|---|---|
| ao40_fec.py | 4861e38ba01d158aa60126004bc65b0f6d33bc4f59411a0f4418b08ab87569e8 |
| qo100_beacon_decoder.py | 1bd13e01ac68abeebd597f5dc0b159f0f70fbfd30ec448f97316a7d1aeac3ec4 |
| sample.wav | 8788f0385db5ca201e8e113a6f5bb0acb1fb2fe763f9c96ddf7533e6a6a79b6f |
| README.md | 3fe63d8e2f0ed20ae2e3d2a5fd314c0ecdbfb4a338e2b0dc38c88146d5e90b8c |

`decoder/beacon_dsp.cpp`, `beacon_fec.cpp` and `beacon_main.cpp` implement the
production RX4 receiver in C++ (GPL-3.0-only), adapting the framing/FEC parameters
and algorithms from this pinned reference. The fast path uses FIR decimation to
4 kHz, squared-carrier acquisition/tracking, matched chip sampling, native
Viterbi and Reed-Solomon with fixed tables. Low audio carriers retain an analytic
input fallback for image rejection. UDP validation, bounded latest-window jobs,
overlap suppression and best-effort CPU budgeting are QO100_TRX2 adaptations.
The unmodified upstream files above remain the independent comparison reference.
`decoder/beacon_decoder.py` retains the previous Python DSP for regression tests;
its command-line entry delegates to the native executable. The production PSK
receiver does not import NumPy/SciPy; those packages remain used by fixtures and
other project components. Tests compare every CRC/FEC payload of the recording
and use independent gr-satellites Viterbi/RS vectors.
The WAV is a historical recording, not current beacon announcements. Its RIFF
size is eight bytes too large; SciPy reads all valid audio with a known warning.
