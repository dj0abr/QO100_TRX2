#!/usr/bin/env python3
"""Small UDP spectrum/waterfall client; also supports headless hardware tests."""
import argparse
import json
from pathlib import Path
import queue
import socket
import threading
import time
import numpy as np
from protocol import Assembler, subscription, tune, audio_packet, HEADER, STATE, AUDIO
from protocol import TX_STATE, tx_control, tx_audio
from protocol import FILTER_STATE, filter_control, filter_state, valid_filter
from protocol import COMPRESSOR_STATE, compressor_control, compressor_state
from protocol import LOOP_STATE, LOOP_AUDIO, LOOP_RF, loop_control, loop_state
from protocol import NR_STATE, nr_control, nr_state
from protocol import SETTINGS_STATE
from settings import SettingsClient
from decimal import Decimal
from tuning import PlotGeometry, DOWNLINK_UPLINK_HZ, uplink_hz, tx_offset_hz, mhz_text, wheel_steps


class AudioReceiveState:
    def __init__(self, frequency, sink=None):
        self.command = (1, frequency)
        self.applied_hz = None
        self.identity = None
        self.tune_error = ""
        self.frames = queue.Queue(maxsize=30)
        self.sink = sink
        self.received = 0
        self.samples = 0
        self.rms = self.peak = 0.
        self.last_audio = 0.


