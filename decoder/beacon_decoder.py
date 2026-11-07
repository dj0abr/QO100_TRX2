#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""RX4 UDP -> QO-100 PSK beacon -> UTF-8 UDP, monitoring receiver.

DSP adapted from ta2ldv/qo100-beacon-decoder (GPL-3.0); see the pinned
sources and provenance in external/qo100-beacon-decoder/ORIGIN.md.
Transport, bounded worker and overlap handling are specific to QO100_TRX4.
"""
import os
# Small FIR solves must not spawn dozens of BLAS threads on a desktop.
os.environ['OPENBLAS_NUM_THREADS'] = '1'
os.environ['OMP_NUM_THREADS'] = '1'
import argparse
from collections import deque
from dataclasses import dataclass
from pathlib import Path
import queue
import signal as signals
import socket
import struct
import sys
import threading
import time

try:
    import numpy as np
    from scipy import signal
    from scipy.io import wavfile
except ImportError as error:
    sys.exit(f'{error}. Install python3-numpy and python3-scipy (./build_all.sh --install-deps).')

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'external/qo100-beacon-decoder'))
import qo100_beacon_decoder as reference

HEADER = struct.Struct('!4sBBHIIQQQIIIHHII')
STREAM = 6  # RX4; wire IDs 3/4 remain TX1/audio echo.
RATE = 12000
BEACON_HZ = 10489750000


@dataclass(frozen=True)
class Frame:
    sample: int
    kind: str
    payload: bytes
    corrections: int = 0


def decode_audio(pcm, rate, rx_hz):
    """Batch DSP on an overlapping bounded window, independent of UDP arrival time."""
    x = np.asarray(pcm, dtype=np.float64)
    peak = np.max(np.abs(x)) if len(x) else 0
    expected = BEACON_HZ - rx_hz
    if len(x) < rate * 14 or peak < 1 or not 400 <= expected <= 3300:
        return [], None
    x /= peak
    analytic = signal.hilbert(x)
    n = np.arange(len(x)) / rate
    lp = signal.firwin(301, 900, fs=rate)
    bb = signal.filtfilt(lp, [1], analytic * np.exp(-2j * np.pi * expected * n))
    # Squaring removes the BPSK data. Search +/-500 Hz about the dial-derived
    # carrier, rather than assuming the reference recording's 1500-Hz tone.
    spectrum = np.abs(np.fft.fft(bb ** 2 * np.hanning(len(bb))))
    frequencies = np.fft.fftfreq(len(bb), 1 / rate)
    candidates = np.flatnonzero(np.abs(frequencies) < 1000)
    carrier = expected + frequencies[candidates[np.argmax(spectrum[candidates])]] / 2
    bb = signal.filtfilt(lp, [1], analytic * np.exp(-2j * np.pi * carrier * n))
    squared = signal.filtfilt(signal.firwin(501, 20, fs=rate), [1], bb ** 2)
    real = np.real(bb * np.exp(-.5j * np.unwrap(np.angle(squared))))
    sps = rate / 800
    matched = signal.filtfilt(np.ones(round(sps)) / sps, [1], real)
    # Joint phase and small clock-error search keeps a 40-second window aligned
    # even when the ADC sample clock is slightly different from the beacon clock.
    best, indices = -1, None
    for ppm in range(-100, 101, 20):
        step = sps * (1 + ppm * 1e-6)
        for offset in range(round(sps)):
            ix = np.arange(offset, len(matched) - 1, step).round().astype(int)
            quality = np.mean(np.abs(matched[ix]))
            if quality > best:
                best, indices = quality, ix
    chips = (matched[indices] > 0).astype(np.uint8)
    frames = []
    for alignment in (0, 1):
        bits = reference.chips_to_bits(chips, alignment)
        for start, payload, valid in reference.find_blocks(bits):
            if valid:
                frames.append(Frame(int(indices[min(2 * (start + 1) + alignment, len(indices)-1)]),
                                    'CRC', bytes(payload)))
        for start, payload, corrections in reference.decode_fec_frames(bits):
            if payload is not None:
                frames.append(Frame(int(indices[min(2 * (start + 1) + alignment, len(indices)-1)]),
                                    'FEC', bytes(payload), corrections))
    frames.sort(key=lambda frame: frame.sample)
    unique = []
    for frame in frames:
        if not any(f.kind == frame.kind and f.payload == frame.payload and
                   abs(f.sample - frame.sample) < rate // 20 for f in unique):
            unique.append(frame)
    return unique, carrier


def format_frame(frame, carrier):
    # Checksum/FEC operates on the original bytes; only display strips highlighting.
    display = ''.join(chr(b & 127) if 32 <= (b & 127) < 127 else ' ' for b in frame.payload)
    kind = display[0] if display else '?'
    check = 'CRC OK' if frame.kind == 'CRC' else f'FEC/RS OK, corrected {frame.corrections}'
    lines = '\n'.join(display[i:i+64].rstrip() for i in range(0, len(display), 64))
    return f'[PSK {kind} | {check} | carrier {carrier:.1f} Hz]\n{lines}\n'


class AudioBuffer:
    """Validate the RX4 wire format and invalidate partial frames on discontinuities."""
    def __init__(self):
        self.generation = 0
        self.identity = None
        self.expected = None
        self.chunks = deque(maxlen=2000)  # 40 seconds, independent of network rate
        self.next_decode = 20 * RATE
        self.received = 0

    def push(self, packet, peer):
        if peer[0] != '127.0.0.1' or len(packet) != 544:
            return None
        h = HEADER.unpack_from(packet)
        if (h[:5] != (b'QTR2', 1, 5, 64, STREAM) or h[9:15] != (RATE, 0, 0, 240, 2, 240)):
            return None
        index = h[7]
        identity = peer, h[15], h[5], h[8]
        if identity == self.identity and self.expected is not None and index < self.expected:
            return None  # Late/duplicate packet; do not erase valid buffered audio.
        if identity != self.identity or index != self.expected:
            self.generation += 1
            self.identity = identity
            self.chunks.clear()
            self.received = 0
            self.next_decode = 20 * RATE
            print(f'RX4 reset generation={self.generation}, frequency={h[8]}, index={index}', flush=True)
        self.expected = index + 240
        self.chunks.append(np.frombuffer(packet, dtype='>i2', offset=64).astype(np.int16))
        self.received += 240
        if self.received < self.next_decode:
            return None
        self.next_decode += 10 * RATE
        pcm = np.concatenate(self.chunks)
        return self.generation, index + 240 - len(pcm), h[8], peer, pcm


class Worker:
    """One running job plus one replaceable latest job; UDP reception never waits."""
    def __init__(self):
        self.condition = threading.Condition()
        self.pending = None
        self.stopping = False
        self.results = queue.Queue(maxsize=2)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def submit(self, job):
        with self.condition:
            self.pending = job
            self.condition.notify()

    def run(self):
        while True:
            with self.condition:
                self.condition.wait_for(lambda: self.stopping or self.pending is not None)
                if self.stopping:
                    return
                job, self.pending = self.pending, None
            generation, start, frequency, peer, pcm = job
            then = time.monotonic()
            try:
                frames, carrier = decode_audio(pcm, RATE, frequency)
                result = generation, start, peer, frames, carrier
                while True:
                    try:
                        self.results.put_nowait(result)
                        break
                    except queue.Full:
                        try:
                            self.results.get_nowait()
                        except queue.Empty:
                            pass  # Main thread may have drained it in the meantime.
                tone = f'{carrier:.1f}' if carrier is not None else 'out of range/silence'
                print(f'Window {len(pcm)/RATE:.0f}s: carrier={tone}, '
                      f'CRC={sum(f.kind == "CRC" for f in frames)}, '
                      f'FEC={sum(f.kind == "FEC" for f in frames)}, '
                      f'{time.monotonic()-then:.2f}s CPU elapsed', flush=True)
            except Exception as error:
                print(f'Beacon decode failed: {error}', file=sys.stderr, flush=True)

    def close(self):
        with self.condition:
            self.stopping = True
            self.pending = None
            self.condition.notify()
        self.thread.join(timeout=7)


def serve(port):
    running = True
    def stop(_number, _frame):
        nonlocal running
        running = False
    signals.signal(signals.SIGINT, stop)
    signals.signal(signals.SIGTERM, stop)
    buffer = AudioBuffer()
    worker = Worker()
    recent = deque(maxlen=64)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        udp.bind(('127.0.0.1', port))
        udp.settimeout(.1)
        print(f'PSK beacon receiver: RX4 UDP 127.0.0.1:{port}; '
              'first window 20s, then every 10s; tune RX4 below 10489.750 MHz', flush=True)
        try:
            while running:
                try:
                    packet, peer = udp.recvfrom(65535)
                    job = buffer.push(packet, peer)
                    if job is not None:
                        worker.submit(job)
                except socket.timeout:
                    pass
                while not worker.results.empty():
                    generation, start, peer, frames, carrier = worker.results.get_nowait()
                    if generation != buffer.generation:
                        continue
                    for frame in frames:
                        absolute = start + frame.sample
                        if any(g == generation and k == frame.kind and p == frame.payload and
                               abs(absolute - t) < RATE // 2 for g, k, p, t in recent):
                            continue
                        recent.append((generation, frame.kind, frame.payload, absolute))
                        text = format_frame(frame, carrier)
                        udp.sendto(text.encode('utf-8'), peer)
                        print(text, end='', flush=True)
        finally:
            worker.close()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=40912)
    parser.add_argument('--wav', type=Path, help='decode a recording offline instead of listening on UDP')
    parser.add_argument('--rx-hz', type=int, default=10489748500, help='USB dial of the WAV recording')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('--port must be in 1..65535')
    if args.wav:
        rate, pcm = wavfile.read(args.wav)
        if pcm.ndim == 2:
            pcm = pcm[:, 0]
        frames, carrier = decode_audio(pcm, rate, args.rx_hz)
        for frame in frames:
            print(format_frame(frame, carrier), end='')
        print(f'Decoded {sum(f.kind == "CRC" for f in frames)} CRC blocks and '
              f'{sum(f.kind == "FEC" for f in frames)} FEC blocks.')
        return 0 if frames else 1
    return serve(args.port)


if __name__ == '__main__':
    sys.exit(main())
