# qo100-beacon-decoder

🇬🇧 English | **[🇹🇷 Türkçe](README.tr.md)**

**A pure-Python decoder for the QO-100 satellite PSK beacon that teaches every
layer of the stack along the way.**

Feed it a `.wav` file recorded from any WebSDR and it prints the text bulletins
transmitted by the satellite, the FEC-protected encoded copies of those
bulletins, and a complete timeline of the transmission. No GNU Radio, no special
hardware — just `numpy`, `scipy` and curiosity.

```
Audio (WAV) ──► BPSK demodulation ──► biphase decoding ──► deframing ──► TEXT
                                                      └──► FEC decode (Viterbi+RS) ──► TEXT
```

---

## Contents

1. [What is QO-100?](#1-what-is-qo-100)
2. [What is the beacon, what does it transmit?](#2-what-is-the-beacon)
3. [How to use (step by step)](#3-how-to-use)
4. [Understanding the outputs](#4-outputs)
5. [Signal layer: 400 bps BPSK and biphase coding](#5-signal-layer)
6. [Framing layer: the AMSAT P3-D format](#6-framing-layer-p3-d-format)
7. [FEC layer: the AO-40 error correction chain](#7-fec-layer-ao-40)
8. [Transmission pattern: one plain, one coded](#8-transmission-pattern)
9. [Files and verification](#9-files-and-verification)
10. [References](#10-references)

---

## 1. What is QO-100?

**QO-100 (Qatar-OSCAR 100)** is an amateur radio transponder carried aboard the
commercial telecommunications satellite Es'hail-2. Launched in 2018, it is a
first: **the world's first geostationary amateur radio satellite.**

Geostationary means the satellite orbits 35,786 km above the equator at exactly
the speed of Earth's rotation — so it appears fixed in the sky (at 25.9° East).
Unlike low-orbit satellites there are no "pass times" to wait for; point a dish
at it once and it is there 24/7. Its footprint stretches from Brazil to Thailand.

It carries two transponders:

| Transponder | Uplink | Downlink | Use |
|---|---|---|---|
| Narrow Band (NB) | 2400.050–2400.300 MHz | 10489.550–10489.800 MHz | SSB, CW, FT8, SSTV, **beacon** |
| Wide Band (WB) | 2401.5–2409.5 MHz | 10491–10499 MHz | DATV (digital TV) |

This project deals with the **PSK beacon** at the very centre of the NB
transponder.

## 2. What is the beacon?

The beacon is a signal transmitted 24/7 by the satellite operators (AMSAT-DL +
Qatar ARS, station callsign DK0SB). Its jobs: provide a frequency reference,
show users the power limit ("keep your signal below the beacon"), and carry
**news bulletins**.

- **Frequency:** 10489.750 MHz (downlink)
- **Modulation:** 400 bps BPSK, biphase (Manchester) coded
- **Content:** message blocks of type K, L, M, N — emergency frequency
  announcement, band plan news, general announcements. Every block is also
  repeated in an FEC-protected copy (see [section 7](#7-fec-layer-ao-40)).

On a waterfall display the beacon shows up as a characteristic **twin-humped**
trace centred on 10489.750 (the twin humps are the signature of biphase coding —
see section 5).

## 3. How to use

### 3.1 Requirements

```bash
pip3 install numpy scipy matplotlib
```

Two files must sit in the same folder: `qo100_beacon_decoder.py` (main script)
and `ao40_fec.py` (FEC decoding module).

### 3.2 Recording (via WebSDR — no antenna needed!)

If you don't own a dish, use a public WebSDR receiver. Tested with:
**<http://websdr.is0grb.it:8901/>** (IS0GRB, Sardinia).

Open the page and configure:

| Setting | Value | Why |
|---|---|---|
| Frequency | **10489748.50 kHz** | Puts the beacon (10489.750) at exactly 1500 Hz in the audio band |
| Mode | **USB2.7** | Single sideband; the 2.75 kHz filter passes the whole signal |
| DSP Noise Reduction | **Disabled** | Noise reduction distorts phase; BPSK lives in the phase |
| Autonotch | **Off** | Would mistake the beacon for an interfering carrier and null it |
| Squelch / Mute / High Boost | **Off** | Nothing should gate or shape the signal |
| Audio AGC | Auto | Works fine |
| Volume | High (no clipping) | For recording SNR |

A screenshot of the correct configuration:

![WebSDR settings](misc/settings_websdr.png)

Verify on the waterfall that the beacon's twin-humped trace sits **inside** the
yellow filter shape. Then:

1. Press **"Audio recording: start"** on the page
2. Wait **at least 60–90 seconds** (a plain block takes 10.4 s, an FEC frame
   13 s; you need a full cycle — 3 minutes is ideal)
3. **stop** → **download** the `.wav` file

### 3.3 Running

```bash
python3 qo100_beacon_decoder.py recording.wav
```

That's it. The script finds the carrier, demodulates, deframes, decodes the FEC
and prints the dump to the console. It also writes three files under `data/`:
the text dump, a block heat map, and a **spectrum analysis image** of your
recording (see sections 4 and 5).

### 3.4 Example output

```
carrier: 1506.14 Hz
chips: 120921, eye quality: 4.73 (>2 good)

6 plain blocks (6 CRC OK), 5 FEC frames (5 decoded)

==============================================================================
RECORDING DUMP - chronological  (t = seconds from start, 400 bps, 151.1s)
==============================================================================
 #    t_beg    t_end  kind tip   size  UTC   verification
------------------------------------------------------------------------------
 0     1.8s    12.2s  DUZ  M     512B  N/A   CRC OK
 1    13.2s    26.2s  FEC  M     256B  N/A   RS OK (0 corrections)  <- first half of previous M block (identical)
 2    27.2s    37.6s  DUZ  N     512B  N/A   CRC OK
 ...
10   128.8s   139.2s  DUZ  N     512B  N/A   CRC OK
    [!] t=139.2s-151.1s (12.0s) unreadable - partial/corrupt block or recording boundary
```

For a complete real dump see
[`data/sample_00_dokum.txt`](data/sample_00_dokum.txt).

And the block contents:

```
==============================================================================
[2] t=27.2s  DUZ  tip='N'  UTC: N/A  CRC OK
------------------------------------------------------------------------------
  N HI de Qatar-OSCAR 100 (DK0SB)
  In order to coordinate potential emergency communications
  during the actual or any other crisis, the following frequency
  will be assigned as international emergency frequency on QO-100
  NB Transponder: Downlink: 10489.860 MHz  Uplink: 2400.360 MHz
  ...
```

## 4. Outputs

Each run produces two files under `data/`, named after the input file (a run
counter prevents overwriting):

| File | Content |
|---|---|
| `data/<name>_00_dokum.txt` | Same chronological dump as the console |
| `data/<name>_00_block.png` | Heat map: one row per block, colour = byte value (0–255). Constant fields show up as vertical stripes |
| `data/<name>_00_spectrum.png` | Power spectrum + spectrogram of the recording (the twin-hump signature) |

Heat-map example (from a real recording — 6 plain text blocks on top with their
striped patterns, decoded FEC blocks below):

![Block heat map](data/sample_00_block.png)

Two health indicators on the console:

- **eye quality**: cleanliness of the demodulator output. >2 is good,
  >4 excellent. Low values mean weak signal or wrong settings.
- **CRC OK / RS OK**: mathematical proof that the decoded data is error-free.

## 5. Signal layer

### What is BPSK?

One of the simplest ways to put digital data on a radio carrier:
**B**inary **P**hase **S**hift **K**eying. The *phase* of the carrier is
flipped 180° according to the bit value. The receiver's job is to catch those
phase flips.

### What is biphase (Manchester) coding, and why?

The beacon does not send bits directly — it **biphase-codes** them: each data
bit becomes two "chips", and the information lives in the **transitions**, not
the chip values. 400 bps of data → 800 chips/s on air.

The reason is practical: a long run of unchanging bits (e.g. 50 padding bytes
of 0x50) would mean a long stretch of constant phase in plain BPSK — and the
receiver's clock would drift. Biphase guarantees at least one transition per
bit, refreshing the receiver's timing continuously. The side effect is visible
in the spectrum: energy piles up in **two humps** either side of the carrier,
with a null right at the centre. (That's the secret of the twin trace on the
waterfall.)

### The spectrum image the decoder produces

Every run generates `data/<name>_NN_spectrum.png` from your own recording —
the same analysis shown below (a real example: [`data/sample_00_spectrum.png`](data/sample_00_spectrum.png)):

![Spectrum](data/sample_00_spectrum.png)

**How to read it.** The top panel is the power spectrum (Welch method):

- The **two humps** either side of 1500 Hz are the biphase BPSK signature —
  energy concentrated around ±350 Hz of the carrier.
- The **notch at exactly 1500 Hz** is the suppressed carrier: BPSK carries no
  power at the carrier frequency itself.
- The signal occupies roughly **1000–2500 Hz**; beyond that the flat noise
  floor and the receiver filter roll-off (~2900 Hz) are visible.

The bottom panel is the spectrogram (frequency over time). The texture changes
mark the block structure: dense striped intervals are data blocks, and the
transition between an uncoded block and an FEC frame is visible as a subtle
change in pattern.

**Using it for troubleshooting:** if the humps are not centred on 1500 Hz, your
dial frequency was off — retune to 10489748.50 kHz. If a hump is cut off at the
edge, your filter was too narrow — use USB2.7 or wider. If the humps barely
rise above the noise floor, the signal was weak (expect a low eye-quality
number and failed CRCs).

### The demodulation chain in this project

```
WAV ─► Hilbert transform (real signal → complex/analytic signal)
    ─► coarse carrier estimate (spectral centroid of the 1000–2500 Hz band)
    ─► fine tuning: square the signal → the 180° flips vanish, leaving one
       clean tone at 2× the carrier (the classic "squaring loop")
    ─► phase tracking (derotate by half the phase of the squared signal)
    ─► 800 chips/s sampling (best instant found by energy maximisation)
    ─► biphase decode: XOR chip pairs → 400 bps bit stream
```

## 6. Framing layer: P3-D format

We have bits; where does a message start? Enter the **AMSAT P3-D telemetry
format** (inherited from the AO-10/13/40 satellites; official spec:
[tlmspec.txt](https://amsat-dl.org/wp-content/uploads/2019/01/tlmspec.txt)).

Every frame looks like this:

```
┌──────────────────┬────────────────┬───────────────┬─────────┐
│ ~130 bytes 0x50  │ SYNC (4 bytes) │ DATA          │ CRC     │
│ 'P' padding      │ 39 15 ED 30    │ 512 bytes     │ 2 bytes │
└──────────────────┴────────────────┴───────────────┴─────────┘
```

- **Padding (0x50 = ASCII 'P'):** inter-frame gap. Shows up as `PPPPPP...`
  runs in raw dumps.
- **Sync word:** a fixed 32-bit pattern meaning "a frame starts here". The
  decoder searches for it at bit level by correlation — no byte alignment
  needed.
- **CRC:** the AMSAT "CYC2" polynomial `x¹⁶+x¹²+x⁵+1` (CRC-CCITT family),
  initial value 0xFFFF, MSB-first. Nice property: computing the CRC over the
  514 bytes *including* a correct CRC yields 0 — verification is one line.
- **Block type = first byte of the data:**

| Type | Meaning |
|---|---|
| `A` | Telemetry: UTC date/time in header + 128 analog + 128 digital channels |
| `E` | Historical event records (A format) |
| `K` `L` `M` `N` | Message blocks — free-text bulletins (this is what QO-100 sends) |
| `D` | File transfer packet |
| `X` | Software upload block |

> Note: the QO-100 beacon currently transmits only K/L/M/N message blocks — no
> A-type telemetry. The decoder is ready anyway: if an A block ever appears,
> its UTC header field is parsed automatically. Until then the dump shows
> `UTC: N/A`.

## 7. FEC layer: AO-40

### What is FEC and why does it exist?

**FEC = Forward Error Correction.** Radio channels are noisy; bits get
corrupted. There are two strategies: detect the error and *ask for a resend*
(that's what TCP does on the internet), or add mathematical redundancy up front
so the receiver can **fix errors on its own**. A satellite beacon has no return
channel — it cannot ask anyone to retransmit. Hence FEC.

### What is AO-40?

AO-40 (AMSAT-OSCAR 40, 2000–2004) was the most ambitious amateur satellite of
its era. For its telemetry, Phil Karn (KA9Q) adapted the CCSDS coding
architecture NASA uses on deep-space probes. The satellite died 20 years ago,
but the FEC design lives on: the QO-100 beacon and the FUNcube satellites
(AO-73, JY1SAT, Nayif-1) still use exactly this format today.

### The encoding chain (transmit side)

256 bytes of text expand to 5200 bits (~2.5×) through five stages:

```
256 bytes of text
  │ 1. Split in two: 2 × 128 bytes
  ▼
Reed-Solomon RS(160,128): 32 parity bytes per half → 2 × 160 = 320 bytes
  │    GF(2⁸), field polynomial 0x187, fcr=112, prim=11 (CCSDS family)
  │    Payoff: can CORRECT up to 16 byte errors per codeword
  ▼
Scrambler (CCSDS randomizer, x⁸+x⁷+x⁵+x³+1, seed 0xFF)
  │    Turns data into statistical noise → prevents long 0/1 runs
  ▼
Convolutional code (K=7, rate 1/2, CCSDS polynomials, 2nd output inverted)
  │    Every bit becomes 2: neighbouring bits get mathematically entangled.
  │    The receiver's Viterbi algorithm recovers the most likely original.
  ▼
Interleaver: 5200 bits written row-wise into an 80-column × 65-row matrix,
  │    read out column-wise → burst errors get scattered into isolated
  │    single errors that RS can fix
  ▼
Distributed sync: a 65-bit sync pattern spread over every 80th bit position
  │    (a contiguous sync dies in one burst; a distributed one survives)
  ▼
5200 bits = 13.0 seconds on air at 400 bps
```

The decoder (`ao40_fec.py`) walks this chain backwards:
**distributed sync search → deinterleave → Viterbi → descramble → RS correction.**

Parameter summary:

| Parameter | Value |
|---|---|
| FEC frame length | 5200 bits (13.0 s) |
| Sync pattern | 65 bits, distributed at every 80th position |
| Interleaver | 80 columns × 65 rows |
| Convolutional code | K=7, r=1/2, 2nd output inverted (CCSDS) |
| Reed-Solomon | RS(160,128) × 2 interleaved, GF(2⁸) 0x187, fcr=112, prim=11 |
| Net payload | 256 bytes = 4 lines × 64 characters of text |

## 8. Transmission pattern

The beacon alternates between the two formats continuously:

```
time ───────────────────────────────────────────────────────────────►
[PLAIN M 10.4s] [FEC M 13s] [PLAIN N 10.4s] [FEC N 13s] [PLAIN K] [FEC K] ...
    512 chars      256 chars
    CRC only       Viterbi+RS protected
```

Each FEC frame is a **protected copy of the first 256 characters of the plain
block right before it** — the decoder compares them byte by byte and reports it
in the dump (`<- first half of previous M block (identical)`). Why transmit
twice? Strong stations read the plain block easily; weak stations with small
dishes can still decode the FEC copy error-free. Same content, two protection
levels.

## 9. Files and verification

| File | Role |
|---|---|
| `qo100_beacon_decoder.py` | DSP front end + P3-D deframer + reporting |
| `ao40_fec.py` | AO-40 FEC decode chain (sync/deinterleave/Viterbi/descramble/RS) |
| `data/` | Example outputs from a real recording + outputs of your own runs |
| `data/` | Outputs of your runs, named after the input file + run counter |

Every stage of the FEC chain has been verified bit-for-bit against the
**official gr-satellites test vectors** (a real AO-73/FUNcube packet):
sync detection ✓, deinterleave ✓, Viterbi output identical to reference ✓,
RS output identical to reference frame ✓.

The implementation is a pure Python/NumPy rewrite informed by gr-satellites
(Daniel Estévez, GPL-3.0) and libfec (Phil Karn, KA9Q); the repository is
therefore licensed GPL-3.0.

## 10. References

- AMSAT P3-D Telemetry Specification (Release 1.8, 2001):
  <https://amsat-dl.org/wp-content/uploads/2019/01/tlmspec.txt>
- G3RUH — Oscar-40 FEC Telemetry (narrative of the FEC design):
  <https://amsat.org/articles/g3ruh/125.html>
- Phil Karn KA9Q — A Proposal for a Coded AO-40 Telemetry Format:
  <http://www.ka9q.net/papers/ao40tlm.html>
- Daniel Estévez — QO-100 beacon analyses:
  <https://destevez.net/2019/02/decoding-the-qo-100-beacon-with-gr-satellites/>
  <https://destevez.net/2019/04/qo-100-beacon-fec-decoder/>
- gr-satellites (reference implementation and test vectors):
  <https://github.com/daniestevez/gr-satellites>
- AMSAT-DL QO-100 pages: <https://amsat-dl.org/en/>
- Tested WebSDR (IS0GRB): <http://websdr.is0grb.it:8901/>