class Receiver:
    def __init__(self, host, port, rx_hz=None, audio_sink=None, rx2_hz=None, tx_hz=None):
        self.settings=SettingsClient()
        self.initial_overrides=(rx_hz,rx2_hz,tx_hz)
        self.settings_generation=0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.connect((host, port))
        self.sock.settimeout(0.02)
        self.assembler = Assembler()
        self.zoom_assembler = Assembler(1)
        self.zoom_frames = queue.Queue(maxsize=4)
        self.channels = {1: AudioReceiveState(rx_hz, audio_sink), 2: AudioReceiveState(rx2_hz)}
        self.tx_lock = threading.RLock()
        self.tx_command = (1, tx_hz, False)
        self.tx_identity = None
        self.tx_session = None
        self.tx_applied_hz = None
        self.tx_confirmed = False
        self.tx_index = self.tx_packets = 0
        self.tx_error = ""
        self.tx_fill = self.tx_pluto_fill = self.tx_ppm = 0
        self.filter_lock = threading.RLock()
        self.filter_command = (1, None, None)  # request, BW, center; None queries backend defaults
        self.filter_session = None
        self.filter_applied = None
        self.filter_acked = None
        self.filter_error = ""
        self.nr_lock = threading.RLock()
        self.nr_command = (1,None)
        self.nr_session = self.nr_applied = self.nr_acked = None
        self.nr_error = ""
        self.compressor_lock = threading.RLock()
        self.compressor_command = (1, None)  # Query the configured amount first.
        self.compressor_session = self.compressor_applied = self.compressor_acked = None
        self.compressor_error = ""
        self.compression_db = self.limiting_db = 0.
        self.loop_lock = threading.RLock()
        self.loop_command = (1,None)
        self.loop_session = self.loop_mode = self.loop_acked = self.loop_generation = None
        self.loop_target = 0
        self.loop_status = 0
        self.loop_rx_lo = None
        self.loop_last_stamp = 0
        self.loop_error = ""
        self.echo_frames = queue.Queue(maxsize=30)
        self.echo_received = self.echo_samples = 0
        self.audio_frames = self.channels[1].frames
        self.audio2_frames = self.channels[2].frames
        self.frames = queue.Queue(maxsize=4)
        self.received = 0
        self.invalid_audio = 0
        self.error = "Waiting for backend"
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @property
    def applied_hz(self):
        return self.channels[1].applied_hz

    @property
    def audio_received(self):
        return self.channels[1].received

    @property
    def tune_error(self):
        return self.channels[1].tune_error

    @tune_error.setter
    def tune_error(self, error):
        self.channels[1].tune_error = error

    def run(self):
        next_subscription = 0
        last_commands = None
        last_tx = None
        next_tx = 0
        last_filter = None
        next_filter = 0
        last_nr = None
        next_nr = 0
        last_compressor = None
        next_compressor = 0
        last_loop = None
        next_loop = 0
        while not self.stop.is_set():
            commands = tuple((stream, state.command) for stream, state in self.channels.items())
            if time.monotonic() >= next_subscription or commands != last_commands:
                try:
                    self.sock.send(subscription())
                    for stream, command in commands:
                        if command[1] is not None and self.settings.ready:
                            self.sock.send(tune(*command, stream_id=stream,session=self.settings.session))
                    last_commands = commands
                except OSError as exc:
                    self.error = str(exc)
                next_subscription = time.monotonic() + 2
            try: self.settings.send(self.sock)
            except OSError as exc: self.settings.error=str(exc)
            with self.tx_lock:
                command = self.tx_command
                if self.tx_session is not None and (command != last_tx or time.monotonic() >= next_tx):
                    try:
                        self.sock.send(tx_control(*command, self.tx_session))
                        last_tx = command
                        next_tx = time.monotonic() + .5
                    except OSError as exc:
                        self.tx_error = str(exc)
            with self.filter_lock:
                command = self.filter_command
                if self.filter_session is not None and (command != last_filter or time.monotonic() >= next_filter):
                    try:
                        request, bandwidth, center = command
                        self.sock.send(filter_control(request, self.filter_session, bandwidth, center))
                        last_filter = command
                        next_filter = time.monotonic() + 1
                    except OSError as exc:
                        self.filter_error = str(exc)
            with self.nr_lock:
                command=self.nr_command
                if self.nr_session is not None and (command!=last_nr or time.monotonic()>=next_nr):
                    try:
                        request,percent=command
                        self.sock.send(nr_control(request,self.nr_session,percent))
                        last_nr=command;next_nr=time.monotonic()+.25
                    except OSError as exc: self.nr_error=str(exc)
            with self.compressor_lock:
                command = self.compressor_command
                if self.compressor_session is not None and (command != last_compressor or time.monotonic() >= next_compressor):
                    try:
                        request, amount = command
                        self.sock.send(compressor_control(request, self.compressor_session, amount))
                        last_compressor = command
                        next_compressor = time.monotonic() + .25
                    except OSError as exc:
                        self.compressor_error = str(exc)
            with self.loop_lock:
                command=self.loop_command
                if self.loop_session is not None and (command!=last_loop or time.monotonic()>=next_loop):
                    try:
                        request,mode=command
                        self.sock.send(loop_control(request,self.loop_session,mode))
                        last_loop=command;next_loop=time.monotonic()+.25
                    except OSError as exc: self.loop_error=str(exc)
            try:
                packet = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError as exc:
                if self.stop.is_set():
                    break
                self.error = str(exc)
                self.stop.wait(0.05)
                continue
            if len(packet) >= 64 and packet[:4] == b"QTR2" and packet[4] == 1:
                fields = HEADER.unpack_from(packet)
                if packet[5]==SETTINGS_STATE:
                    values=self.settings.receive(packet)
                    if values is not None:
                        for stream,key,override in ((1,'initial_rx_hz',self.initial_overrides[0]),(2,'initial_rx2_hz',self.initial_overrides[1])):
                            self.set_frequency(int(values[key]) if override is None else override,stream)
                        with self.tx_lock:
                            self.set_tx(frequency=int(values['initial_tx_hz']) if self.initial_overrides[2] is None else self.initial_overrides[2],enabled=False)
                        self.initial_overrides=(None,None,None)
                        self.settings_generation+=1
                    continue
                if packet[5]==NR_STATE:
                    result=nr_state(packet)
                    with self.nr_lock:
                        if result and result[0]==self.nr_command[0] and result[1]==self.nr_session:
                            self.nr_applied=result[2];self.nr_acked=result[0]
                            self.nr_error="RX1 NR außerhalb 0..100 %" if result[3] else ""
                    continue
                if packet[5]==LOOP_STATE:
                    result=loop_state(packet)
                    with self.loop_lock:
                        if (result and result[0]==self.loop_command[0] and result[1]==self.loop_session
                            and fields[6]>=self.loop_last_stamp):
                            self.loop_last_stamp=fields[6]
                            previous_generation=self.loop_generation
                            _,_,self.loop_mode,self.loop_target,self.loop_rx_lo,self.loop_generation,self.loop_status=result
                            self.loop_acked=result[0] if self.loop_status!=3 else None
                            self.loop_error={0:"",1:"Ungültiger Testmodus",2:"HF-Loop benötigt Pluto-Betrieb",3:""}.get(self.loop_status,"Loop-Fehler")
                            if previous_generation is not None and previous_generation!=self.loop_generation:
                                self.assembler=Assembler();self.zoom_assembler=Assembler(1)
                                self.assembler.last_timestamp=self.zoom_assembler.last_timestamp=fields[6]
                                for source in (self.frames,self.zoom_frames):
                                    while True:
                                        try: source.get_nowait()
                                        except queue.Empty: break
                    continue
                if packet[5] == COMPRESSOR_STATE:
                    result = compressor_state(packet)
                    with self.compressor_lock:
                        if result and result[0] == self.compressor_command[0] and result[1] == self.compressor_session:
                            self.compressor_applied = result[2]
                            self.compressor_acked = result[0]
                            self.compressor_error = "TX1 compressor outside 0..140 percent" if result[3] else ""
                            self.compression_db, self.limiting_db = result[4:]
                    continue
                if packet[5] == FILTER_STATE:
                    result = filter_state(packet)
                    with self.filter_lock:
                        if result and result[0] == self.filter_command[0] and result[1] == self.filter_session:
                            self.filter_applied = (result[2], result[3])
                            self.filter_acked = result[0]
                            self.filter_error = "RX1 filter outside 100..4000 Hz or BW outside 500..3900 Hz" if result[4] else ""
                    continue
                if packet[5] == TX_STATE:
                    with self.tx_lock:
                        if (len(packet) == 64 and fields[3] == 64 and fields[4] == 3 and
                            fields[5] == self.tx_command[0] and fields[15] == self.tx_session):
                            self.tx_error = {0:"",1:"TX frequency outside capture window",2:"Pluto wartet / Umschaltung läuft"}.get(fields[13],"TX error")
                            identity = (fields[15], fields[7])
                            if identity != self.tx_identity:
                                self.tx_index = 0
                            self.tx_identity = identity
                            self.tx_applied_hz = fields[8]
                            self.tx_confirmed = bool(fields[11]) and not fields[13]
                            self.tx_fill, self.tx_pluto_fill = fields[10], fields[12]
                            self.tx_ppm = fields[14] if fields[14] < 2**31 else fields[14]-2**32
                    continue
                if packet[5] == STATE:
                    state = self.channels.get(fields[4])
                    if len(packet) == 64 and fields[3] == 64 and state and fields[5] == state.command[0]:
                        with self.tx_lock:
                            if self.tx_session != fields[15]:
                                if self.tx_session is not None and fields[15]!=self.settings.session:
                                    self.settings.reset()
                                self.tx_session = fields[15]
                                # A backend restart always requires a new manual PTT press.
                                request, hz, _ = self.tx_command
                                self.tx_command = ((request+1)&0xffffffff, hz, False)
                                self.tx_confirmed = False
                                self.tx_identity = None
                        with self.filter_lock:
                            if self.filter_session != fields[15]:
                                self.filter_session = fields[15]
                                request, bandwidth, center = self.filter_command
                                self.filter_command = ((request+1)&0xffffffff, None, None)
                                self.filter_applied = None
                                self.filter_acked = None
                        with self.nr_lock:
                            if self.nr_session!=fields[15]:
                                self.nr_session=fields[15]
                                request,percent=self.nr_command
                                self.nr_command=((request+1)&0xffffffff,None)
                                self.nr_applied=self.nr_acked=None
                        with self.compressor_lock:
                            if self.compressor_session != fields[15]:
                                self.compressor_session = fields[15]
                                request, amount = self.compressor_command
                                self.compressor_command = ((request+1)&0xffffffff, None)
                                self.compressor_applied = self.compressor_acked = None
                        with self.loop_lock:
                            if self.loop_session!=fields[15]:
                                request,mode=self.loop_command
                                self.loop_command=((request+1)&0xffffffff,0 if self.loop_session is not None else None)
                                self.loop_session=fields[15]
                                self.loop_mode=self.loop_acked=self.loop_generation=None
                                self.loop_last_stamp=0
                        state.tune_error = "RX frequency outside capture window" if fields[13] else ""
                        if not fields[13]:
                            identity = (fields[15], fields[7])
                            if state.identity != identity:
                                if fields[4]==1:
                                    self.zoom_assembler=Assembler(1)
                                    while True:
                                        try: self.zoom_frames.get_nowait()
                                        except queue.Empty: break
                                while True:
                                    try: state.frames.get_nowait()
                                    except queue.Empty: break
                                state.rms = state.peak = state.last_audio = 0.
                            state.applied_hz = fields[8]
                            state.identity = identity
                    continue
                if packet[5] == AUDIO:
                    audio = audio_packet(packet)
                    if audio is None:
                        self.invalid_audio += 1
                        continue
                    if audio.stream_id==4:
                        if (self.loop_mode==LOOP_AUDIO and self.tx_command[2] and
                            (audio.session_id,audio.epoch)==self.tx_identity and
                            audio.center_hz==self.tx_applied_hz and len(audio.samples)==240):
                            self.echo_received+=1;self.echo_samples+=len(audio.samples)
                            self.offer(self.echo_frames,audio)
                            sink=self.channels[1].sink
                            if sink: sink(audio)
                        continue
                    state = self.channels[audio.stream_id]
                    if (audio.session_id, audio.epoch) == state.identity:
                        state.received += 1
                        state.samples += len(audio.samples)
                        state.rms = float(np.sqrt(np.mean(audio.samples**2)))
                        state.peak = float(np.max(np.abs(audio.samples)))
                        state.last_audio = time.monotonic()
                        if state.sink and self.loop_mode!=LOOP_AUDIO:
                            state.sink(audio)
                        elif not state.sink:
                            self.offer(state.frames, audio)
                    continue
                if fields[4] == 1:
                    if self.applied_hz is None or fields[8]!=self.applied_hz: continue
                    frame = self.zoom_assembler.push(packet)
                    if frame is not None:
                        self.offer(self.zoom_frames, frame)
                    continue
            frame = self.assembler.push(packet)
            if frame is not None:
                self.received += 1
                self.error = ""
                self.offer(self.frames, frame)

    @staticmethod
    def offer(target, item):
        if target.full():
            try: target.get_nowait()
            except queue.Empty: pass
        target.put_nowait(item)

    def set_frequency(self, hz, stream_id=1):
        state = self.channels[stream_id]
        state.command = ((state.command[0] + 1) & 0xffffffff, hz)

    def set_filter(self, bandwidth, center):
        if not valid_filter(bandwidth, center):
            raise ValueError("RX1 filter requires BW 500..3900 Hz with edges inside 100..4000 Hz")
        with self.filter_lock:
            self.filter_command = ((self.filter_command[0]+1)&0xffffffff, bandwidth, center)
            self.filter_acked = None
            self.filter_error = ""
            return self.filter_command[0]

    def query_filter(self):
        with self.filter_lock:
            self.filter_command = ((self.filter_command[0]+1)&0xffffffff, None, None)
            self.filter_acked = None
            return self.filter_command[0]

    def set_nr(self, percent=None):
        if percent is not None and (not isinstance(percent,int) or not 0<=percent<=100):
            raise ValueError("RX1 NR percent must be 0..100")
        with self.nr_lock:
            self.nr_command=((self.nr_command[0]+1)&0xffffffff,percent)
            self.nr_acked=None;self.nr_error=""
            return self.nr_command[0]

    def set_tx(self, frequency=None, enabled=None):
        with self.tx_lock:
            request, hz, ptt = self.tx_command
            self.tx_command = ((request+1)&0xffffffff, hz if frequency is None else frequency,
                               ptt if enabled is None else enabled)
            self.tx_confirmed = False

    def set_compressor(self, amount=None):
        if amount is not None and (not isinstance(amount, int) or not 0 <= amount <= 1400):
            raise ValueError("TX1 compressor amount must be 0..1400")
        with self.compressor_lock:
            self.compressor_command = ((self.compressor_command[0]+1)&0xffffffff, amount)
            self.compressor_acked = None
            self.compressor_error = ""
            return self.compressor_command[0]

    def set_loop(self, mode):
        if mode not in (0,1,2): raise ValueError("Test loop must be 0 normal, 1 audio, or 2 RF")
        self.set_tx(enabled=False)
        with self.loop_lock:
            self.loop_command=((self.loop_command[0]+1)&0xffffffff,mode)
            self.loop_acked=None;self.loop_error=""
            return self.loop_command[0]

    def send_microphone(self, samples):
        with self.tx_lock:
            _, hz, enabled = self.tx_command
            if not enabled or not self.tx_confirmed or self.tx_identity is None or self.tx_applied_hz != hz:
                return
            session, epoch = self.tx_identity
            try:
                self.sock.send(tx_audio(epoch, self.tx_index, hz, session, samples))
                self.tx_index += len(samples)
                self.tx_packets += 1
            except OSError as exc:
                self.tx_error = str(exc)

    def close(self):
        deadline=time.monotonic()+1
        while self.settings.values is not None and (self.settings.preferences or self.settings.patch is not None) and time.monotonic()<deadline:
            try: self.settings.send(self.sock)
            except OSError: break
            time.sleep(.02)
        self.set_loop(0)
        with self.tx_lock:
            if self.tx_session is not None:
                try: self.sock.send(tx_control(*self.tx_command, self.tx_session))
                except OSError: pass
        with self.loop_lock:
            if self.loop_session is not None:
                try: self.sock.send(loop_control(self.loop_command[0],self.loop_session,0))
                except OSError: pass
        self.stop.set()
        self.thread.join(timeout=1)
        self.sock.close()


