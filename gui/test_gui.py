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
from decimal import Decimal


class Receiver:
    def __init__(self, host, port, rx_hz=10489748500, audio_sink=None):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        self.sock.connect((host, port))
        self.sock.settimeout(0.2)
        self.assembler = Assembler()
        self.zoom_assembler = Assembler(1)
        self.zoom_frames = queue.Queue(maxsize=4)
        self.audio_frames = queue.Queue(maxsize=30)
        self.audio_sink = audio_sink
        self.audio_received = 0
        self.command = (1, rx_hz)
        self.applied_hz = None
        self.audio_identity = None
        self.tune_error = ""
        self.frames = queue.Queue(maxsize=4)
        self.received = 0
        self.error = "Waiting for backend"
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        next_subscription = 0
        last_command = None
        while not self.stop.is_set():
            if time.monotonic() >= next_subscription or self.command != last_command:
                try:
                    self.sock.send(subscription())
                    self.sock.send(tune(*self.command))
                    last_command = self.command
                except OSError as exc:
                    self.error = str(exc)
                next_subscription = time.monotonic() + 2
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
                if packet[5] == STATE:
                    if fields[5] == self.command[0]:
                        self.tune_error = "RX frequency outside capture window" if fields[13] else ""
                        if not fields[13]:
                            self.applied_hz = fields[8]
                            self.audio_identity = (fields[15], fields[7])
                    continue
                if packet[5] == AUDIO:
                    audio = audio_packet(packet)
                    if audio is not None and (audio.session_id, audio.epoch) == self.audio_identity:
                        self.audio_received += 1
                        if self.audio_sink:
                            self.audio_sink(audio)
                        else:
                            self.offer(self.audio_frames, audio)
                    continue
                if fields[4] == 1:
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
            try:
                target.get_nowait()
            except queue.Empty:
                pass
        target.put_nowait(item)

    def set_frequency(self, hz):
        self.command = ((self.command[0] + 1) & 0xffffffff, hz)

    def close(self):
        self.stop.set()
        self.thread.join(timeout=1)
        self.sock.close()


def save_capture(path, frame):
    np.savez_compressed(path, db=frame.db, center_hz=frame.center_hz,
                        sample_rate=frame.sample_rate, fft_size=frame.fft_size,
                        sample_index=frame.sample_index, timestamp_ns=frame.timestamp_ns)


def headless(args):
    receiver = Receiver(args.host, args.port, parse_frequency(args.rx_frequency))
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


