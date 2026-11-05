# QTR2 UDP protocol, version 1

One GUI-facing UDP socket: default backend `0.0.0.0:40900`. The GUI sends a
SUBSCRIBE datagram from its receive socket and repeats it every 2 seconds.
The backend sends SPECTRUM fragments to that datagram's source IP and port.
Only one current subscriber; subscription expiry defaults to 8 seconds.

All integers are big endian. No native C/C++ structs are sent.
The header is exactly 64 bytes. Maximum datagram size: 1200 bytes.
Python reference format: `struct.Struct("!4sBBHIIQQQIIIHHII")`.

| Offset | Width | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `QTR2` |
| 4 | 1 | Protocol version, 1 |
| 5 | 1 | Type: 1 SUBSCRIBE, 2 SPECTRUM, 3 TUNE, 4 STATE, 5 AUDIO, 6 TX_CONTROL, 7 TX_STATE, 8 TX_AUDIO, 9 FILTER_CONTROL, 10 FILTER_STATE, 11 COMPRESSOR_CONTROL, 12 COMPRESSOR_STATE |
| 6 | 2 | Header size, 64 |
| 8 | 4 | Stream ID: 0 = full-band spectrum; 1 = RX1; 2 = RX2; 3 = TX |
| 12 | 4 | Frame ID / control request ID / audio tuning epoch, uint32 |
| 16 | 8 | Backend Unix timestamp in nanoseconds at frame emission |
| 24 | 8 | FFT exclusive end index / audio start index / STATE tuning epoch |
| 32 | 8 | Display RF center frequency in Hz, unsigned 64 bit |
| 40 | 4 | Complex sample rate in samples/s |
| 44 | 4 | FFT size |
| 48 | 4 | First bin index in this datagram |
| 52 | 2 | Number of bins in this datagram |
| 54 | 2 | Format: 1 = 0.01 dBFS; 2 = PCM16; STATE: status 0 OK, 1 out of range |
| 56 | 4 | Total bins in the frame, equal to FFT size |
| 60 | 4 | Backend session ID, newly generated for each backend run |
| 64 | 2 × count | Signed big-endian spectral levels |

SUBSCRIBE has type 1, length 64, and every field after offset 7 is zero.
SPECTRUM contains at most 568 bins per datagram. All its fragments carry the
same frame metadata; only first-bin index, count and payload differ. For 8192
bins this is 15 datagrams per spectrum line. The frequency of bin `k` is
`center_hz + (k - fft_size/2) * sample_rate / fft_size`.

Levels are tone-normalized dBFS per FFT bin, clamped to -160..+20 dBFS, not PSD
or calibrated dBm. A bin-centered complex tone with amplitude 0.5 measures
approximately -6.02 dBFS. Hann-window coherent gain is included in normalization.
Power averaging precedes conversion to dB.

The GUI assembles fragments using `(session_id, frame_id)`, checks consistent
metadata, tolerates reordered and repeated fragments, and discards incomplete
frames. No retransmission of display data. The reference client keeps at most
4 unfinished frames for at most 0.5 seconds. A new session permits backend
restart while the GUI remains open. The current client uses timestamps to
discard late frames, assuming the backend wall clock does not jump backwards.

Sample counters reflect **received samples**, not hardware absolute time.
The existing raw driver protocol has no sequence field, so loss before the
backend cannot be counted exactly. Runtime receive-rate diagnostics expose
sustained rate deficits. GUI-fragment loss is visible separately.

## Receiver control and audio (v0.2)

Spectrum stream 0 is unchanged. Stream 1 is the zoom: center is the confirmed
USB carrier, sample rate 75000, FFT size 8192. The GUI displays only +/-25 kHz
of these bins. Same fragmentation and dBFS encoding as the wide spectrum.

TUNE: a 64-byte header, type 3, stream 1. Frame ID holds the request sequence;
center_hz is the requested absolute downlink frequency in Hz. Other fields are
zero. Only the current subscriber may tune. The GUI repeats its newest request
at subscription renewal until acknowledged. Older request IDs are ignored.

STATE: 64 bytes, type 4, stream 1, echoed request ID, center_hz = actual tuning.
The sample-index field holds the current audio epoch, format/status = 0 for OK
or 1 if the requested zoom leaves the capture window. Session ID identifies the
backend run. The epoch increments only when the frequency actually changes.
Repeated requests do not reset filters or audio. Rejected commands retain the
previous frequency. UI clients should display confirmed backend state.

AUDIO: type 5, stream 1, frame ID = tuning epoch, format = 2. Sample rate 12000,
fft_size and first_bin = 0. Bin-count and total-count fields both hold the number
of mono PCM samples. Sample index is the **start** of this packet in the epoch.
Payload is signed PCM16 big endian, not dB data; 32768 represents unit amplitude.
The normal count is 240: 20 ms, 480 PCM bytes, 544 bytes including header.
Audio is not fragmented. The GUI accepts only the session/epoch acknowledged
by STATE, drops duplicate/late packets, buffers reordering and fills missing
sample intervals with silence. Playback gain is local to the GUI. Audio is
transported over the same GUI socket; no audio-specific UDP port is added.

