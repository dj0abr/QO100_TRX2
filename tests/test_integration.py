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
        process = subprocess.Popen([sys.argv[1], "--config", str(config), "--source", "synthetic", "--duration", "12"],
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
            def channel_audio(stream, count=40):
                q=receiver.channels[stream].frames
                return [q.get(timeout=3) for _ in range(count)]

            def amplitude(packets, hz):
                x=np.concatenate([p.samples for p in packets])[3000:]
                return abs(np.sum(x*np.exp(-2j*np.pi*hz*np.arange(len(x))/12000)))*2/len(x)

            def wait_frequency(stream, hz):
                deadline=time.monotonic()+3
                while receiver.channels[stream].applied_hz!=hz and time.monotonic()<deadline:
                    receiver.frames.get(timeout=1)
                assert receiver.channels[stream].applied_hz==hz

            second=channel_audio(2)
            assert all(p.stream_id==2 and p.center_hz==10489600000 for p in second)
            assert .075<amplitude(second,1700)<.085
            assert amplitude(second,2200)<.0003
            primary_delivered=[]
            receiver.channels[1].sink=primary_delivered.append
            first_identity=receiver.channels[1].identity
            receiver.set_frequency(10489600700,stream_id=2)
            wait_frequency(2,10489600700)
            # An old RX2 request must not tune back or affect RX1's sequence numbers.
            receiver.sock.send(tune(1,10489600000,stream_id=2))
            second_tuned=channel_audio(2)
            assert all(p.stream_id==2 and p.center_hz==10489600700 for p in second_tuned)
            assert .075<amplitude(second_tuned,1000)<.085
            assert receiver.applied_hz==10489748500 and receiver.channels[1].identity==first_identity
            assert len(primary_delivered)>20
            assert all(p.stream_id==1 for p in primary_delivered), "RX2 routed to soundcard sink"
            assert all(b.sample_index==a.sample_index+len(a.samples) and b.epoch==a.epoch
                       for a,b in zip(primary_delivered,primary_delivered[1:])), "RX1 interrupted by RX2 tuning"
            second_identity=receiver.channels[2].identity
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
            second_after_primary=channel_audio(2,25)
            assert receiver.channels[2].identity==second_identity
            assert all(p.center_hz==10489600700 for p in second_after_primary)
            assert all(b.sample_index==a.sample_index+len(a.samples)
                       for a,b in zip(second_after_primary,second_after_primary[1:])), "RX2 interrupted by RX1 tuning"
            receiver.set_frequency(10490050000,stream_id=2)
            deadline=time.monotonic()+2
            while not receiver.channels[2].tune_error and time.monotonic()<deadline:
                receiver.frames.get(timeout=1)
            assert receiver.channels[2].tune_error and receiver.channels[2].applied_hz==10489600700
            assert receiver.applied_hz==10489749500
            assert receiver.invalid_audio==0
            print("Two independent USB streams, wanted/LSB tones, per-channel ACKs, stale and rejected tuning, "
                  "continuity while tuning the other channel, and RX1-only playback routing verified")
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
