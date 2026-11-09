#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""RX5 QTR2 audio -> bundled MM beacon decoder -> persistent local HTTP texts."""
import argparse
import fcntl
import json
from pathlib import Path
import queue
import signal
import socket
import struct
import subprocess
import sys
import threading

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'external/mmdecoder'))
from data_collector import Collector, TextAPI
from mm_diagnostics import Diagnostics

HEADER = struct.Struct('!4sBBHIIQQQIIIHHII')


def audio_packet(packet):
    if len(packet) != 544:
        return None
    h = HEADER.unpack_from(packet)
    if (h[:5] != (b'QTR2', 1, 5, 64, 8) or h[9:15] != (12000, 0, 0, 240, 2, 240)
            or not h[15] or h[7] % 240 or h[7] > 2**64-241):
        return None
    return (h[15], h[5], h[8]), h[7], packet[64:]


class Decoder:
    """Bounded audio queue keeps UDP reception independent of decode/disk work."""
    def __init__(self, probe, collector):
        self.probe, self.collector = str(probe), collector
        self.diagnostics = Diagnostics()
        self.queue = queue.Queue(maxsize=50)
        self.stopping = threading.Event()
        self.failed = None
        self.process = self.reader = None
        self.worker = threading.Thread(target=self.run, daemon=True)
        self.worker.start()

    def submit(self, value):
        try:
            self.queue.put_nowait(value)
        except queue.Full:
            # The next sample-index gap resets stale DSP history.
            while True:
                try: self.queue.get_nowait()
                except queue.Empty: break
            self.queue.put_nowait(value)

    def finish(self):
        if not self.process:
            return
        try:
            self.process.stdin.close()
        except (OSError, ValueError):
            pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        self.reader.join(timeout=3)
        self.process.stdout.close()
        self.process = None

    def read(self, process):
        try:
            for line in process.stdout:
                event = json.loads(line)
                self.collector.ingest(event)
                self.diagnostics.add_status(event)
        except Exception as exc:
            self.failed = f'MM decoder output: {exc}'
            if process.poll() is None:
                process.terminate()  # Unblock the writer if stdout can no longer be drained.

    def restart(self, identity):
        self.finish()
        self.collector.begin_session()
        self.diagnostics.reset(identity)
        self.process = subprocess.Popen([self.probe, '--input-rate', '12000'],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)  # stderr inherits supervisor log
        self.reader = threading.Thread(target=self.read, args=(self.process,), daemon=True)
        self.reader.start()

    def run(self):
        previous = None
        expected = 0
        try:
            while not self.stopping.is_set():
                try: identity, index, pcm = self.queue.get(timeout=.2)
                except queue.Empty:
                    if self.process and self.process.poll() is not None:
                        raise RuntimeError(f'RX core exited: {self.process.returncode}')
                    continue
                if identity == previous and index < expected:
                    continue  # duplicate/reordered audio
                if identity != previous or index-expected > 6000:
                    self.restart(identity)
                    previous, expected = identity, index
                if index > expected:
                    self.process.stdin.write(bytes((index-expected)*4))
                samples = np.frombuffer(pcm, dtype='>i2').astype('<f4') / np.float32(32768)
                self.diagnostics.add_audio(samples)
                self.process.stdin.write(samples.tobytes())
                self.process.stdin.flush()
                expected = index+240
        except Exception as exc:
            self.failed = f'MM audio: {exc}'
        finally:
            self.finish()

    def close(self):
        self.stopping.set()
        self.worker.join(timeout=8)
        if self.worker.is_alive():
            raise RuntimeError('MM decoder did not stop')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=40916)
    parser.add_argument('--http-port', type=int, default=8766)
    parser.add_argument('--data-dir', type=Path, default=ROOT/'data/mm-beacon')
    parser.add_argument('--probe', type=Path, default=ROOT/'build/bin/qo100_mm_probe')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or not 0 <= args.http_port <= 65535:
        parser.error('Invalid UDP/HTTP port')
    if not args.probe.is_file():
        parser.error('MM RX core missing; run ./build_all.sh')
    args.data_dir.mkdir(parents=True, exist_ok=True)
    stopping = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stopping.set())
    # The imported Collector has a thread lock; this additionally prevents two writers.
    with (args.data_dir/'monitor.lock').open('a') as lock, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: parser.error('MM data directory already has a running monitor')
        udp.bind(('127.0.0.1', args.port))
        udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024*1024)
        udp.settimeout(.2)
        collector = Collector(args.data_dir)
        api = decoder = None
        try:
            decoder = Decoder(args.probe, collector)
            api = TextAPI(collector, args.http_port, diagnostics=decoder.diagnostics.snapshot)
            print(f'MM beacon receiver: RX5 UDP 127.0.0.1:{args.port}; 12000 -> 48000 samples/s; HTTP {api.url}', flush=True)
            while not stopping.is_set():
                if decoder.failed:
                    raise RuntimeError(decoder.failed)
                try: packet, peer = udp.recvfrom(1201)
                except socket.timeout: continue
                if peer[0] != '127.0.0.1': continue
                value = audio_packet(packet)
                if value is not None: decoder.submit(value)
        finally:
            if decoder: decoder.close()
            if api: api.close()
            collector.close()


if __name__ == '__main__':
    try: main()
    except (OSError, RuntimeError) as exc:
        sys.exit(f'MM beacon: {exc}')