def save_capture(path, frame):
    np.savez_compressed(path, db=frame.db, center_hz=frame.center_hz,
                        sample_rate=frame.sample_rate, fft_size=frame.fft_size,
                        sample_index=frame.sample_index, timestamp_ns=frame.timestamp_ns)


def headless(args):
    receiver = Receiver(args.host, args.port, parse_frequency(args.rx_frequency), rx2_hz=parse_frequency(args.rx2_frequency), tx_hz=parse_frequency(args.tx_frequency))
    start = time.monotonic()
    received = []
    try:
        while len(received) < args.frames and time.monotonic() - start < args.timeout:
            try:
                frame = receiver.frames.get(timeout=0.2)
            except queue.Empty:
                continue
            received.append(frame)
        if len(received) < args.frames:
            raise RuntimeError(f"Only {len(received)}/{args.frames} frames received: {receiver.error}")
        f = received[-1]
        if args.capture:
            save_capture(args.capture, f)
        if args.screenshot:
            draw_image(received).save(args.screenshot)
        result = {
            "frames": len(received), "center_hz": f.center_hz,
            "sample_rate": f.sample_rate, "fft_size": f.fft_size,
            "bin_hz": f.sample_rate / f.fft_size,
            "peak_hz": float(f.frequencies_hz[np.argmax(f.db)]),
            "peak_dbfs": float(f.db.max()), "median_dbfs": float(np.median(f.db)),
            "invalid_packets": receiver.assembler.invalid_packets,
            "incomplete_frames": receiver.assembler.incomplete_frames,
            "sample_index": f.sample_index,
            "rx2_frequency_hz":receiver.channels[2].applied_hz,
            "rx2_audio_packets":receiver.channels[2].received,
            "rx2_audio_samples":receiver.channels[2].samples,
            "rx2_rms":receiver.channels[2].rms,
        }
        if len(received) > 1 and f.timestamp_ns > received[0].timestamp_ns:
            result["observed_iq_rate"] = (f.sample_index - received[0].sample_index) / ((f.timestamp_ns - received[0].timestamp_ns) / 1e9)
        print(json.dumps(result, indent=2))
        return 0
    finally:
        receiver.close()


def palette():
    anchors = np.array([[4, 8, 24], [14, 39, 91], [8, 117, 159],
                        [41, 199, 169], [243, 222, 87], [255, 103, 58]], dtype=float)
    x = np.linspace(0, len(anchors) - 1, 256)
    return np.stack([np.interp(x, np.arange(len(anchors)), anchors[:, i]) for i in range(3)], axis=1).astype(np.uint8)


