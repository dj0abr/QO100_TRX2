"""Bounded audio jitter buffer, clock correction and Linux default ALSA output.

Uses system libasound and libsamplerate through ctypes; no Python audio package.
"""
import ctypes as C
import ctypes.util
import threading
import time
import numpy as np


class JitterBuffer:
    def __init__(self):
        self.lock = threading.RLock()
        self.identity = None
        self.packets = {}
        self.next_index = None
        self.losses = 0
        self.overruns = 0

    def add(self, packet):
        with self.lock:
            identity = (packet.stream_id, packet.session_id, packet.epoch)
            if identity != self.identity:
                self.identity = identity
                self.packets.clear()
                self.next_index = None
            if self.next_index is not None and packet.sample_index < self.next_index:
                return
            self.packets[packet.sample_index] = packet.samples
            if sum(map(len, self.packets.values())) > 6000:
                # Keep at most 0.5 s; recover to recent audio instead of growing delay.
                newest = sorted(self.packets)[-10:]
                self.packets = {k: self.packets[k] for k in newest}
                self.next_index = min(newest)
                self.overruns += 1

    def level(self):
        with self.lock:
            return sum(map(len, self.packets.values()))

    def clear(self):
        with self.lock:
            self.identity=None;self.packets.clear();self.next_index=None

    def take(self):
        with self.lock:
            if not self.packets:
                return None
            if self.next_index is None:
                self.next_index = min(self.packets)
            if self.next_index in self.packets:
                result = self.packets.pop(self.next_index)
            else:
                # A missing 20 ms packet gets silence; later packets retain their time.
                gap = min(self.packets) - self.next_index
                result = np.zeros(min(gap, 240), dtype=np.float32)
                self.losses += 1
            self.next_index += len(result)
            return result


class SrcData(C.Structure):
    _fields_ = [("data_in", C.POINTER(C.c_float)), ("data_out", C.POINTER(C.c_float)),
                ("input_frames", C.c_long), ("output_frames", C.c_long),
                ("input_frames_used", C.c_long), ("output_frames_gen", C.c_long),
                ("end_of_input", C.c_int), ("src_ratio", C.c_double)]


class AudioPlayer:
    def __init__(self):
        self.buffer = JitterBuffer()
        self.stop = threading.Event()
        self.error = ""
        self.written = 0
        self.underruns = 0
        self.ppm = 0
        self.volume = 1.0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def add(self, packet):
        self.buffer.add(packet)

    def reset(self):
        self.buffer.clear()

    def run(self):
        pcm = C.c_void_p()
        state = None
        alsa = src = None
        try:
            alsa = C.CDLL(ctypes_path("asound"))
            src = C.CDLL(ctypes_path("samplerate"))
            alsa.snd_pcm_open.argtypes = [C.POINTER(C.c_void_p), C.c_char_p, C.c_int, C.c_int]
            alsa.snd_pcm_set_params.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_int, C.c_uint]
            alsa.snd_pcm_writei.argtypes = [C.c_void_p, C.c_void_p, C.c_ulong]
            alsa.snd_pcm_writei.restype = C.c_long
            alsa.snd_pcm_recover.argtypes = [C.c_void_p, C.c_int, C.c_int]
            alsa.snd_pcm_drop.argtypes = alsa.snd_pcm_prepare.argtypes = alsa.snd_pcm_close.argtypes = [C.c_void_p]
            alsa.snd_strerror.argtypes = [C.c_int]; alsa.snd_strerror.restype = C.c_char_p
            src.src_new.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]; src.src_new.restype = C.c_void_p
            src.src_process.argtypes = [C.c_void_p, C.POINTER(SrcData)]
            src.src_reset.argtypes = src.src_delete.argtypes = [C.c_void_p]
            error = C.c_int()
            state = src.src_new(2, 1, C.byref(error))
            if not state:
                raise RuntimeError(f"Audio resampler error {error.value}")
            while not self.stop.is_set() and self.buffer.level() < 2400:
                self.stop.wait(.01)
            if self.stop.is_set():
                return
            code = alsa.snd_pcm_open(C.byref(pcm), b"default", 0, 0)
            if code < 0:
                raise RuntimeError(alsa.snd_strerror(code).decode())
            # S16_LE, interleaved mono, 12 kHz; ALSA may convert to hardware rate.
            code = alsa.snd_pcm_set_params(pcm, 2, 3, 1, 12000, 1, 80000)
            if code < 0:
                raise RuntimeError(alsa.snd_strerror(code).decode())
            identity = None
            pending = np.empty(0, dtype=np.float32)
            integral = smoothed = 0.
            while not self.stop.is_set():
                with self.buffer.lock:
                    current = self.buffer.identity
                    if current != identity:
                        identity = current
                        pending = np.empty(0, dtype=np.float32)
                        src.src_reset(state)
                        alsa.snd_pcm_drop(pcm); alsa.snd_pcm_prepare(pcm)
                        integral = smoothed = 0.
                    while len(pending) < 512:
                        more = self.buffer.take()
                        if more is None:
                            break
                        pending = np.concatenate((pending, more))
                    level = len(pending) + self.buffer.level()
                if len(pending) < 240:
                    self.stop.wait(.01)
                    continue
                smoothed += .002 * ((level - 1200) / 1200 - smoothed)
                integral = float(np.clip(integral + smoothed * .000002, -.001, .001))
                correction = float(np.clip(.001 * smoothed + integral, -.003, .003))
                self.ppm = correction * 1e6
                output = np.empty(240, dtype=np.float32)
                d = SrcData(pending.ctypes.data_as(C.POINTER(C.c_float)), output.ctypes.data_as(C.POINTER(C.c_float)),
                            len(pending), 240, 0, 0, 0, 1 - correction)
                if src.src_process(state, C.byref(d)):
                    raise RuntimeError("Audio clock resampler failed")
                pending = pending[d.input_frames_used:].copy()
                if not d.output_frames_gen:
                    continue
                pcm_data = (np.clip(output[:d.output_frames_gen] * self.volume, -1, 1) * 32767).astype("<i2")
                sent = 0
                while sent < len(pcm_data) and not self.stop.is_set():
                    code = alsa.snd_pcm_writei(pcm, pcm_data.ctypes.data + 2*sent, len(pcm_data)-sent)
                    if code < 0:
                        self.underruns += 1
                        if alsa.snd_pcm_recover(pcm, code, 1) < 0:
                            raise RuntimeError(alsa.snd_strerror(code).decode())
                    elif code:
                        sent += code; self.written += code
                    else:
                        self.stop.wait(.005)
        except (OSError, RuntimeError) as exc:
            self.error = str(exc)
        finally:
            if pcm.value and alsa:
                alsa.snd_pcm_drop(pcm); alsa.snd_pcm_close(pcm)
            if state and src:
                src.src_delete(state)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)


