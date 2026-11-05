"""Independent Python receiver verifies the actual C++ UDP output with known tones."""
import subprocess
import sys
import tempfile
import time
from pathlib import Path
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from test_gui import Receiver
from protocol import tune


def main():
    with tempfile.TemporaryDirectory() as temp:
        config = Path(temp) / "backend.conf"
        # Ask the OS for a free GUI port; no hardware or fixed local Pluto ports involved.
        import socket
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        config.write_text(Path(sys.argv[2]).read_text().replace("gui_port = 40900", f"gui_port = {port}"))
        process = subprocess.Popen([sys.argv[1], "--config", str(config), "--source", "synthetic", "--duration", "8"],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        receiver = Receiver("127.0.0.1", port)
        try:
            f = receiver.frames.get(timeout=6)
            assert f.center_hz == 10489750000 and f.sample_rate == 600000
            assert len(f.db) == 8192
            negative = int(8192 / 2 - 150000 * 8192 / 600000)
            positive = int(8192 / 2 + 75000 * 8192 / 600000)
            assert np.argmax(f.db) == negative, (np.argmax(f.db), negative)
            assert abs(float(f.db[negative]) + 12.04) < 0.05, f.db[negative]
            assert abs(float(f.db[positive]) + 20) < 0.05, f.db[positive]
            assert receiver.assembler.invalid_packets == 0
            zoom = receiver.zoom_frames.get(timeout=4)
            assert zoom.stream_id==1 and zoom.center_hz==10489748500 and zoom.sample_rate==75000
            audio = [receiver.audio_frames.get(timeout=3) for _ in range(40)]
            assert all(p.center_hz==10489748500 for p in audio)
            samples=np.concatenate([p.samples for p in audio])[3000:]
            t=np.arange(len(samples))/12000
            upper=abs(np.sum(samples*np.exp(-2j*np.pi*1000*t)))*2/len(samples)
            lower=abs(np.sum(samples*np.exp(-2j*np.pi*2000*t)))*2/len(samples)
            assert .115<upper<.125, upper
            assert lower < upper*.003, (upper,lower)
            receiver.set_frequency(10489749500)
            deadline=time.monotonic()+3
            while receiver.applied_hz!=10489749500 and time.monotonic()<deadline:
                receiver.frames.get(timeout=1)
            assert receiver.applied_hz==10489749500
            # A delayed older control datagram must not tune back.
            receiver.sock.send(tune(1,10489748500))
            while True:
                z=receiver.zoom_frames.get(timeout=2)
                if z.center_hz==10489749500: break
            receiver.set_frequency(10490050000)
            deadline=time.monotonic()+2
            while not receiver.tune_error and time.monotonic()<deadline:
                receiver.frames.get(timeout=1)
            assert receiver.tune_error and receiver.applied_hz==10489749500
            print(f"Wide FFT, zoom, USB audio ({upper:.4f}), LSB suppression, confirmed tuning and invalid/stale commands verified")
        finally:
            receiver.close()
            process.terminate()
            try:
                output = process.communicate(timeout=5)[0]
            except subprocess.TimeoutExpired:
                process.kill()
                output = process.communicate()[0]
            print(output)
        assert process.returncode == 0, process.returncode


if __name__ == "__main__":
    main()
