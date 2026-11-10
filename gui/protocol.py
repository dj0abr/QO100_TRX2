"""QTR2 v1 wire format and bounded FFT reassembly, independent of tkinter."""
from dataclasses import dataclass
import struct
import time
import numpy as np

HEADER = struct.Struct("!4sBBHIIQQQIIIHHII")
MAGIC, VERSION, SUBSCRIBE, SPECTRUM = b"QTR2", 1, 1, 2
TUNE, STATE, AUDIO = 3, 4, 5
MAX_DATAGRAM = 1200
SETTINGS_CONTROL, SETTINGS_STATE = 17,18


def settings_control(request, session=0, revision=0, patch=None):
    payload=b'' if patch is None else ''.join(f'{k}={v}\n' for k,v in sorted(patch.items())).encode('ascii')
    if len(payload)>1136 or (patch is not None and not payload): raise ValueError('Invalid settings payload size')
    return HEADER.pack(MAGIC,VERSION,SETTINGS_CONTROL,64,0,request,0,revision if patch is not None else 0,
                       0,0,len(payload),0,len(payload),int(patch is not None),len(payload),session)+payload


def settings_state(packet):
    if not 64<=len(packet)<=MAX_DATAGRAM: return None
    f=HEADER.unpack_from(packet);n=len(packet)-64
    if (f[:4]!=(MAGIC,VERSION,SETTINGS_STATE,64) or f[4] or f[6] or f[8] or f[9]
        or f[10]!=n or f[12]!=n or f[14]!=n or f[13]>6 or f[11]&~7): return None
    try:
        values={}
        for line in packet[64:].decode('utf-8').splitlines():
            key,sep,value=line.partition('=')
            if not sep or not key or key in values: return None
            values[key]=value
        if values.get('settings_schema')!='1': return None
        return f[5],f[15],f[7],f[13],f[11],values
    except UnicodeError: return None


def subscription():
    return HEADER.pack(MAGIC, VERSION, SUBSCRIBE, HEADER.size, *([0] * 12))


