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
| 5 | 1 | Type: 1 SUBSCRIBE, 2 SPECTRUM, 3 TUNE, 4 STATE, 5 AUDIO |
| 6 | 2 | Header size, 64 |
| 8 | 4 | Stream ID: 0 = full-band RX spectrum; 1 = RX channel 1 |
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