def draw_image(frames, width=1100, height=650, floor=-100, ceiling=0, span_hz=None, rx_hz=None, tx_downlink_hz=None):
    from PIL import Image, ImageDraw
    image = Image.new("RGB", (width, height), "#0b1321")
    draw = ImageDraw.Draw(image)
    frame = frames[-1]
    span = span_hz or frame.sample_rate
    geometry = PlotGeometry(frame.center_hz,span,width,height)
    x0,x1,y0,y1=geometry.left,geometry.right,geometry.spectrum_top,geometry.spectrum_bottom
    waterfall_y = geometry.waterfall_top
    mask = np.abs(np.arange(frame.fft_size)-frame.fft_size/2) <= span * frame.fft_size / frame.sample_rate / 2
    draw.text((20, 15), ("QO100_TRX2 | USB zoom +/-25 kHz" if frame.stream_id else "QO100_TRX2 | Full RX spectrum / waterfall"), fill="#e8edf7")
    draw.text((20, 35), f"Center {frame.center_hz / 1e6:.6f} MHz | {span / 1000:g} kHz span | FFT {frame.fft_size} | {frame.sample_rate/frame.fft_size:.2f} Hz/bin", fill="#a4b5cd")
    for db in range(int(floor), int(ceiling) + 1, 20):
        y = y1 - (db - floor) / (ceiling - floor) * (y1 - y0)
        draw.line((x0, y, x1, y), fill="#243348")
        draw.text((8, y - 5), f"{db} dBFS", fill="#a4b5cd")
    db = resample_display(frame.db[mask], x1 - x0)
    points = [(x0 + i, y1 - np.clip((v - floor) / (ceiling - floor), 0, 1) * (y1 - y0)) for i, v in enumerate(db)]
    draw.line(points, fill="#54e0ca", width=1)
    for i in range(7):
        x = x0 + (x1 - x0) * i / 6
        hz = frame.center_hz - span / 2 + span * i / 6
        label = f"{hz / 1e6:.6f}" if span_hz else f"{hz / 1e6:.3f}"
        box = draw.textbbox((0, 0), label)
        text_x = max(2, min(width - (box[2]-box[0]) - 2, x - (box[2]-box[0])/2))
        draw.text((text_x, y1 + 10), label, fill="#a4b5cd")
    rows = np.stack([resample_display(f.db[mask], x1 - x0) for f in reversed(frames)])
    scaled = np.clip((rows - floor) / (ceiling - floor) * 255, 0, 255).astype(np.uint8)
    waterfall = Image.fromarray(palette()[scaled]).resize((x1 - x0, height - waterfall_y - 15), resample=Image.Resampling.NEAREST)
    image.paste(waterfall, (x0, waterfall_y))
    for label,hz,color,dy in (("RX1",rx_hz,"#7cabff",2),("TX",tx_downlink_hz,"#ffb75c",15)):
        if hz is None: continue
        x=geometry.x(hz)
        if x0<=x<x1:
            draw.line((x,y0,x,y1),fill=color)
            draw.line((x,waterfall_y,x,geometry.bottom-1),fill=color)
            draw.text((min(x+3,x1-30),y0+dy),label,fill=color)
    return image


def resample_display(values, width):
    # Retain narrow signals when combining several FFT bins into one screen pixel.
    if len(values) >= width:
        edges = np.linspace(0, len(values), width + 1, dtype=int)
        return np.maximum.reduceat(values, edges[:-1])
    return np.interp(np.linspace(0, len(values) - 1, width), np.arange(len(values)), values)


