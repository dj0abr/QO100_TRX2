#!/usr/bin/env python3
"""Opt-in real Pluto RX test. Not part of CTest; TX stays at zero IQ unless GUI software PTT is held."""
import argparse
import json
from pathlib import Path
import queue
import subprocess
import sys
import time
import wave
import numpy as np

PROJECT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT / "gui"))
from test_gui import Receiver, draw_image, save_capture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", action="store_true", help="Also open the Tk test GUI for 6 seconds")
    parser.add_argument("--frames", type=int, default=120)
    args = parser.parse_args()
    if args.frames < 40:
        parser.error("Use at least 40 frames for meaningful rate measurement")
    config_path = PROJECT / "config/backend.conf"
    settings = {}
    for line in config_path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            key, value = line.split("=", 1)
            settings[key.strip()] = value.strip()
    captures = PROJECT / "build/captures"
    captures.mkdir(parents=True, exist_ok=True)
    received = []
    audio_received, audio2_received, zoom_received = [], [], []
    driver_pids = []
    receiver = None
    with (captures / "hardware-backend.log").open("w") as log:
        backend = subprocess.Popen([str(PROJECT / "build/bin/qo100_backend"), "--config", str(config_path)],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            receiver = Receiver("127.0.0.1", int(settings["gui_port"]),rx_hz=int(settings["initial_rx_hz"]))
            start = time.monotonic()
            while len(received) < args.frames and time.monotonic() - start < 20 + args.frames / 10:
                try:
                    received.append(receiver.frames.get(timeout=0.3))
                    for source, target in ((receiver.audio_frames,audio_received),(receiver.audio2_frames,audio2_received),(receiver.zoom_frames,zoom_received)):
                        while True:
                            try: target.append(source.get_nowait())
                            except queue.Empty: break
                except queue.Empty:
                    if backend.poll() is not None:
                        raise RuntimeError("Backend exited; see build/captures/hardware-backend.log")
            assert len(received) == args.frames, f"Only {len(received)} frames"
            receiver.close()
            assert len(zoom_received)>20 and len(audio_received)>100 and len(audio2_received)>100
            assert receiver.channels[2].applied_hz==int(settings['initial_rx2_hz'])
            assert all(p.stream_id==2 and p.center_hz==int(settings['initial_rx2_hz']) for p in audio2_received)
            assert all(b.sample_index==a.sample_index+len(a.samples) and b.epoch==a.epoch
                       for a,b in zip(audio2_received,audio2_received[1:])), "RX2 sample discontinuity"
            decoded2=np.concatenate([p.samples for p in audio2_received])
            assert np.isfinite(decoded2).all() and np.std(decoded2)>.00001
            with wave.open(str(captures/'rx2-usb.wav'),'wb') as wav:
                wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(12000)
                wav.writeframes((np.clip(decoded2,-1,1)*32767).astype('<i2').tobytes())
            assert zoom_received[-1].center_hz==int(settings['initial_rx_hz'])
            assert zoom_received[-1].sample_rate==75000
            assert receiver.applied_hz==int(settings['initial_rx_hz'])
            decoded=np.concatenate([p.samples for p in audio_received])
            assert np.isfinite(decoded).all() and np.std(decoded)>.00001
            with wave.open(str(captures/'rx-usb.wav'),'wb') as wav:
                wav.setnchannels(1);wav.setsampwidth(2);wav.setframerate(12000)
                wav.writeframes((np.clip(decoded,-1,1)*32767).astype('<i2').tobytes())
            draw_image(zoom_received,span_hz=50000).save(captures/'zoom-live.png')
            first, last = received[0], received[-1]
            assert last.sample_rate == int(settings["pluto_sample_rate"]) // 2
            assert last.center_hz == int(settings["rx_center_hz"])
            assert last.fft_size == int(settings["fft_size"])
            assert np.isfinite(last.db).all() and float(np.ptp(last.db)) > 5
            observed = (last.sample_index - first.sample_index) / ((last.timestamp_ns - first.timestamp_ns) / 1e9)
            assert abs(observed / last.sample_rate - 1) < 0.02, observed
            assert receiver.assembler.invalid_packets == 0
            assert receiver.assembler.incomplete_frames == 0
            driver_pids = [int(x) for x in Path(f"/proc/{backend.pid}/task/{backend.pid}/children").read_text().split()]
            assert len(driver_pids) == 1, driver_pids
            uri = settings["pluto_uri"]

            def attr(direction, channel, name):
                return subprocess.check_output(["iio_attr", "-u", uri, direction, "-c", "ad9361-phy", channel, name], text=True, timeout=5).strip()

            tx_powerdown = attr("-o", "altvoltage1", "powerdown")
            rx_frequency = attr("-o", "altvoltage0", "frequency")
            hardware_rate = attr("-i", "voltage0", "sampling_frequency")
            assert int(rx_frequency) == int(settings["pluto_rx_hz"])
            assert int(hardware_rate) == int(settings["pluto_sample_rate"])
            save_capture(captures / "rx-live.npz", last)
            draw_image(received).save(captures / "rx-live.png")
            if args.gui:
                subprocess.run(["/usr/bin/python3", str(PROJECT / "gui/test_gui.py"), "--port", settings["gui_port"],
                                "--seconds", "8", "--screenshot", str(captures / "gui-live.png"), "--audio-report", str(captures/"audio-playback.json")], check=True, timeout=15)
                assert (captures / "gui-live.png").exists()
                playback=json.loads((captures/'audio-playback.json').read_text())
                assert playback['played_samples']>24000,playback
                assert playback['audio_error']=='',playback
                assert playback['zoom_frames']>20,playback
                assert playback['rx2_packets']>100 and playback['rx2_samples']>24000,playback
                assert playback['rx2_applied_hz']==int(settings['initial_rx2_hz']),playback
                assert playback['invalid_audio']==0,playback
                assert playback['rx1_filter']==[int(settings['rx1_filter_bandwidth_hz']),
                                                 int(settings['rx1_filter_center_hz'])],playback
                assert playback['rx1_filter_error']=='',playback
            result = {
                "source": uri, "frames": len(received), "iq_sample_rate_observed": observed,
                "center_hz": last.center_hz, "pluto_rx_hz": int(rx_frequency),
                "pluto_sample_rate": int(hardware_rate), "tx_lo_powerdown": int(tx_powerdown),
                "fft_size": last.fft_size, "bin_hz": last.sample_rate / last.fft_size,
                "peak_hz": float(last.frequencies_hz[np.argmax(last.db)]),
                "peak_dbfs": float(last.db.max()), "median_dbfs": float(np.median(last.db)),
                "invalid_packets": receiver.assembler.invalid_packets,
                "incomplete_frames": receiver.assembler.incomplete_frames,
                "gui_smoke_test": args.gui, "zoom_frames":len(zoom_received),
                "audio_packets":len(audio_received), "audio_sample_rate":12000,
                "usb_carrier_hz":receiver.applied_hz,"audio_rms":float(np.std(decoded)),
                "rx2_carrier_hz":receiver.channels[2].applied_hz,"rx2_audio_packets":len(audio2_received),
                "rx2_audio_samples":len(decoded2),"rx2_audio_rms":float(np.std(decoded2)),
            }
        finally:
            if receiver:
                receiver.close()
            backend.terminate()
            try:
                backend.wait(timeout=5)
            except subprocess.TimeoutExpired:
                backend.kill()
                backend.wait()
            assert backend.returncode == 0, f"Backend exit status {backend.returncode}; see hardware-backend.log"
            assert all(not Path(f"/proc/{pid}").exists() for pid in driver_pids), "Driver child still running"
        result["driver_cleanup"] = "verified"
        result["tested_at_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        (captures / "hardware-test.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
