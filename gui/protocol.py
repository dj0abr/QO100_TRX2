"""QTR2 v1 wire format and bounded FFT reassembly, independent of tkinter."""
from dataclasses import dataclass
import struct
import time
import numpy as np

HEADER = struct.Struct("!4sBBHIIQQQIIIHHII")
MAGIC, VERSION, SUBSCRIBE, SPECTRUM = b"QTR2", 1, 1, 2
TUNE, STATE, AUDIO = 3, 4, 5
MAX_DATAGRAM = 1200


def subscription():
    return HEADER.pack(MAGIC, VERSION, SUBSCRIBE, HEADER.size, *([0] * 12))


def tune(request, frequency, stream_id=1):
    if stream_id not in (1, 2):
        raise ValueError("Receiver stream must be 1 or 2")
    return HEADER.pack(MAGIC, VERSION, TUNE, 64, stream_id, request, 0, 0,
                       frequency, 0, 0, 0, 0, 0, 0, 0)


@dataclass
class AudioPacket:
    epoch: int
    sample_index: int
    center_hz: int
    session_id: int
    samples: np.ndarray
    stream_id: int = 1


def audio_packet(packet):
    if len(packet) < 64 or len(packet) > MAX_DATAGRAM:
        return None
    f = HEADER.unpack_from(packet)
    if (f[:4] != (MAGIC, VERSION, AUDIO, 64) or f[4] not in (1, 2) or f[9] != 12000
        or f[10] or f[11] or f[13] != 2 or f[12] != f[14]
        or not 0 < f[12] <= 568 or len(packet) != 64 + 2*f[12]):
        return None
    return AudioPacket(f[5], f[7], f[8], f[15], np.frombuffer(packet, dtype=">i2", offset=64).astype(np.float32)/32768, f[4])


@dataclass
class Frame:
    stream_id: int
    frame_id: int
    timestamp_ns: int
    sample_index: int
    center_hz: int
    sample_rate: int
    fft_size: int
    session_id: int
    db: np.ndarray

    @property
    def frequencies_hz(self):
        return self.center_hz + (np.arange(self.fft_size) - self.fft_size / 2) * self.sample_rate / self.fft_size


class Assembler:
    """Bounded reassembly, tolerant of packet order, loss and driver restarts."""
    def __init__(self, stream_id=0):
        self.stream_id = stream_id
        self.pending = {}
        self.invalid_packets = 0
        self.incomplete_frames = 0
        self.last_timestamp = -1
        self.last_identity = None

    def push(self, packet, now=None):
        now = time.monotonic() if now is None else now
        try:
            if not HEADER.size <= len(packet) <= MAX_DATAGRAM:
                raise ValueError()
            (magic, version, kind, hsize, stream, frame_id, stamp, index, center,
             rate, size, first, count, fmt, total, session) = HEADER.unpack_from(packet)
            if (magic != MAGIC or version != VERSION or kind != SPECTRUM or
                hsize != HEADER.size or stream != self.stream_id or fmt != 1 or
                size != total or size < 1024 or size > 65536 or size & (size - 1) or
                not 0 < rate <= 100000000 or not count or first + count > size or
                len(packet) != HEADER.size + 2 * count):
                raise ValueError()
        except (ValueError, struct.error):
            self.invalid_packets += 1
            return None
        identity = (session, frame_id)
        if identity == self.last_identity or stamp < self.last_timestamp:
            return None
        for key, state in list(self.pending.items()):
            if now - state[0] > 0.5:
                del self.pending[key]
                self.incomplete_frames += 1
        metadata = (stream, frame_id, stamp, index, center, rate, size, session)
        if identity not in self.pending:
            if len(self.pending) >= 4:
                oldest = min(self.pending, key=lambda key: self.pending[key][0])
                del self.pending[oldest]
                self.incomplete_frames += 1
            self.pending[identity] = (now, metadata, np.empty(size, dtype=np.int16), np.zeros(size, dtype=bool))
        _, expected, values, present = self.pending[identity]
        if metadata != expected:
            self.invalid_packets += 1
            return None
        incoming = np.frombuffer(packet, dtype=">i2", offset=HEADER.size)
        overlap = present[first:first + count]
        if np.any(overlap & (values[first:first + count] != incoming)):
            self.invalid_packets += 1
            return None
        values[first:first + count] = incoming
        present[first:first + count] = True
        if not present.all():
            return None
        del self.pending[identity]
        self.last_timestamp, self.last_identity = stamp, identity
        # Discard older incomplete frames after accepting a newer frame.
        for key, state in list(self.pending.items()):
            if state[1][2] <= stamp:
                del self.pending[key]
                self.incomplete_frames += 1
        return Frame(*metadata, values.astype(np.float32) / 100)