def gui(args):
    import tkinter as tk
    from PIL import ImageTk
    root = tk.Tk()
    root.title("QO100_TRX2 — USB RX/TX Test")
    root.geometry("1120x1030")
    root.configure(bg="#0b1321")
    from audio import AudioPlayer
    player = None if args.no_audio else AudioPlayer()
    receiver = Receiver(args.host, args.port, parse_frequency(args.rx_frequency), player.add if player else None, rx2_hz=parse_frequency(args.rx2_frequency), tx_hz=parse_frequency(args.tx_frequency))
    history, zoom_history = [], []
    wheel_step_hz=getattr(args,'wheel_step_hz',None) or 10
    ui_settings_generation=0
    ui_settings_syncing=True
    title = tk.Label(root, text=f"QO100_TRX2 — {args.host}:{args.port}", bg="#0b1321", fg="#e8edf7", font=("sans", 15))
    title.pack(pady=8)
    controls = tk.Frame(root, bg="#0b1321")
    controls.pack()
    floor = tk.DoubleVar(value=-100)
    def floor_changed(value):
        if not ui_settings_syncing: receiver.settings.save_preferences(gui_display_floor=float(value))
    tk.Label(controls, text="Display floor (dBFS)", bg="#0b1321", fg="#a4b5cd").pack(side="left")
    tk.Scale(controls, from_=-160, to=-40, resolution=.1, orient="horizontal", variable=floor,
             name="display_floor",command=floor_changed,length=260, bg="#0b1321", fg="#e8edf7", highlightthickness=0).pack(side="left")
    def open_settings():
        from settings_dialog import show_settings
        show_settings(root,receiver,release_ptt)
    settings_button=tk.Button(controls,name="settings",text="Einstellungen",command=open_settings,state="disabled")
    settings_button.pack(side="left",padx=6)
    tuning = tk.Frame(root, bg="#0b1321")
    tuning.pack()
    tk.Label(tuning, text="RX1 MHz (USB / Audio)", bg="#0b1321", fg="#e8edf7").pack(side="left")
    frequency = tk.StringVar(value=args.rx_frequency)
    entry = tk.Entry(tuning, name="rx1_frequency", textvariable=frequency, width=20)
    entry.pack(side="left", padx=8)
    def apply_tuning():
        try:
            receiver.set_frequency(parse_frequency(frequency.get()))
            update_offset_label()
        except (ValueError, ArithmeticError):
            receiver.tune_error = "Invalid RX frequency"
    entry.bind("<Return>", lambda event: apply_tuning())
    rx1_button=tk.Button(tuning, text="Tune", command=apply_tuning)
    rx1_button.pack(side="left")
    if player:
        gain = tk.DoubleVar(value=4)
        player.volume = 4
        def gain_changed(value):
            player.volume=float(value)
            if not ui_settings_syncing and receiver.loop_mode!=LOOP_AUDIO:
                receiver.settings.save_preferences(gui_audio_gain=float(value))
        tk.Label(tuning, text="Audio gain", bg="#0b1321", fg="#e8edf7").pack(side="left", padx=8)
        tk.Scale(tuning, from_=0, to=20, resolution=.1, orient="horizontal", variable=gain, length=200,
                 name="audio_gain",command=gain_changed, bg="#0b1321", fg="#e8edf7").pack(side="left")
    filter_controls = tk.Frame(root, bg="#0b1321")
    filter_controls.pack(pady=3)
    filter_bw = tk.IntVar(value=3900)
    filter_center = tk.IntVar(value=2050)
    filter_syncing = False
    filter_ready = False
    filter_after = None
    last_filter_applied = None
    def constrain_filter():
        nonlocal filter_syncing
        filter_syncing = True
        width = int(filter_bw.get())
        minimum = (200+width+1)//2
        maximum = (8000-width)//2
        center_scale.configure(from_=minimum, to=maximum, state="normal" if filter_ready and minimum<maximum else "disabled")
        filter_center.set(max(minimum, min(maximum, int(filter_center.get()))))
        filter_syncing = False
    def send_filter():
        nonlocal filter_after
        filter_after = None
        if filter_ready:
            receiver.set_filter(int(filter_bw.get()), int(filter_center.get()))
    def filter_changed(value=None):
        nonlocal filter_after
        if filter_syncing or not filter_ready: return
        constrain_filter()
        # Throttle to at most 20 updates/s while dragging, retaining the latest values.
        if filter_after is None: filter_after = root.after(50, send_filter)
    tk.Label(filter_controls, text="RX1 BW (Hz)", bg="#0b1321", fg="#e8edf7").pack(side="left", padx=5)
    bw_scale = tk.Scale(filter_controls, name="rx1_bandwidth", from_=500, to=3900, resolution=1,
                        orient="horizontal", variable=filter_bw, command=filter_changed, length=300,
                        bg="#0b1321", fg="#e8edf7", highlightthickness=0, state="disabled")
    bw_scale.pack(side="left")
    tk.Label(filter_controls, text="Mitte / Lage (Audio Hz)", bg="#0b1321", fg="#e8edf7").pack(side="left", padx=5)
    center_scale = tk.Scale(filter_controls, name="rx1_filter_center", from_=350, to=3750, resolution=1,
                            orient="horizontal", variable=filter_center, command=filter_changed, length=300,
                            bg="#0b1321", fg="#e8edf7", highlightthickness=0, state="disabled")
    center_scale.pack(side="left")
    presets = tk.Frame(root, bg="#0b1321")
    presets.pack()
    def preset_filter(width):
        filter_bw.set(width)
        filter_changed()
    preset_buttons = []
    for label, width in (("500 Hz",500),("1.2 kHz",1200),("1.8 kHz",1800),("2.7 kHz",2700),("Full BW",3900)):
        button = tk.Button(presets, text=label, command=lambda width=width: preset_filter(width), state="disabled")
        button.pack(side="left", padx=3)
        preset_buttons.append(button)
    filter_monitor = tk.Label(root, text="RX1 Filter: wartet auf Backend", bg="#0b1321", fg="#a4b5cd", anchor="w")
    filter_monitor.pack(fill="x", padx=12)
    # Share the preset row to leave both waterfalls visible on a 1080-line display.
    nr_enabled=tk.BooleanVar(value=False)
    nr_strength=tk.IntVar(value=60)
    nr_ready=nr_syncing=False
    nr_after=None
    last_nr_applied=None
    def send_nr():
        nonlocal nr_after
        nr_after=None
        if nr_ready: receiver.set_nr(nr_strength.get() if nr_enabled.get() else 0)
    def nr_changed(value=None):
        nonlocal nr_after
        if nr_syncing or not nr_ready: return
        if not ui_settings_syncing and nr_strength.get()>0:
            receiver.settings.save_preferences(gui_nr_strength=nr_strength.get())
        if nr_after is None: nr_after=root.after(50,send_nr)
    nr_switch=tk.Checkbutton(presets,name="rx1_nr_enabled",text="RX1 NR",variable=nr_enabled,
                            command=nr_changed,bg="#0b1321",fg="#e8edf7",selectcolor="#243348",state="disabled")
    nr_switch.pack(side="left",padx=(20,3))
    nr_scale=tk.Scale(presets,name="rx1_nr_strength",from_=0,to=100,resolution=1,orient="horizontal",
                      variable=nr_strength,command=nr_changed,length=230,bg="#0b1321",fg="#e8edf7",
                      highlightthickness=0,state="disabled")
    nr_scale.pack(side="left")
    nr_monitor=tk.Label(presets,text="NR wartet…",bg="#0b1321",fg="#a4b5cd")
    nr_monitor.pack(side="left",padx=5)
    tuning2 = tk.Frame(root, bg="#0b1321")
    tuning2.pack(pady=3)
    tk.Label(tuning2, text="RX2 MHz (Digital-Test)", bg="#0b1321", fg="#e8edf7").pack(side="left")
    frequency2 = tk.StringVar(value=args.rx2_frequency)
    entry2 = tk.Entry(tuning2, textvariable=frequency2, width=20)
    entry2.pack(side="left", padx=8)
    def apply_tuning2():
        try:
            receiver.set_frequency(parse_frequency(frequency2.get()), stream_id=2)
        except (ValueError, ArithmeticError):
            receiver.channels[2].tune_error = "Invalid RX2 frequency"
    entry2.bind("<Return>", lambda event: apply_tuning2())
    tk.Button(tuning2, text="Tune RX2", command=apply_tuning2).pack(side="left")
    monitor2 = tk.Label(root, text="RX2: waiting for 12 kHz audio", bg="#0b1321", fg="#a4b5cd", anchor="w")
    monitor2.pack(fill="x", padx=12)
    from audio import Microphone
    microphone = None
    tx_controls = tk.Frame(root, bg="#0b1321")
    tx_controls.pack(pady=3)
    tk.Label(tx_controls, text="TX MHz (USB)", bg="#0b1321", fg="#e8edf7").pack(side="left")
    tx_frequency = tk.StringVar(value=args.tx_frequency)
    tx_entry = tk.Entry(tx_controls, name="tx_frequency", textvariable=tx_frequency, width=20)
    tx_entry.pack(side="left", padx=8)
    def apply_tx():
        try:
            receiver.set_tx(frequency=parse_frequency(tx_frequency.get()))
            update_offset_label()
        except (ValueError, ArithmeticError): receiver.tx_error = "Invalid TX frequency"
    tx_entry.bind("<Return>", lambda event: apply_tx())
    tk.Button(tx_controls, text="Tune TX", command=apply_tx).pack(side="left")
    def release_ptt(event=None):
        nonlocal microphone
        receiver.set_tx(enabled=False)
        if microphone:
            microphone.close()
            microphone = None
        if player and receiver.loop_mode==LOOP_AUDIO: player.reset()
        ptt.configure(text="Audio testen halten" if receiver.loop_mode==LOOP_AUDIO else "PTT halten — Mikrofon senden", bg="#d9d9d9")
    def press_ptt(event=None):
        nonlocal microphone
        if microphone is not None: return
        if receiver.loop_status==3 or receiver.loop_mode is None: return
        try: hz = parse_frequency(tx_frequency.get())
        except (ValueError, ArithmeticError):
            receiver.tx_error = "Invalid TX frequency"
            return
        receiver.set_tx(frequency=hz, enabled=True)
        update_offset_label()
        microphone = Microphone(receiver.send_microphone)
        ptt.configure(text="AUDIO-LOOP — zum Stoppen loslassen" if receiver.loop_mode==LOOP_AUDIO else "SENDEN — zum Stoppen loslassen",
                      bg="#54b9b0" if receiver.loop_mode==LOOP_AUDIO else "#f07060")
    ptt = tk.Button(tx_controls, name="ptt", text="PTT halten — Mikrofon senden")
    ptt.pack(side="left", padx=12)
    ptt.bind("<ButtonPress-1>", press_ptt)
    offset_label = tk.Label(tx_controls, name="tx_offset", text="", bg="#0b1321", fg="#ffb75c")
    offset_label.pack(side="left",padx=5)
    def update_offset_label():
        if receiver.channels[1].command[1] is None or receiver.tx_command[1] is None:
            offset_label.configure(text="Einstellungen werden geladen …");return
        if receiver.loop_mode==LOOP_RF:
            offset_label.configure(text="HF-Loop: RX1 folgt TX")
            return
        try:
            offset=tx_offset_hz(receiver.channels[1].command[1],receiver.tx_command[1])
            offset_label.configure(text=f"TX-Offset {offset:+d} Hz | Rad {wheel_step_hz} Hz")
        except (ValueError,ArithmeticError):
            offset_label.configure(text="TX-Offset: Frequenzeingabe prüfen")
    update_offset_label()
    loop_choice=tk.IntVar(value=0)
    loop_previous=None
    loop_previous_generation=None
    loop_saved_gain=None
    def select_loop():
        release_ptt()
        if player: player.reset()
        receiver.set_loop(loop_choice.get())
    tk.Label(controls,text="Test:",bg="#0b1321",fg="#e8edf7").pack(side="left",padx=6)
    loop_buttons=[]
    for text,mode in (("Normal",0),("Audio-Loop",1),("HF-Loop",2)):
        button=tk.Radiobutton(controls,name=f"test_loop_{mode}",text=text,variable=loop_choice,value=mode,
                              command=select_loop,bg="#0b1321",fg="#e8edf7",selectcolor="#243348",state="disabled")
        button.pack(side="left");loop_buttons.append(button)
    loop_monitor=tk.Label(controls,text="",bg="#0b1321",fg="#ffb75c")
    loop_monitor.pack(side="left",padx=5)
    root.bind("<ButtonRelease-1>", lambda event: release_ptt() if microphone else None)
    root.bind("<FocusOut>", lambda event: release_ptt() if event.widget is root and microphone else None)
    root.bind("<Escape>", release_ptt)
    tx_monitor = tk.Label(root, text="TX: PTT aus", bg="#0b1321", fg="#a4b5cd", anchor="w")
    tx_monitor.pack(fill="x", padx=12)
    compressor_controls = tk.Frame(root, bg="#0b1321")
    compressor_controls.pack()
    compressor_amount = tk.DoubleVar(value=100)
    compressor_ready = compressor_syncing = False
    compressor_after = None
    last_compressor_applied = None
    def send_compressor():
        nonlocal compressor_after
        compressor_after = None
        if compressor_ready:
            receiver.set_compressor(round(compressor_amount.get()*10))
    def compressor_changed(value=None):
        nonlocal compressor_after
        if compressor_syncing or not compressor_ready: return
        # Programmatic display of an API setting below the slider range must
        # not silently overwrite that setting with the nearest slider value.
        if (receiver.compressor_applied is not None and receiver.compressor_acked==receiver.compressor_command[0]
            and round(compressor_amount.get()*10)==max(600,min(1400,receiver.compressor_applied))): return
        if compressor_after is None: compressor_after = root.after(50, send_compressor)
    def normal_compressor():
        compressor_amount.set(100)
        compressor_changed()
    tk.Label(compressor_controls, text="TX1 Sprachkompressor (%)", bg="#0b1321", fg="#e8edf7").pack(side="left", padx=5)
    compressor_scale = tk.Scale(compressor_controls, name="tx1_compressor", from_=60, to=140, resolution=.1,
                               orient="horizontal", variable=compressor_amount, command=compressor_changed,
                               length=350, bg="#0b1321", fg="#e8edf7", highlightthickness=0, state="disabled")
    compressor_scale.pack(side="left")
    compressor_normal = tk.Button(compressor_controls, text="100 % Normal", command=normal_compressor, state="disabled")
    compressor_normal.pack(side="left", padx=8)
    compressor_monitor = tk.Label(root, text="TX1 Kompressor: wartet auf Backend", bg="#0b1321", fg="#a4b5cd", anchor="w")
    compressor_monitor.pack(fill="x", padx=12)
    canvas = tk.Label(root, name="wide_waterfall", bg="#0b1321", text="Waiting for spectrum data…", fg="#a4b5cd", cursor="crosshair")
    canvas.pack(fill="both", expand=True)
    zoom_canvas = tk.Label(root, name="zoom_waterfall", bg="#0b1321", text="Waiting for zoom…", fg="#a4b5cd", cursor="crosshair")
    zoom_canvas.pack(fill="both", expand=True)
    def valid_mouse_tuning(rx_hz,tx_hz):
        if not history: return False
        center=history[-1].center_hz
        if abs(rx_hz-center)>275000 or abs(tx_hz-uplink_hz(center))>275000:
            receiver.tune_error="Abstimmung außerhalb des Fensters: 25 kHz Randreserve erforderlich"
            return False
        return True
    def waterfall_click(event):
        if receiver.loop_mode==LOOP_RF or receiver.loop_status==3: return
        geometry=getattr(event.widget,'plot_geometry',None)
        if geometry is None: return
        hz=geometry.widget_frequency_at(event.x,event.y,event.widget.winfo_width(),event.widget.winfo_height())
        if hz is None: return
        try:
            offset=tx_offset_hz(receiver.channels[1].command[1],receiver.tx_command[1])
            tx_hz=uplink_hz(hz,offset)
        except (ValueError,ArithmeticError):
            receiver.tune_error="Frequenzeingaben vor der Mausabstimmung prüfen"
            return
        if not valid_mouse_tuning(hz,tx_hz): return
        frequency.set(mhz_text(hz));tx_frequency.set(mhz_text(tx_hz))
        receiver.set_frequency(hz)
        receiver.set_tx(frequency=tx_hz)
        update_offset_label()
    def waterfall_wheel(event):
        geometry=getattr(event.widget,'plot_geometry',None)
        if geometry is None: return "break"
        if geometry.widget_frequency_at(event.x,event.y,event.widget.winfo_width(),event.widget.winfo_height()) is None:
            return "break"
        steps=wheel_steps(event)
        if not steps: return "break"
        try:
            rx_hz=receiver.channels[1].command[1]
            tx_hz=receiver.tx_command[1]+wheel_step_hz*steps
        except (ValueError,ArithmeticError):
            receiver.tx_error="Frequenzeingaben vor der Mausabstimmung prüfen"
            return "break"
        if valid_mouse_tuning(rx_hz,tx_hz):
            tx_frequency.set(mhz_text(tx_hz))
            receiver.set_tx(frequency=tx_hz)
            update_offset_label()
        return "break"
    for widget in (canvas,zoom_canvas):
        widget.bind("<Button-1>",waterfall_click)
        for event in ("<Button-4>","<Button-5>","<MouseWheel>"):
            widget.bind(event,waterfall_wheel)
    status = tk.Label(root, text="", bg="#0b1321", fg="#a4b5cd", anchor="w")
    status.pack(fill="x", padx=12, pady=6)
    last_frame = 0
    last_floor = floor.get()
    tick_timer=close_timer=None

    def tick():
        nonlocal tick_timer
        nonlocal last_frame, last_floor, filter_ready, filter_syncing, last_filter_applied
        nonlocal compressor_ready, compressor_syncing, last_compressor_applied
        nonlocal nr_ready,nr_syncing,last_nr_applied
        nonlocal loop_previous,loop_previous_generation,loop_saved_gain
        nonlocal ui_settings_generation,ui_settings_syncing,wheel_step_hz
        settings_values=receiver.settings.values
        if settings_values is not None and receiver.settings_generation!=ui_settings_generation:
            ui_settings_syncing=True
            frequency.set(mhz_text(receiver.channels[1].command[1]))
            frequency2.set(mhz_text(receiver.channels[2].command[1]))
            tx_frequency.set(mhz_text(receiver.tx_command[1]))
            floor.set(float(settings_values['gui_display_floor']))
            if player: gain.set(float(settings_values['gui_audio_gain']));player.volume=gain.get()
            nr_strength.set(int(settings_values['gui_nr_strength']))
            ui_settings_generation=receiver.settings_generation
            ui_settings_syncing=False
            if getattr(args,'wheel_step_hz',None) is not None:
                receiver.settings.save_preferences(gui_wheel_step_hz=args.wheel_step_hz)
                args.wheel_step_hz=None
        if settings_values is not None:
            wheel_step_hz=int(settings_values['gui_wheel_step_hz'])
            update_offset_label()
        settings_button.configure(state="normal" if settings_values is not None and not receiver.settings.pending else "disabled")
        mode=receiver.loop_mode
        if mode is not None and (mode!=loop_previous or receiver.loop_generation!=loop_previous_generation):
            if player:
                player.reset()
                if mode==LOOP_AUDIO and loop_previous!=LOOP_AUDIO:
                    loop_saved_gain=gain.get();gain.set(1);player.volume=1
                elif mode!=LOOP_AUDIO and loop_previous==LOOP_AUDIO and loop_saved_gain is not None:
                    gain.set(loop_saved_gain);player.volume=loop_saved_gain;loop_saved_gain=None
            if loop_previous_generation is not None and receiver.loop_generation!=loop_previous_generation:
                history.clear();zoom_history.clear()
            loop_previous=mode
            loop_previous_generation=receiver.loop_generation
            if mode!=LOOP_RF: frequency.set(mhz_text(receiver.channels[1].command[1]))
            update_offset_label()
        if mode==LOOP_RF and receiver.applied_hz is not None: frequency.set(mhz_text(receiver.applied_hz))
        pending=receiver.loop_acked!=receiver.loop_command[0]
        if mode is not None:
            if not pending: loop_choice.set(mode)
            for value,button in enumerate(loop_buttons): button.configure(state="normal" if not pending or value==0 else "disabled")
            lo=f"RX-LO {receiver.loop_rx_lo/1e6:.3f} MHz" if receiver.loop_rx_lo is not None else ""
            loop_monitor.configure(text=("Umschaltung…" if receiver.loop_status==3 else receiver.loop_error or lo))
        locked=mode==LOOP_RF or receiver.loop_status==3
        entry.configure(state="disabled" if locked else "normal")
        rx1_button.configure(state="disabled" if locked else "normal")
        ptt.configure(state="disabled" if mode is None or pending or receiver.settings.flags&6 else "normal")
        if microphone is None:
            ptt.configure(text="Audio testen halten" if mode==LOOP_AUDIO else "PTT halten — Mikrofon senden")
        changed = False
        try:
            while True:
                frame = receiver.frames.get_nowait()
                if history and (frame.center_hz, frame.fft_size, frame.session_id) != (history[-1].center_hz, history[-1].fft_size, history[-1].session_id):
                    history.clear()
                history.append(frame)
                history[:] = history[-220:]
                last_frame = time.monotonic()
                changed = True
        except queue.Empty:
            pass
        if history and (changed or floor.get() != last_floor):
            last_floor = floor.get()
            rendered = draw_image(history, height=265, floor=last_floor,rx_hz=receiver.applied_hz,
                                  tx_downlink_hz=receiver.tx_applied_hz+DOWNLINK_UPLINK_HZ if receiver.tx_applied_hz is not None else None)
            photo = ImageTk.PhotoImage(rendered)
            canvas.configure(image=photo, text="")
            canvas.image = photo
            canvas.plot_geometry=PlotGeometry(history[-1].center_hz,history[-1].sample_rate,rendered.width,rendered.height)
            if args.screenshot:
                rendered.save(args.screenshot)
        zoom_changed = False
        try:
            while True:
                frame = receiver.zoom_frames.get_nowait()
                if receiver.applied_hz is not None and frame.center_hz != receiver.applied_hz:
                    continue
                if zoom_history and (frame.center_hz,frame.session_id)!=(zoom_history[-1].center_hz,zoom_history[-1].session_id):
                    zoom_history.clear()
                zoom_history.append(frame); zoom_history[:] = zoom_history[-220:]
                zoom_changed = True
        except queue.Empty:
            pass
        if zoom_history and (zoom_changed or changed or floor.get() != last_floor):
            zoom_rendered = draw_image(zoom_history,height=265,floor=floor.get(),span_hz=50000,rx_hz=receiver.applied_hz,
                                      tx_downlink_hz=receiver.tx_applied_hz+DOWNLINK_UPLINK_HZ if receiver.tx_applied_hz is not None else None)
            photo = ImageTk.PhotoImage(zoom_rendered)
            zoom_canvas.configure(image=photo,text="")
            zoom_canvas.image = photo
            zoom_canvas.plot_geometry=PlotGeometry(zoom_history[-1].center_hz,50000,zoom_rendered.width,zoom_rendered.height)
            if args.screenshot and history:
                from PIL import Image
                from PIL import ImageDraw
                combined = Image.new("RGB",(1100,570),"#0b1321")
                combined.paste(draw_image(history,height=265,floor=floor.get()),(0,0))
                combined.paste(zoom_rendered,(0,265))
                rx2 = receiver.channels[2]
                ImageDraw.Draw(combined).text((20,542),
                    f"RX2 monitor | {rx2.applied_hz} Hz | 12 kHz PCM mono | {rx2.received} packets | {rx2.samples} samples | RMS {rx2.rms:.5f} | no soundcard output", fill="#a4b5cd")
                combined.save(args.screenshot)
        applied = receiver.filter_applied
        if applied is not None and applied != last_filter_applied and filter_after is None and receiver.filter_acked == receiver.filter_command[0]:
            filter_syncing = True
            filter_bw.set(applied[0]); filter_center.set(applied[1])
            filter_syncing = False
            filter_ready = True
            bw_scale.configure(state="normal")
            for button in preset_buttons: button.configure(state="normal")
            constrain_filter()
            last_filter_applied = applied
        if applied is not None:
            width, center = applied
            pending = filter_after is not None or receiver.filter_acked != receiver.filter_command[0]
            filter_monitor.configure(text=f"RX1 Filter: {center-width/2:g}–{center+width/2:g} Hz | BW {width} Hz | Mitte {center} Hz"
                + (" | wird übernommen…" if pending else " | bestätigt") + (f" | {receiver.filter_error}" if receiver.filter_error else ""))
        nr=receiver.nr_applied
        if nr is not None and nr!=last_nr_applied and nr_after is None and receiver.nr_acked==receiver.nr_command[0]:
            nr_syncing=True
            nr_enabled.set(nr>0)
            if nr: nr_strength.set(nr)
            nr_syncing=False
            nr_ready=True
            nr_switch.configure(state="normal");nr_scale.configure(state="normal")
            last_nr_applied=nr
        if nr is not None:
            pending=nr_after is not None or receiver.nr_acked!=receiver.nr_command[0]
            nr_monitor.configure(text=receiver.nr_error or ("NR aus" if nr==0 else f"NR {nr} % / bis {int(np.floor(.3*nr+.5))} dB")
                                 + (" …" if pending else " ✓"))
        amount = receiver.compressor_applied
        if amount is not None and amount != last_compressor_applied and compressor_after is None and receiver.compressor_acked == receiver.compressor_command[0]:
            compressor_syncing = True
            compressor_amount.set(max(60,min(140,amount/10)))
            compressor_syncing = False
            compressor_ready = True
            compressor_scale.configure(state="normal")
            compressor_normal.configure(state="normal")
            last_compressor_applied = amount
        if amount is not None:
            pending = compressor_after is not None or receiver.compressor_acked != receiver.compressor_command[0]
            label = f"{amount/10:g} %" if amount else "aus"
            compressor_monitor.configure(text=f"TX1 Kompressor: {label} | Anhebung bis {18*amount/1000:.1f} dB"
                + f" | Verdichtung {receiver.compression_db:.1f} dB | Spitzenbegrenzung {receiver.limiting_db:.1f} dB"
                + (" | wird übernommen…" if pending else " | bestätigt")
                + (f" | {receiver.compressor_error}" if receiver.compressor_error else ""))
        if microphone and (microphone.error or not receiver.tx_command[2]):
            if microphone.error: receiver.tx_error = microphone.error
            release_ptt()
        tx_label="Audio-Test" if mode==LOOP_AUDIO else "TX"
        tx_monitor.configure(text=f"{tx_label} {'AKTIV' if receiver.tx_confirmed else 'PTT aus / wartet'} | {receiver.tx_applied_hz} Hz | Mic 48 → 12 kHz | UDP {receiver.tx_packets} packets | Echo {receiver.echo_received} | FIFO {receiver.tx_fill}/2880 | Pluto {receiver.tx_pluto_fill}/8 | {receiver.tx_ppm} ppm | {receiver.tx_error}")
        second = receiver.channels[2]
        # Monitoring only: discard already-counted RX2 test packets; never send to AudioPlayer.
        while True:
            try: receiver.audio2_frames.get_nowait()
            except queue.Empty: break
        rms_db = 20*np.log10(max(second.rms, 1e-8))
        age2 = time.monotonic()-second.last_audio
        state2 = "LIVE" if age2 < 1 else "WAITING"
        monitor2.configure(text=f"RX2 {state2} | confirmed {second.applied_hz} Hz | 12 kHz mono | packets {second.received} | samples {second.samples} | RMS {rms_db:.1f} dBFS | peak {second.peak:.4f} | {second.tune_error}")
        age = time.monotonic() - last_frame if last_frame else float("inf")
        state = "LIVE" if age < 1 else "WAITING / STALE"
        status.configure(text=f"{state} | Frames {receiver.received} | Incomplete {receiver.assembler.incomplete_frames} | Invalid {receiver.assembler.invalid_packets} | RX {receiver.applied_hz} Hz | Audio {receiver.audio_received} packets | {receiver.error} {receiver.tune_error}" +
                         (f" | Played {player.written} | {player.ppm:.0f} ppm | {player.error}" if player else " | Muted"))
        tick_timer=root.after(50, tick)

    def close():
        if tick_timer is not None: root.after_cancel(tick_timer)
        if close_timer is not None: root.after_cancel(close_timer)
        if filter_after is not None: root.after_cancel(filter_after)
        if compressor_after is not None: root.after_cancel(compressor_after)
        if nr_after is not None: root.after_cancel(nr_after)
        release_ptt()
        receiver.close()
        if player:
            player.close()
        if args.audio_report:
            report = {"audio_packets":receiver.audio_received,"applied_hz":receiver.applied_hz,
                      "zoom_frames":len(zoom_history),"played_samples":player.written if player else 0,
                      "audio_error":player.error if player else "muted", "underruns":player.underruns if player else 0,
                      "rx2_packets":receiver.channels[2].received,"rx2_samples":receiver.channels[2].samples,
                      "rx2_applied_hz":receiver.channels[2].applied_hz,"rx2_rms":receiver.channels[2].rms,
                      "invalid_audio":receiver.invalid_audio,
                      "tx_packets":receiver.tx_packets,"tx_applied_hz":receiver.tx_applied_hz,
                      "tx_audio_fill":receiver.tx_fill,"tx_pluto_fill":receiver.tx_pluto_fill,
                      "tx_ppm":receiver.tx_ppm,"tx_error":receiver.tx_error,
                      "rx1_filter":receiver.filter_applied,"rx1_filter_error":receiver.filter_error,
                      "rx1_nr_percent":receiver.nr_applied,"rx1_nr_error":receiver.nr_error,
                      "tx1_compressor":receiver.compressor_applied,"tx1_compressor_error":receiver.compressor_error,
                      "compression_db":receiver.compression_db,"limiting_db":receiver.limiting_db}
            report['tx_offset_hz']=tx_offset_hz(receiver.channels[1].command[1],receiver.tx_command[1])
            report.update(test_loop=receiver.loop_mode,loop_error=receiver.loop_error,loop_rx_lo=receiver.loop_rx_lo,
                          echo_packets=receiver.echo_received,echo_samples=receiver.echo_samples)
            args.audio_report.write_text(json.dumps(report,indent=2)+"\n")
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    if args.seconds:
        close_timer=root.after(int(args.seconds * 1000), close)
    tick()
    root.mainloop()
    return 0