def draw_image(frames, width=1100, height=650, floor=-100, ceiling=0, span_hz=None):
    from PIL import Image, ImageDraw
    image = Image.new("RGB", (width, height), "#0b1321")
    draw = ImageDraw.Draw(image)
    frame = frames[-1]
    x0, x1, y0, y1 = 70, width - 20, 60, int(height*.45)
    waterfall_y = int(height*.55)
    span = span_hz or frame.sample_rate
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
    root.title("QO100_TRX2 — USB RX Test")
    root.geometry("1120x900")
    root.configure(bg="#0b1321")
    from audio import AudioPlayer
    player = None if args.no_audio else AudioPlayer()
    receiver = Receiver(args.host, args.port, parse_frequency(args.rx_frequency), player.add if player else None)
    history, zoom_history = [], []
    title = tk.Label(root, text=f"QO100_TRX2 — {args.host}:{args.port}", bg="#0b1321", fg="#e8edf7", font=("sans", 15))
    title.pack(pady=8)
    controls = tk.Frame(root, bg="#0b1321")
    controls.pack()
    floor = tk.DoubleVar(value=-100)
    tk.Label(controls, text="Display floor (dBFS)", bg="#0b1321", fg="#a4b5cd").pack(side="left")
    tk.Scale(controls, from_=-160, to=-40, resolution=5, orient="horizontal", variable=floor,
             length=300, bg="#0b1321", fg="#e8edf7", highlightthickness=0).pack(side="left")
    tuning = tk.Frame(root, bg="#0b1321")
    tuning.pack()
    tk.Label(tuning, text="RX MHz (USB)", bg="#0b1321", fg="#e8edf7").pack(side="left")
    frequency = tk.StringVar(value=args.rx_frequency)
    entry = tk.Entry(tuning, textvariable=frequency, width=20)
    entry.pack(side="left", padx=8)
    def apply_tuning():
        try:
            receiver.set_frequency(parse_frequency(frequency.get()))
        except (ValueError, ArithmeticError):
            receiver.tune_error = "Invalid RX frequency"
    entry.bind("<Return>", lambda event: apply_tuning())
    tk.Button(tuning, text="Tune", command=apply_tuning).pack(side="left")
    if player:
        gain = tk.DoubleVar(value=4)
        player.volume = 4
        tk.Label(tuning, text="Audio gain", bg="#0b1321", fg="#e8edf7").pack(side="left", padx=8)
        tk.Scale(tuning, from_=0, to=20, orient="horizontal", variable=gain, length=200,
                 command=lambda value: setattr(player,"volume",float(value)), bg="#0b1321", fg="#e8edf7").pack(side="left")
    canvas = tk.Label(root, bg="#0b1321", text="Waiting for spectrum data…", fg="#a4b5cd")
    canvas.pack(fill="both", expand=True)
    zoom_canvas = tk.Label(root, bg="#0b1321", text="Waiting for zoom…", fg="#a4b5cd")
    zoom_canvas.pack(fill="both", expand=True)
    status = tk.Label(root, text="", bg="#0b1321", fg="#a4b5cd", anchor="w")
    status.pack(fill="x", padx=12, pady=6)
    last_frame = 0
    last_floor = floor.get()

    def tick():
        nonlocal last_frame, last_floor
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
            rendered = draw_image(history, height=350, floor=last_floor)
            photo = ImageTk.PhotoImage(rendered)
            canvas.configure(image=photo, text="")
            canvas.image = photo
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
            zoom_rendered = draw_image(zoom_history,height=350,floor=floor.get(),span_hz=50000)
            photo = ImageTk.PhotoImage(zoom_rendered)
            zoom_canvas.configure(image=photo,text="")
            zoom_canvas.image = photo
            if args.screenshot and history:
                from PIL import Image
                combined = Image.new("RGB",(1100,700))
                combined.paste(draw_image(history,height=350,floor=floor.get()),(0,0))
                combined.paste(zoom_rendered,(0,350)); combined.save(args.screenshot)
        age = time.monotonic() - last_frame if last_frame else float("inf")
        state = "LIVE" if age < 1 else "WAITING / STALE"
        status.configure(text=f"{state} | Frames {receiver.received} | Incomplete {receiver.assembler.incomplete_frames} | Invalid {receiver.assembler.invalid_packets} | RX {receiver.applied_hz} Hz | Audio {receiver.audio_received} packets | {receiver.error} {receiver.tune_error}" +
                         (f" | Played {player.written} | {player.ppm:.0f} ppm | {player.error}" if player else " | Muted"))
        root.after(50, tick)

    def close():
        receiver.close()
        if player:
            player.close()
        if args.audio_report:
            report = {"audio_packets":receiver.audio_received,"applied_hz":receiver.applied_hz,
                      "zoom_frames":len(zoom_history),"played_samples":player.written if player else 0,
                      "audio_error":player.error if player else "muted", "underruns":player.underruns if player else 0}
            args.audio_report.write_text(json.dumps(report,indent=2)+"\n")
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    if args.seconds:
        root.after(int(args.seconds * 1000), close)
    tick()
    root.mainloop()
    return 0


def parse_frequency(text):
    value = Decimal(text.replace(",",".")) * 1000000
    if not value.is_finite() or value != value.to_integral_value() or value <= 0 or value > 10**12:
        raise ValueError("Frequency must be positive MHz with at most six decimal places")
    return int(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rx-frequency", default="10489.748500", help="USB carrier frequency in MHz")
    parser.add_argument("--no-audio", action="store_true", help="Disable soundcard playback")
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
    try:
        return headless(args) if args.headless else gui(args)
    except (OSError, RuntimeError, ValueError, ArithmeticError) as exc:
        parser.exit(1, f"Error: {exc}\n")


if __name__ == "__main__":
    raise SystemExit(main())
