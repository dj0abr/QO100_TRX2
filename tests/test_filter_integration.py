"""Exercise RX1 filter controls and continuous dual audio over the real UDP link."""
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gui"))
from protocol import HEADER, AUDIO, STATE, audio_packet, subscription, tune, filter_control, filter_state, filter_edges_control


def main():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory() as temp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
        sock.settimeout(.05)
        sock.connect(("127.0.0.1", port))
        config = Path(temp) / "backend.conf"
        text = Path(sys.argv[2]).read_text().replace("gui_port = 40900", f"gui_port = {port}")
        text = text.replace("rx1_filter_low_hz = 100", "rx1_filter_low_hz = 750")
        text = text.replace("rx1_filter_high_hz = 4000", "rx1_filter_high_hz = 1250")
        config.write_text(text)
        process = subprocess.Popen([sys.argv[1], "--config", str(config), "--source", "synthetic"],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        audio = {1: [], 2: []}
        states, replies = {}, {}
        zoom_ids = set()
        next_subscription = 0

        def pump(seconds):
            nonlocal next_subscription
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if time.monotonic() >= next_subscription:
                    try:
                        sock.send(subscription())
                        sock.send(tune(1, 10489748500, stream_id=1))
                        sock.send(tune(1, 10489600000, stream_id=2))
                        next_subscription = time.monotonic() + .5
                    except ConnectionRefusedError:
                        continue  # Backend is still starting.
                try:
                    packet = sock.recv(65536)
                except (socket.timeout, ConnectionRefusedError):
                    continue
                f = HEADER.unpack_from(packet)
                reply = filter_state(packet)
                if reply:
                    replies[reply[0]] = reply
                elif f[2] == STATE:
                    states[f[4]] = f
                elif f[2] == AUDIO:
                    p = audio_packet(packet)
                    assert p is not None
                    if p.stream_id in audio:
                        audio[p.stream_id].append(p)
                elif f[2] == 2 and f[4] == 1:
                    zoom_ids.add(f[5])

        request = 0

        def command(width=None, center=None, status=0, packet=None):
            nonlocal request
            request += 1
            payload = packet if packet is not None else filter_control(request, session, width, center)
            deadline = time.monotonic() + 2
            while request not in replies and time.monotonic() < deadline:
                sock.send(payload)
                pump(.05)
            assert request in replies, f"Missing filter ACK {request}"
            reply = replies[request]
            assert reply[1] == session and reply[4] == status, reply
            return reply[2:4]

        def level(stream, hz):
            pump(.12)  # Let the 20-ms transition and packet assembly finish.
            start = len(audio[stream])
            pump(.42)
            packets = audio[stream][start:]
            assert len(packets) >= 15
            x = np.concatenate([p.samples for p in packets])
            return abs(np.mean(x * np.exp(-2j*np.pi*hz*np.arange(len(x))/12000))) * 2

        try:
            deadline = time.monotonic() + 3
            while not {1, 2}.issubset(states) and time.monotonic() < deadline:
                pump(.1)
            assert {1, 2}.issubset(states), states
            session = states[1][15]
            identity = {s: (f[15], f[7]) for s, f in states.items()}
            assert command() == (500, 1000), "Query must preserve configured startup filter"
            assert .115 < level(1, 1000) < .125
            assert command(500, 2500) == (500, 2500)
            assert level(1, 1000) < .00005
            assert .075 < level(2, 1700) < .085, "RX2 affected by RX1 filtering"
            assert command(3900, 2050) == (3900, 2050)
            assert .115 < level(1, 1000) < .125
            assert command(501, 1000) == (501, 1000)
            # The same request ID cannot replace an already accepted setting.
            sock.send(filter_control(request, session, 500, 2500))
            pump(.08)
            assert command() == (501, 1000)
            sock.send(filter_control(request-1, session, 500, 2500))
            pump(.08)
            assert command() == (501, 1000), "Stale control applied"
            sock.send(filter_control(request+1, session ^ 1, 500, 2500))
            pump(.08)
            assert request+1 not in replies, "Wrong-session control acknowledged"
            assert command() == (501, 1000)
            malformed = list(HEADER.unpack(filter_control(request+1, session, 500, 2500)))
            malformed[4] = 2
            sock.send(HEADER.pack(*malformed))
            pump(.08)
            assert request+1 not in replies, "RX2 filter control accepted"
            assert command() == (501, 1000)
            invalid = list(HEADER.unpack(filter_control(request+1, session, 500, 2500)))
            invalid[10] = 100  # Well-formed command, unsupported bandwidth.
            assert command(status=1, packet=HEADER.pack(*invalid)) == (501, 1000)
            assert command() == (501, 1000)
            # Exact cutoff protocol, including a half-Hz center and legacy half-Hz edges.
            assert command(packet=filter_edges_control(request+1, session)) == (501, 1000)
            assert command(packet=filter_edges_control(request+1, session, 301, 2700)) == (2399, 1500.5)
            assert command(packet=filter_edges_control(request+1, session)) == (2399, 1500.5)
            assert command(packet=filter_edges_control(request+1, session, 700.5, 1700)) == (999.5, 1200.25)
            assert .115 < level(1, 1000) < .125
            assert level(1, 1700) < .05
            assert .075 < level(2, 1700) < .085
            invalid_edges = list(HEADER.unpack(filter_edges_control(request+1, session, 100, 4000)))
            invalid_edges[8], invalid_edges[10] = 1900000, 2000000
            assert command(status=1, packet=HEADER.pack(*invalid_edges)) == (999.5, 1200.25)
            for stream, packets in audio.items():
                assert len(packets) > 100
                assert all((p.session_id, p.epoch) == identity[stream] for p in packets)
                assert all(b.sample_index == a.sample_index + len(a.samples)
                           for a, b in zip(packets, packets[1:])), f"RX{stream} audio interrupted"
            assert len(zoom_ids) > 30
            print("Configured startup, shifted passband, full/odd bandwidth, RX2 independence, continuous "
                  "audio/zoom, duplicate/stale/wrong-session/invalid controls verified over UDP")
        finally:
            sock.close()
            process.terminate()
            output = process.communicate(timeout=5)[0]
            print(output)
        assert process.returncode == 0, process.returncode


if __name__ == "__main__":
    main()
