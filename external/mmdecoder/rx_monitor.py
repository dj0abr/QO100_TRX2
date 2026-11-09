#!/usr/bin/python3
"""QO-100 HS-Modem RX probe: PulseAudio capture, diagnostics and Tk window.

The decoder's stdout is JSON Lines; this adapter never opens an audio output.
"""
from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime
import json
import math
from pathlib import Path
import signal
import subprocess
import threading
import time
import sqlite3

import numpy as np
from data_collector import Collector, TextAPI, replay_logs

ROOT = Path(__file__).resolve().parent
RATE = 48000
BLOCK = 4800


def spectrum(samples):
    """Hann-windowed amplitude spectrum and a tentative 20..900 Hz pilot."""
    n = min(32768, len(samples))
    if n < 4096:
        return np.array([]), np.array([]), None
    x = np.asarray(samples[-96000:], dtype=float)
    window = np.hanning(n)
    power = np.mean([np.abs(np.fft.rfft(x[start:start+n] * window)) ** 2
                     for start in range(0, len(x)-n+1, n//2)], axis=0)
    db = 10 * np.log10(np.maximum(power * (2 / window.sum()) ** 2, 1e-20))
    freq = np.fft.rfftfreq(n, 1 / RATE)
    indices = np.flatnonzero((freq >= 20) & (freq <= 900))
    # Rank narrow peaks by prominence over their local floor. The stronger
    # broadband data at 200..300 Hz must not hide a weak low-frequency pilot.
    gap = max(2, math.ceil(4*n/RATE))
    radius = max(gap+1, math.ceil(25*n/RATE))
    scores = []
    for i in indices:
        neighbors = np.r_[power[max(0, i-radius):max(0, i-gap)],
                          power[i+gap:min(len(power), i+radius+1)]]
        floor = max(float(np.median(neighbors)), 1e-30)
        scores.append(10 * math.log10(max(float(power[i]), 1e-30) / floor))
    best = int(np.argmax(scores))
    idx, prominence = indices[best], scores[best]
    pilot = None
    if prominence >= 10 and db[idx] > -110:
        logp = np.log(np.maximum(power[idx - 1:idx + 2], 1e-30))
        denominator = logp[0] - 2 * logp[1] + logp[2]
        delta = 0.5 * (logp[0] - logp[2]) / denominator if denominator else 0
        pilot = (float((idx + np.clip(delta, -0.5, 0.5)) * RATE / n), prominence)
    visible = freq <= 4000
    return freq[visible], db[visible], pilot


class Receiver:
    def __init__(self, source, center, log_path, record=None, manual=False, collector=None, raw_frames=False):
        self.source, self.center = source, center
        self.collector, self.raw_frames = collector, raw_frames
        if collector:
            collector.begin_session()
        self.log_path = Path(log_path)
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log = self.log_path.open("a", buffering=1)
        self.log.write(json.dumps({"event": "session", "source": source,
                                   "center_hz": center, "afc_enabled": not manual, "sample_rate": RATE,
                                   "started_at": datetime.now().astimezone().isoformat()}) + "\n")
        self.record = Path(record).open("wb") if record else None
        self.guard = threading.Lock()
        self.stopping = threading.Event()
        self.audio = deque(maxlen=20)
        self.status = {}
        self.status_at = 0.0
        self.messages = deque(maxlen=100)
        self.errors = deque(maxlen=10)
        self.threads = []
        self.capture = self.decoder = None
        self.last_logged_second = -1
        try:
            decoder_args = [str(ROOT / "build/rx_probe"), "--center-hz", str(center)]
            if manual:
                decoder_args.append("--manual")
            self.decoder = subprocess.Popen(
                decoder_args,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.capture = subprocess.Popen(
                ["parec", "--device=" + source, "--format=float32le", "--rate=48000",
                 "--channels=1", "--raw", "--latency-msec=100"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            for target in (self._capture_audio, self._read_events,
                           lambda: self._read_errors(self.capture, "PulseAudio"),
                           lambda: self._read_errors(self.decoder, "RX-Kern")):
                t = threading.Thread(target=target, daemon=True)
                self.threads.append(t)
                t.start()
        except Exception:
            self.close()
            raise

    def error(self, text):
        if not self.stopping.is_set():
            with self.guard:
                self.errors.append(text)

    def _capture_audio(self):
        try:
            while not self.stopping.is_set():
                data = self.capture.stdout.read(BLOCK * 4)
                if not data:
                    self.error("Audioquelle beendet; kein Empfang.")
                    break
                if len(data) % 4:
                    self.error("Unvollständiges Float32-Audiosample.")
                    break
                with self.guard:
                    self.audio.append(np.frombuffer(data, dtype="<f4").copy())
                if self.record:
                    self.record.write(data)
                self.decoder.stdin.write(data)
                self.decoder.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            self.error("Audioeingang: " + str(exc))
        finally:
            if self.decoder and self.decoder.stdin:
                try:
                    self.decoder.stdin.close()
                except (OSError, ValueError):
                    pass

    def _read_errors(self, process, label):
        for line in process.stderr:
            self.error(label + ": " + line.decode("utf-8", "replace").strip())

    def _read_events(self):
        try:
            for line in self.decoder.stdout:
                event = json.loads(line)
                event["received_at"] = datetime.now().astimezone().isoformat()
                collected = []
                if self.collector:
                    try:
                        collected = self.collector.ingest(event)
                    except (OSError, sqlite3.Error) as exc:
                        self.error("Datensammler: " + str(exc))
                with self.guard:
                    if event["event"] == "status":
                        self.status = event
                        self.status_at = time.monotonic()
                    elif self.collector:
                        self.messages.extend(collected)
                    else:
                        self.messages.append(event)
                # Text events live in data/events.jsonl. Raw frame tracing is optional.
                second = int(event.get("audio_seconds", 0))
                if ((event["event"] == "status" and second != self.last_logged_second)
                        or (event["event"] == "frame" and self.raw_frames)):
                    self.log.write(json.dumps({k: v for k, v in event.items() if k != "points"}) + "\n")
                    if event["event"] == "status":
                        self.last_logged_second = second
            self.error("RX-Kern beendet.")
        except (ValueError, OSError) as exc:
            self.error("RX-Ausgabe: " + str(exc))

    def snapshot(self):
        with self.guard:
            audio = np.concatenate(list(self.audio)) if self.audio else np.array([])
            events = list(self.messages)
            self.messages.clear()
            return dict(self.status), self.status_at, audio, events, list(self.errors)

    def close(self):
        self.stopping.set()
        for proc in (self.capture, self.decoder):
            if proc and proc.poll() is None:
                proc.terminate()
        for proc in (self.capture, self.decoder):
            if proc:
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        for thread in self.threads:
            thread.join(timeout=2)
        if self.record:
            self.record.close()
        self.log.close()


def run_gui(args, collector, api_url=""):
    import tkinter as tk
    from tkinter import ttk
    root = tk.Tk()
    root.title("QO-100 · Bakenmonitor")
    root.geometry("1200x940")
    root.minsize(900, 700)
    root.configure(bg="#101923")
    style = ttk.Style(root)
    style.theme_use("clam")
    style.configure("TFrame", background="#101923")
    style.configure("TLabel", background="#101923", foreground="#e1e9f0", font=("Sans", 11))
    outer = ttk.Frame(root, padding=16)
    outer.pack(fill="both", expand=True)
    title = "QO-100 · Empfang und " + ("manuelle Abstimmung" if args.manual else "automatische Frequenzkorrektur")
    ttk.Label(outer, text=title, font=("Sans", 18, "bold")).pack(anchor="w")
    settings = tk.StringVar(value=f"Quelle: {args.source}  ·  48 kHz mono  ·  8APSK / 7200 bit/s")
    ttk.Label(outer, textvariable=settings).pack(anchor="w", pady=(5, 10))
    counts_label = tk.StringVar()
    ttk.Label(outer, textvariable=counts_label, font=("Sans", 11, "bold")).pack(anchor="w", pady=(0, 5))
    transfer_label = tk.StringVar()
    ttk.Label(outer, textvariable=transfer_label).pack(anchor="w", pady=(0, 5))
    reset_button = ttk.Button(outer, text="Synchronisation neu starten")
    reset_button.pack(anchor="e", pady=(0, 6))
    state = tk.Label(outer, text="Audioeingang wird geöffnet …", bg="#26374b", fg="white", font=("Sans", 15, "bold"), padx=12, pady=10, anchor="w")
    state.pack(fill="x")
    detail = tk.StringVar()
    ttk.Label(outer, textvariable=detail).pack(anchor="w", pady=8)
    pilot_label = tk.StringVar(value="Hilfsträger wird gesucht …")
    ttk.Label(outer, textvariable=pilot_label, font=("Sans", 12, "bold")).pack(anchor="w", pady=(0, 5))
    footer_frame = ttk.Frame(outer)
    footer_frame.pack(side="bottom", fill="x")
    notebook = ttk.Notebook(outer)
    notebook.pack(fill="both", expand=True, pady=(8, 5))
    from tkinter.scrolledtext import ScrolledText

    def text_area(parent, wrap="word"):
        widget = ScrolledText(parent, bg="#0a111a", fg="#d4e4ef", insertbackground="white",
                              font=("Monospace", 11), wrap=wrap, state="disabled")
        widget.pack(fill="both", expand=True, pady=5)
        return widget

    channel_widgets = {}
    for channel, title in (("cw-skimmer", "CW-Skimmer"), ("dx-cluster", "DX-Cluster")):
        page = ttk.Frame(notebook, padding=10)
        notebook.add(page, text=title)
        ttk.Label(page, text="Empfangene Meldungen · nach Empfangszeit · Duplikate innerhalb von 120 s unterdrückt").pack(anchor="w")
        channel_widgets[channel] = text_area(page, "none")
    bulletin_page = ttk.Frame(notebook, padding=10)
    notebook.add(bulletin_page, text="Bulletins")
    bulletin_select = ttk.Combobox(bulletin_page, state="readonly", font=("Sans", 11))
    bulletin_select.pack(fill="x", pady=4)
    bulletin_view = text_area(bulletin_page)
    bulletin_rows = []
    displayed_bulletin = [None]
    diagnostic = ttk.Frame(notebook, padding=10)
    notebook.add(diagnostic, text="Empfang / AFC")
    notebook.select(bulletin_page if collector.bulletins() else 1)

    def set_text(widget, text, follow=False):
        position = widget.yview()
        widget.configure(state="normal")
        widget.delete("1.0", "end")
        widget.insert("1.0", text)
        widget.configure(state="disabled")
        if follow and (not position or position[1] >= 0.99):
            widget.see("end")
        elif position:
            widget.yview_moveto(position[0])

    def show_bulletin(*_):
        index = bulletin_select.current()
        if 0 <= index < len(bulletin_rows):
            event = bulletin_rows[index]
            if event['seq'] == displayed_bulletin[0]:
                return
            displayed_bulletin[0] = event['seq']
            set_text(bulletin_view, f"{event['fields']['filename']}\nEmpfangen: {event['received_at']}\n\n{event['text']}\n")
        else:
            set_text(bulletin_view, "Noch kein vollständig empfangenes Bulletin.\nUnvollständige Dateien werden über Wiederholungen ergänzt.")

    bulletin_select.bind("<<ComboboxSelected>>", show_bulletin)
    displayed_revision = [-1]

    def refresh_collected():
        info = collector.status()
        counts = info["counts"]
        counts_label.set(f"Gesammelt:  CW-Skimmer {counts['cw-skimmer']}   ·   DX-Cluster {counts['dx-cluster']}   ·   Bulletins {counts['bulletin']}")
        pending = info["pending_bulletins"]
        if pending:
            transfer_label.set("Bulletin-Empfang: " + "   ·   ".join(f"{p['filename']} {p['received']}/{p['expected']} Blöcke" for p in pending[:3]))
        else:
            transfer_label.set("Bulletin-Empfang: warte auf nächste Aussendung")
        if info["last_error"]:
            transfer_label.set(transfer_label.get()+"   ·   "+info["last_error"])
        if info["last_sequence"] == displayed_revision[0]:
            return
        displayed_revision[0] = info["last_sequence"]
        for channel, widget in channel_widgets.items():
            rows = collector.recent(channel, 500)
            text = "".join(f"{r['received_at'][11:19]}  {r['text']}\n" for r in rows)
            if not text:
                text = "Noch keine Meldung empfangen.\n"
            set_text(widget, text, follow=True)
        old_index = bulletin_select.current()
        selected_seq = bulletin_rows[old_index]["seq"] if 0 <= old_index < len(bulletin_rows) else None
        follow_latest = old_index in (-1, 0)
        bulletin_rows[:] = collector.bulletins()
        bulletin_select["values"] = [f"{r['fields']['filename']}  ·  {r['received_at']}" for r in bulletin_rows]
        if bulletin_rows:
            index = 0 if follow_latest else next((i for i,r in enumerate(bulletin_rows) if r['seq']==selected_seq),0)
            bulletin_select.current(index)
        show_bulletin()

    plot = tk.Canvas(diagnostic, bg="#0a111a", highlightthickness=0, height=225)
    plot.pack(fill="both", expand=True)
    bottom = ttk.Frame(diagnostic)
    bottom.pack(fill="x", pady=(10, 5))
    constellation = tk.Canvas(bottom, width=225, height=210, bg="#0a111a", highlightthickness=0)
    constellation.pack(side="left")
    instructions = ("Manuell abstimmen:\n\n"
                    "1. Den schmalen Hilfsträger auf die grüne Soll-Linie bringen.\n"
                    "2. Nach jeder Änderung 2–3 Sekunden warten.\n"
                    "3. Grün bedeutet: mehrere FEC- und CRC-geprüfte Blöcke.\n\n"
                    "Der Trägerfund allein beweist keine Synchronisation.\n"
                    "Die PLL-Anzeige ist erst bei gültigen Blöcken aussagekräftig.") if args.manual else (
                    "Automatische Abstimmung aktiv\n\n"
                    "Der Hilfsträger wird gesucht und der Versatz intern korrigiert.\n"
                    "Die grüne Linie folgt der eingestellten Empfangsmitte.\n"
                    "Grün bestätigt mehrere FEC- und CRC-geprüfte Blöcke.\n\n"
                    "Bei kurzem Ausfall bleibt die letzte Frequenz erhalten.\n"
                    "Bei längerem Synchronisationsverlust beginnt die Suche erneut.\n"
                    "Das komplette Signal muss im Audio-Durchlassbereich liegen.")
    ttk.Label(bottom, text=instructions, justify="left").pack(side="left", padx=20)
    history = tk.Text(diagnostic, height=4, bg="#0a111a", fg="#b6d8e8", font=("Monospace", 10), wrap="none")
    history.pack(fill="x", pady=5)
    footer = tk.StringVar(value=str(args.log))
    ttk.Label(footer_frame, textvariable=footer, font=("Sans", 9)).pack(anchor="w")
    ttk.Label(footer_frame, text=f"Texte: {args.data_dir}  ·  Abholung: {api_url or 'HTTP ausgeschaltet'}", font=("Sans", 9)).pack(anchor="w")
    holder = [Receiver(args.source, args.center_hz, args.log, args.record, args.manual, collector, args.raw_frames)]
    closing = [False]
    point_history = deque(maxlen=700)

    def reset():
        holder[0].close()
        holder[0] = Receiver(args.source, args.center_hz, args.log, manual=args.manual,
                             collector=collector, raw_frames=args.raw_frames)
        point_history.clear()
        history.delete("1.0", "end")

    reset_button.configure(command=reset)

    def close():
        closing[0] = True
        holder[0].close()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)

    def update():
        if closing[0]:
            return
        s, at, audio, events, errors = holder[0].snapshot()
        refresh_collected()
        fresh = at and time.monotonic() - at < 2
        locked = fresh and s.get("locked", False)
        if not fresh:
            state.configure(text="KEIN AUDIO / noch keine aktuellen RX-Daten", bg="#803b35")
        elif locked:
            state.configure(text="SYNCHRON · FEC- und CRC-geprüfte Daten", bg="#21644e")
        else:
            state.configure(text="SUCHE · noch keine stabile Frame-Synchronisation", bg="#805b24")
        if s:
            current_center = s.get("center_hz", args.center_hz)
            afc_names = {"manual": "Manuell", "searching": "Suche", "acquiring": "Prüfe Träger",
                         "tracking": "Nachführen", "hold": "Halte Frequenz"}
            afc_name = afc_names.get(s.get("afc_state"), "Suche")
            settings.set(f"Quelle: {args.source}  ·  48 kHz mono  ·  8APSK / 7200 bit/s  ·  Mischer: {current_center:.1f} Hz  ·  AFC: {afc_name}")
            pll = f"{s['pll_hz']:+.2f} Hz" if locked else "—"
            detail.set(f"Gültige Blöcke: {s['valid_frames']}   |   letzte 2 s: {s['recent_frames']}   |   "
                       f"RMS: {s['rms_dbfs']:.1f} dBFS   |   Peak: {s['peak_dbfs']:.1f} dBFS   |   PLL: {pll}")
            if s["peak_dbfs"] >= -0.5:
                footer.set("Audio nahe Vollaussteuerung: Pegel reduzieren.   " + str(args.log))
            else:
                footer.set(errors[-1] if errors else f"Header-Kandidaten: {s['header_candidates']} · FEC verworfen: {s['fec_failures']} · CRC verworfen: {s['crc_failures']} · Log: {args.log}")
        elif errors:
            footer.set(errors[-1])
        f, db, pilot = spectrum(audio)
        current_center = s.get("center_hz", args.center_hz)
        target = current_center - 1400
        if pilot and fresh:
            if args.manual:
                pilot_label.set(f"Hilfsträger-Kandidat: {pilot[0]:.1f} Hz   ·   Soll: {target:.1f} Hz   ·   Abweichung: {pilot[0]-target:+.1f} Hz")
            else:
                pilot_label.set(f"Hilfsträger im Eingang: {pilot[0]:.1f} Hz   ·   Mischerkorrektur: {current_center-1500:+.1f} Hz   ·   Rest zum Träger: {pilot[0]-target:+.1f} Hz")
        else:
            pilot_label.set(f"Kein eindeutiger Hilfsträger-Kandidat · Mischerkorrektur: {current_center-1500:+.1f} Hz")
        w, h = max(plot.winfo_width(), 200), max(plot.winfo_height(), 120)
        left, right, top, floor = 48, w-12, 22, h-30
        plot.delete("all")
        def px(freq): return left + freq / 4000 * (right-left)
        def py(level): return top + (0 - np.clip(level, -110, 0)) / 110 * (floor-top)
        for level in range(-100, 1, 20):
            y = py(level)
            plot.create_line(left, y, right, y, fill="#223246")
            plot.create_text(left-5, y, text=str(level), fill="#91a7bc", anchor="e", font=("Sans", 9))
        for freq in range(0, 4001, 500):
            x = px(freq)
            plot.create_line(x, top, x, floor, fill="#223246")
            plot.create_text(x, floor+15, text=str(freq), fill="#91a7bc", font=("Sans", 9))
        for freq, color, label in ((target, "#62e5ad", f"Pilot / Mischer {target:.1f} Hz"),
                                   (current_center, "#6088be", "Empfangsmitte")):
            plot.create_line(px(freq), top, px(freq), floor, fill=color, dash=(4, 3))
            plot.create_text(px(freq)+5, top, text=label, fill=color, anchor="nw", font=("Sans", 9))
        if len(f):
            # One maximum per screen column preserves narrow pilot peaks.
            stride = max(1, len(f)//int(right-left))
            coords = []
            for start in range(0, len(f), stride):
                idx = start + int(np.argmax(db[start:start+stride]))
                coords.extend((px(f[idx]), py(db[idx])))
            if len(coords) >= 4:
                plot.create_line(*coords, fill="#66c7ec", width=1)
        plot.create_text(right, 7, text="Hz", anchor="e", fill="#91a7bc")
        plot.create_text(left, 7, text="Audio-Spektrum · dBFS je FFT-Bin", anchor="w", fill="#91a7bc", font=("Sans", 9))
        constellation.delete("all")
        constellation.create_line(0, 105, 225, 105, fill="#223246")
        constellation.create_line(112, 0, 112, 210, fill="#223246")
        if fresh:
            point_history.extend(s.get("points", []))
            for re, im in point_history:
                if math.isfinite(re) and math.isfinite(im):
                    x, y = 112 + re*63, 105 - im*63
                    constellation.create_oval(x-1, y-1, x+1, y+1, fill="#66c7ec", outline="")
        constellation.create_text(8, 8, text="8APSK-Konstellation", anchor="nw", fill="#91a7bc")
        for event in events:
            description = event['fields'].get('filename',event['text'].split('\n')[0])
            history.insert("end", datetime.fromisoformat(event["received_at"]).strftime("%H:%M:%S") + "  " + event['channel'] + " · " + description + "\n")
        if int(history.index("end-1c").split(".")[0]) > 250:
            history.delete("1.0", "100.0")
        history.see("end")
        root.after(250, update)

    update()
    try:
        root.mainloop()
    finally:
        if not closing[0]:
            holder[0].close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default="fromQO100")
    parser.add_argument("--center-hz", type=float, default=1500)
    parser.add_argument("--manual", action="store_true", help="Automatische Frequenzkorrektur abschalten")
    parser.add_argument("--headless", action="store_true")
    parser.add_argument("--seconds", type=float, default=0, help="Laufzeit im Headless-Modus; 0 = unbegrenzt")
    parser.add_argument("--record", type=Path, help="Optional: Mono Float32 LE, 48 kHz, ohne WAV-Header")
    parser.add_argument("--log", type=Path, default=ROOT/"logs"/datetime.now().strftime("rx-%Y%m%d-%H%M%S.jsonl"))
    parser.add_argument("--data-dir", type=Path, default=ROOT/"data", help="Persistente Texte, Datenbank und Bulletin-Zwischenspeicher")
    parser.add_argument("--http-port", type=int, default=8765, help="Lokale Textschnittstelle; 0 wählt einen freien Port")
    parser.add_argument("--no-http", action="store_true", help="Nur Textdateien bereitstellen")
    parser.add_argument("--raw-frames", action="store_true", help="Zusätzlich alle rohen Empfangsblöcke protokollieren")
    parser.add_argument("--import-logs", action="store_true", help="Bisherige rx-*.jsonl-Aufzeichnungen importieren")
    parser.add_argument("--replay-only", action="store_true", help="Aufzeichnungen importieren und ohne Live-Empfang beenden")
    args = parser.parse_args()
    if not math.isfinite(args.center_hz) or not 1420 <= args.center_hz <= 2300:
        parser.error("--center-hz muss zwischen 1420 und 2300 liegen")
    if not math.isfinite(args.seconds) or args.seconds < 0:
        parser.error("--seconds muss endlich und >= 0 sein")
    if not 0 <= args.http_port <= 65535:
        parser.error("--http-port muss zwischen 0 und 65535 liegen")
    collector = Collector(args.data_dir)
    api = None
    try:
        if args.import_logs or args.replay_only:
            replay_logs(collector, (ROOT/"logs").glob("rx-*.jsonl"))
        if args.replay_only:
            print(json.dumps(collector.status(), ensure_ascii=False))
            return
        if not args.no_http:
            try:
                api = TextAPI(collector, args.http_port)
            except OSError as exc:
                parser.exit(1, f"Textschnittstelle konnte nicht starten: {exc}. Anderen --http-port wählen oder --no-http verwenden.\n")
        run_live(args, collector, api.url if api else "")
    finally:
        if api:
            api.close()
        collector.close()


def run_live(args, collector, api_url):
    if not (ROOT/"build/rx_probe").is_file():
        raise SystemExit("RX-Kern fehlt. Bitte zuerst make ausführen.")
    if not args.headless:
        run_gui(args, collector, api_url)
        return
    receiver = Receiver(args.source, args.center_hz, args.log, args.record, args.manual, collector, args.raw_frames)
    interrupted = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: interrupted.set())
    start = time.monotonic()
    try:
        while not interrupted.wait(1):
            s, at, audio, events, errors = receiver.snapshot()
            _, _, pilot = spectrum(audio)
            if s:
                s.pop("points", None)
                s["locked"] = bool(s["locked"] and time.monotonic()-at < 2)
                s["pilot_hz"] = pilot[0] if pilot else None
                print(json.dumps(s), flush=True)
            for event in events:
                print(json.dumps(event, ensure_ascii=False), flush=True)
            if errors:
                print(json.dumps({"event": "error", "message": errors[-1]}), flush=True)
                if receiver.capture.poll() is not None or receiver.decoder.poll() is not None:
                    raise SystemExit(1)
            if args.seconds and time.monotonic()-start >= args.seconds:
                break
    finally:
        receiver.close()


if __name__ == "__main__":
    main()