def ctypes_path(name):
    path = C.util.find_library(name)
    if not path:
        raise RuntimeError(f"Missing system library: {name}")
    return path


class Microphone:
    """Default ALSA capture at 48 kHz, sinc anti-alias downsampling to 12 kHz.

    Open only while software PTT is held. Nonblocking capture permits prompt
    shutdown even if the default device stops supplying samples.
    """
    def __init__(self, sink):
        self.sink = sink
        self.stop = threading.Event()
        self.error = ""
        self.samples = 0
        self.rms = 0.
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        pcm = C.c_void_p()
        state = None
        alsa = src = None
        try:
            alsa = C.CDLL(ctypes_path("asound"))
            src = C.CDLL(ctypes_path("samplerate"))
            alsa.snd_pcm_open.argtypes = [C.POINTER(C.c_void_p), C.c_char_p, C.c_int, C.c_int]
            alsa.snd_pcm_set_params.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_uint, C.c_uint, C.c_int, C.c_uint]
            alsa.snd_pcm_readi.argtypes = [C.c_void_p, C.c_void_p, C.c_ulong]
            alsa.snd_pcm_readi.restype = C.c_long
            alsa.snd_pcm_recover.argtypes = [C.c_void_p, C.c_int, C.c_int]
            alsa.snd_pcm_drop.argtypes = alsa.snd_pcm_close.argtypes = [C.c_void_p]
            alsa.snd_strerror.argtypes = [C.c_int]; alsa.snd_strerror.restype = C.c_char_p
            src.src_new.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]; src.src_new.restype = C.c_void_p
            src.src_process.argtypes = [C.c_void_p, C.POINTER(SrcData)]
            src.src_delete.argtypes = [C.c_void_p]
            code = alsa.snd_pcm_open(C.byref(pcm), b"default", 1, 1)  # capture, nonblock
            if code < 0:
                raise RuntimeError(alsa.snd_strerror(code).decode())
            code = alsa.snd_pcm_set_params(pcm, 2, 3, 1, 48000, 1, 80000)
            if code < 0:
                raise RuntimeError(alsa.snd_strerror(code).decode())
            error = C.c_int()
            state = src.src_new(2, 1, C.byref(error))
            if not state:
                raise RuntimeError(f"Microphone resampler error {error.value}")
            raw = np.empty(960, dtype='<i2')
            pending = np.empty(0, dtype=np.float32)
            packets = np.empty(0, dtype=np.float32)
            output = np.empty(512, dtype=np.float32)
            while not self.stop.is_set():
                count = alsa.snd_pcm_readi(pcm, raw.ctypes.data, len(raw))
                if count == -11:  # EAGAIN
                    self.stop.wait(.003)
                    continue
                if count < 0:
                    if alsa.snd_pcm_recover(pcm, count, 1) < 0:
                        raise RuntimeError(alsa.snd_strerror(count).decode())
                    continue
                if not count:
                    self.stop.wait(.003)
                    continue
                pending = np.concatenate((pending, raw[:count].astype(np.float32)/32768))
                d = SrcData(pending.ctypes.data_as(C.POINTER(C.c_float)), output.ctypes.data_as(C.POINTER(C.c_float)),
                            len(pending), len(output), 0, 0, 0, .25)
                if src.src_process(state, C.byref(d)):
                    raise RuntimeError("Microphone downsampling failed")
                pending = pending[d.input_frames_used:].copy()
                packets = np.concatenate((packets, output[:d.output_frames_gen]))
                while len(packets) >= 240 and not self.stop.is_set():
                    block = packets[:240].copy()
                    packets = packets[240:].copy()
                    self.rms = float(np.sqrt(np.mean(block**2)))
                    self.samples += 240
                    self.sink(block)
        except (OSError, RuntimeError) as exc:
            self.error = str(exc)
        finally:
            if pcm.value and alsa:
                alsa.snd_pcm_drop(pcm); alsa.snd_pcm_close(pcm)
            if state and src:
                src.src_delete(state)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)
