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

`decoder/beacon_decoder.py` adapts the upstream DSP for live RX2 input, carrier
search relative to the RX dial, sample-clock estimation, bounded overlapping
windows, UDP validation and browser text output. This adaptation is GPL-3.0.
The upstream frame/CRC/FEC functions are imported directly, without modification.
Only NumPy and SciPy are required at runtime; no matplotlib or GNU Radio.
The WAV is a historical recording, not current beacon announcements. Its RIFF
size is eight bytes too large; SciPy reads all valid audio with a known warning.