Audio counters restart at zero on retune. The new epoch flushes playback
history and the clock resampler. The GUI's bounded jitter buffer and slow
sample-rate correction accommodate network jitter and different soundcard
and Pluto clocks. No retransmission of live audio.

## Two simultaneous USB receivers (v0.3)

Stream 2 adds independent RX2 control and audio. TUNE, STATE and AUDIO retain
their existing v1 layouts, with stream_id=2. Stream 1 is unchanged. Only RX1
produces a zoom spectrum in this version; there is no stream-2 SPECTRUM.

A client subscribes once, then sends/retries its latest TUNE for each channel.
Each channel owns its request sequence, confirmed frequency, audio epoch and
sample counter. Clients must match STATE by **both stream ID and request ID**,
and AUDIO by **stream ID, session ID and audio epoch**. Equal request or epoch
numbers on different channels do not imply shared state. Retuning one channel
must not flush, retune or restart the other. Unsupported stream IDs are rejected.

Both AUDIO streams use 12000 Hz mono PCM16, 240 samples/20 ms, and share the
existing GUI UDP socket. RX2 has the same +100..4000 Hz USB selection but no
AGC, compression or digital decoding. In the test GUI only stream 1 is routed
to ALSA default. Stream 2 enters a separate bounded queue and is monitored by
packet/sample counts, RMS and peak level. It is then discarded. Test captures
can store RX2 audio as a separate 12-kHz WAV file.

## USB transmitter (v0.4)

TX shares the subscribed GUI socket and uses stream 3. All controls and audio
are accepted only from the current subscriber. TX_CONTROL additionally carries
the current backend session ID (learned from RX STATE). A backend restart must
reset client PTT to OFF; it requires a new manual PTT press.

TX_CONTROL (6), 64 bytes: frame_id = monotonic uint32 request, center_hz =
absolute **uplink** USB carrier in Hz, format = 0 PTT off / 1 PTT on,
session_id = current backend session. Other fields zero. Valid carriers lie
within tx_center_hz ±275000 Hz. Repeat the latest control at least every 500 ms.
After 1500 ms without a valid control the backend clears PTT. Older request IDs
are ignored; repeated identical commands renew the lease without resetting DSP.
A changed command requires a new request ID. Rejected frequencies retain state.