def tune(request, frequency, stream_id=1, session=0):
    if stream_id not in (1, 2):
        raise ValueError("Receiver stream must be 1 or 2")
    return HEADER.pack(MAGIC, VERSION, TUNE, 64, stream_id, request, 0, 0,
                       frequency, 0, 0, 0, 0, 0, 0, session)


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
    if (f[:4] != (MAGIC, VERSION, AUDIO, 64) or f[4] not in (1, 2, 4, 5, 6, 8) or f[9] != 12000
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
                size != total or size < 1024 or size > 65536 or
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


TX_CONTROL, TX_STATE, TX_AUDIO = 6, 7, 8


def tx_control(request, frequency, enabled, session):
    return HEADER.pack(MAGIC, VERSION, TX_CONTROL, 64, 3, request, 0, 0,
                       frequency, 0, 0, 0, 0, int(enabled), 0, session)


def tx_audio(epoch, index, frequency, session, samples):
    if len(samples) != 240 or index % 240:
        raise ValueError("TX audio must contain 240 aligned mono samples")
    samples = np.asarray(samples, dtype=np.float32)
    if not np.isfinite(samples).all():
        raise ValueError("TX audio must contain finite samples")
    pcm = (np.clip(samples, -1, 1)*32767).astype('>i2').tobytes()
    return HEADER.pack(MAGIC, VERSION, TX_AUDIO, 64, 3, epoch, 0, index,
                       frequency, 12000, 0, 0, 240, 2, 240, session) + pcm


FILTER_CONTROL, FILTER_STATE = 9, 10
COMPRESSOR_CONTROL, COMPRESSOR_STATE = 11, 12
LOOP_CONTROL, LOOP_STATE = 13, 14
LOOP_NORMAL, LOOP_AUDIO, LOOP_RF = 0, 1, 2
NR_CONTROL, NR_STATE = 15, 16


def nr_control(request, session, percent=None):
    if percent is not None and (not isinstance(percent, int) or not 0 <= percent <= 100):
        raise ValueError("RX1 NR percent must be 0..100")
    return HEADER.pack(MAGIC,VERSION,NR_CONTROL,64,1,request,0,0,0,
                       12000 if percent is not None else 0,percent or 0,0,0,
                       int(percent is not None),0,session)


def nr_state(packet):
    if len(packet)!=64: return None
    f=HEADER.unpack(packet)
    if (f[:4]!=(MAGIC,VERSION,NR_STATE,64) or f[4]!=1 or f[6] or f[8] or f[9]!=12000
        or f[10]>100 or f[11] or f[12] or f[13]>1 or f[14]): return None
    return f[5],f[15],f[10],f[13]


def loop_control(request, session, mode=None):
    if mode is not None and mode not in (0,1,2):
        raise ValueError("Test loop must be 0 normal, 1 audio, or 2 RF")
    return HEADER.pack(MAGIC,VERSION,LOOP_CONTROL,64,0,request,0,0,0,0,
                       mode or 0,0,0,int(mode is not None),0,session)


def loop_state(packet):
    if len(packet)!=64: return None
    f=HEADER.unpack(packet)
    if (f[:4]!=(MAGIC,VERSION,LOOP_STATE,64) or f[4] or f[9]!=600000
        or f[10]>2 or f[11]>2 or f[12] or f[13]>3 or f[14]): return None
    return f[5],f[15],f[10],f[11],f[8],f[7],f[13]


def compressor_control(request, session, amount=None):
    if amount is not None and (not isinstance(amount, int) or not 0 <= amount <= 1400):
        raise ValueError("TX1 compressor amount must be 0..1400")
    return HEADER.pack(MAGIC, VERSION, COMPRESSOR_CONTROL, 64, 3, request, 0, 0,
                       0, 12000 if amount is not None else 0, amount or 0,
                       0, 0, int(amount is not None), 0, session)


def compressor_state(packet):
    if len(packet) != 64:
        return None
    f = HEADER.unpack(packet)
    if (f[:4] != (MAGIC, VERSION, COMPRESSOR_STATE, 64) or f[4] != 3 or f[6] or f[8]
        or f[9] != 12000 or f[10] > 1400 or f[12] or f[13] not in (0, 1)):
        return None
    return f[5], f[15], f[10], f[13], f[11]/1000, f[14]/1000


def valid_filter(bandwidth, center):
    return (isinstance(bandwidth, int) and isinstance(center, int)
            and 500 <= bandwidth <= 3900 and 0 <= center <= 4000
            and 2*center >= 200+bandwidth and 2*center+bandwidth <= 8000)


def filter_control(request, session, bandwidth=None, center=None):
    setting = bandwidth is not None or center is not None
    if setting and not valid_filter(bandwidth, center):
        raise ValueError("RX1 filter requires BW 500..3900 Hz with edges inside 100..4000 Hz")
    return HEADER.pack(MAGIC, VERSION, FILTER_CONTROL, 64, 1, request, 0, 0,
                       center if setting else 0, 12000 if setting else 0,
                       bandwidth if setting else 0, 0, 0, int(setting), 0, session)


def valid_filter_edges(low, high):
    return (isinstance(low, (int, float)) and isinstance(high, (int, float))
            and 100 <= low <= high <= 4000 and high-low >= 500)


def filter_edges_control(request, session, low=None, high=None):
    setting = low is not None or high is not None
    if setting and not valid_filter_edges(low, high):
        raise ValueError("RX1 filter edges must be inside 100..4000 Hz, at least 500 Hz apart")
    return HEADER.pack(MAGIC, VERSION, FILTER_CONTROL, 64, 1, request, 0, 0,
                       round(low*1000) if setting else 0, 12000 if setting else 0,
                       round(high*1000) if setting else 0, 0, 0, 2 if setting else 3, 0, session)


def filter_state(packet):
    if len(packet) != 64:
        return None
    f = HEADER.unpack(packet)
    if (f[:4] != (MAGIC, VERSION, FILTER_STATE, 64) or f[4] != 1
        or f[9] != 12000 or f[6] or f[12] or f[13] not in (0, 1)):
        return None
    if f[11] or f[14]:
        low, high = f[11]/1000, f[14]/1000
        if not valid_filter_edges(low, high): return None
        return f[5], f[15], high-low, (low+high)/2, f[13]
    if not valid_filter(f[10], f[8]): return None
    # Legacy state: request, backend session, target BW/center, status.
    return f[5], f[15], f[10], f[8], f[13]