def parse_frequency(text):
    if text is None: return None
    value = Decimal(text.replace(",",".")) * 1000000
    if not value.is_finite() or value != value.to_integral_value() or value <= 0 or value > 10**12:
        raise ValueError("Frequency must be positive MHz with at most six decimal places")
    return int(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rx-frequency", help="Override backend RX1 frequency in MHz")
    parser.add_argument("--tx-frequency", help="Override backend TX frequency in MHz")
    parser.add_argument("--rx2-frequency", help="Override backend RX2 frequency in MHz")
    parser.add_argument("--no-audio", action="store_true", help="Disable soundcard playback")
    parser.add_argument("--wheel-step-hz",type=int,help="Override stored TX mouse-wheel step (Hz)")
    parser.add_argument("--audio-report", type=Path, help="Write playback test statistics on GUI close")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=40900)
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--timeout", type=float, default=20)
    parser.add_argument("--seconds", type=float, default=0, help="Close the GUI after N seconds (visual smoke test)")
    parser.add_argument("--capture", type=Path, help="Save last headless frame as .npz")
    parser.add_argument("--screenshot", type=Path, help="Save a rendered spectrum/waterfall PNG")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535 or args.frames < 1 or args.timeout <= 0:
        parser.error("Invalid port, frame count or timeout")
    if args.wheel_step_hz is not None and not 1<=args.wheel_step_hz<=1000:
        parser.error("wheel-step-hz must be 1..1000")
    try:
        return headless(args) if args.headless else gui(args)
    except (OSError, RuntimeError, ValueError, ArithmeticError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