TX_STATE (7), 64 bytes, echoes request and actual frequency. sample_index =
TX epoch, session_id = backend session, sample_rate = 12000, format = 0 OK /
1 out of range. first_bin = actual PTT (0/1), fft_size = audio FIFO samples
(including resampler buffering), bin_count = estimated driver FIFO blocks,
total_count = signed int32 clock correction in ppm (two's complement).
The backend sends acknowledgments and periodic telemetry every 250 ms. Match
both request and session. Epoch changes on PTT transitions, retunes or forced
mute. Resending a timed-out ON command cannot re-key; increment request after
a new manual PTT press. PTT is a data gate, not a hardware/GPIO command.

TX_AUDIO (8) has the same PCM16 layout as RX AUDIO, with stream=3 and exactly
240 samples at 12000 Hz. frame_id = acknowledged TX epoch, center_hz = confirmed
uplink carrier, session_id = backend session, sample_index = start index
(multiple of 240). The index advances by 240 per captured packet and resets
only when the acknowledged epoch/session changes. Duplicate, old, misaligned,
wrong-session and wrong-epoch audio is discarded. Clients send only after the
matching successful ON acknowledgment. No retransmission. Packet gaps are
silence; after 200 ms without accepted audio the backend clears audio/DSP
history and emits exact zeros until the FIFO has refilled.

## RX1 variable passband (v0.6)

FILTER_CONTROL (9) and FILTER_STATE (10) use the existing subscribed GUI socket.
Both are exactly 64 bytes and support **stream 1 only**. There is no RX2/TX
filter control. Bandwidth and position are audio frequencies relative to the
USB carrier; they do not change RF tuning or the zoom center.

FILTER_CONTROL: frame_id = monotonic uint32 request, session_id = current
backend session learned from RX STATE. The format field selects **0=query** or
**1=set**. For a query, center_hz, sample_rate and fft_size must be zero.
For a set, center_hz holds the **audio passband center in Hz**, sample_rate is
12000, and fft_size holds the **bandwidth in Hz**. Timestamp, sample_index,
first_bin, bin_count and total_count must be zero in both modes.

Allowed bandwidth: 500..3900 Hz, in integer-Hz steps. Both edges
`center_hz ± bandwidth/2` must remain inside 100..4000 Hz. The center is an
integer number of Hz; odd bandwidths have half-Hz edges. Defaults are BW 3900,
center 2050 (full 100..4000 Hz); BW 500, center 700 selects 450..950 Hz.

FILTER_STATE: echoed request and backend session, sample_index = current RX1
audio epoch, center_hz = accepted target audio center, sample_rate = 12000,
fft_size = accepted target bandwidth. Format/status is **0=OK** or
**1=out of range**. Timestamp, first_bin, bin_count and total_count are zero,
as in RX STATE. An invalid set retains the previous target
and returns it with status 1. Malformed packets, other streams, wrong sessions
and older requests are ignored. Duplicate requests acknowledge current state
and the original request status without applying a changed payload.

The filter has its own request sequence, independent of RX/TX tuning. New
subscribers or renewed subscriptions after expiry may start a fresh sequence;
the selected filter itself is retained. Query first to display the current
backend/configuration state. Repeat the latest command until acknowledged
(the test GUI repeats it every second). Match ACKs by request and session.

An ACK describes the accepted target; the DSP crossfades over 20 ms without
resetting RX1 audio indices, epoch, NCO, resampler or zoom. The replacement
513-tap liquid FIR is initialized with the preceding 512 raw 12-kHz complex
samples. Rapid updates coalesce to the latest target after the current fade,
so convergence may take up to about 40 ms plus processing/transport delay.
RX2 and TX retain their fixed nominal 100..4000-Hz passband. Both RX audio
streams continue to use 12000-Hz PCM16 regardless of the selected RX1 width.

## TX1 speech compressor (v0.7)

COMPRESSOR_CONTROL (11) and COMPRESSOR_STATE (12) use **stream 3 (modulator 1)**
on the existing subscribed socket. Both are exactly 64 bytes. They have their
own monotonic uint32 request sequence, independent of PTT, TX tuning and RX
filter controls. A compressor command never asserts PTT or renews its lease.

CONTROL: frame_id = request, session_id = current backend session, format =
**0=query / 1=set**. For a set, sample_rate = 12000 and fft_size = amount
**0..1400**, representing **0.0..140.0 percent** in 0.1-% steps. Zero is a valid
set (speech bypass); a query has both fields zero and format 0. All other
fields, including center_hz, timestamp and sample_index, must be zero.

STATE: echoed request/session, sample_index = current TX epoch, center_hz = 0,
sample_rate = 12000, fft_size = accepted target amount, format/status =
**0=OK / 1=out of range**. first_bin = current speech gain reduction in
**milli-dB**, total_count = current SSB peak-limiter reduction in **milli-dB**
(both nonnegative uint32). Timestamp and bin_count are zero. The two meter
values reflect DSP state at acknowledgment time, rather than held maxima.

Query first to honor the configured startup value (`tx1_compressor_percent`,
default 100). The test GUI exposes 60..140 percent, with a **100% Normal**
button; lower values remain available through config/API for compatibility and
bypass. It repeats its latest query/set every 250 ms for ACKs
and metering. Match request and session. Invalid amounts retain the preceding
target and acknowledge status 1. Malformed packets, other streams, wrong
sessions and stale requests are ignored. A duplicate request returns current
state and its original status without reapplying a modified payload. Subscriber
changes/expired subscriptions reset request sequencing while retaining amount.

An ACK describes the accepted target. The amount slews at 100 percentage points
per 20 ms at 12 kHz (16 ms across the GUI range, up to 28 ms for 0..140%);
TX epoch, audio indices, FIFO and PTT remain unchanged. Retune,
PTT transitions and missing-audio resets clear compressor/limiter history while
retaining amount. Amount 0 bypasses speech gain; the final SSB peak limiter
remains active with the same fixed 5-ms delay at every setting. TX_AUDIO format,
sample rate and packet size are unchanged. No RX stream is processed here.

## Local Pluto direct TX transport

Only loopback: backend -> 127.0.0.1:40808, driver status -> 127.0.0.1:40810.
Generated driver config selects TX_RAW_IQ=1. Legacy 32-bit/interpolated/marker
transport remains available only when that option is absent or zero.

Each 20 ms block contains 24000 complex I16/Q16 little-endian samples at
1200000 samples/s (96000 bytes). Two UDP datagrams each carry a 16-byte header:
ASCII PTXI, uint32 block sequence, uint32 byte offset, uint32 total bytes;
header integers big endian. Each carries 48000 payload bytes. The driver
accepts consecutive offsets of a new sequence, enqueues only complete blocks,
and discards partial, duplicate or old blocks. Loopback transport needs no
network MTU constraint. A lost fragment loses the block rather than mixing IQ.

After every hardware push: 16-byte status, ASCII PTXS, uint32 sequence,
uint32 filled FIFO blocks, uint32 usable FIFO capacity (8), all big endian.
The backend discards stale statuses and fills toward capacity/2. At startup,
audio absence, or empty FIFO, the driver pushes zeros. Hardware push errors
terminate the driver and are reported by backend child monitoring.
