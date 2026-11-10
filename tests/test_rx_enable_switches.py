"""Manual demodulator switches: actual PCM/worker removal, FFT and control survive."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'gui'))
from protocol import HEADER, settings_control, settings_state, subscription, tune


def main():
    with tempfile.TemporaryDirectory(prefix='qo100-rx-switches-') as temp:
        for mask in (15, 0, 1, 2, 4, 8, 12, 3):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
                probe.bind(('127.0.0.1', 0))
                port = probe.getsockname()[1]
            config = Path(temp) / f'backend-{mask}.conf'
            text = Path(sys.argv[2]).read_text().replace('gui_port = 40900', f'gui_port = {port}')
            enabled = [True] + [bool(mask & (1 << bit)) for bit in range(4)]
            for rx in range(2, 6):
                text = text.replace(f'rx{rx}_enabled = 1', f'rx{rx}_enabled = {int(enabled[rx-1])}')
            if mask == 15: text = text.replace('digital_auto_idle = 1', 'digital_auto_idle = 0')
            config.write_text(text)
            backend = subprocess.Popen([sys.argv[1], '--config', str(config), '--source', 'synthetic'],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.bind(('127.0.0.1', 0))
                client.settimeout(.03)
                destination = ('127.0.0.1', port)
                audio, spectra, replies, settings = {}, set(), {}, {}
                last_subscription = 0

                def collect(seconds, predicate=lambda: False, request=None):
                    nonlocal last_subscription
                    end = time.monotonic() + seconds
                    last_request = 0
                    while time.monotonic() < end:
                        now = time.monotonic()
                        if now - last_subscription > .1:
                            client.sendto(subscription(), destination)
                            last_subscription = now
                        if request is not None and now - last_request > .1:
                            client.sendto(request, destination)
                            last_request = now
                        try:
                            packet = client.recv(65536)
                        except socket.timeout:
                            assert backend.poll() is None, 'Backend exited with disabled RX'
                            continue
                        h = HEADER.unpack_from(packet)
                        if h[2] == 5:
                            audio.setdefault(h[4], []).append(h)
                        elif h[2] == 2:
                            spectra.add(h[4])
                        elif h[2] == 4:
                            replies[h[4]] = h
                        elif h[2] == 18:
                            value = settings_state(packet)
                            assert value is not None
                            settings[value[0]] = value
                        if predicate():
                            return
                    if request is not None:
                        assert predicate(), f'Missing control response, RX mask {mask}'

                try:
                    collect(2, lambda: 10 in settings, settings_control(10))
                    _, session, revision, status, _, values = settings[10]
                    assert status == 0
                    assert values['digital_activity_control'] == ('0' if mask == 15 else '1')
                    demand = HEADER.pack(b'QTR2',1,20,64,7,1,0,0,0,0,0,0 if mask == 15 else 3,0,0,0,session)
                    client.sendto(demand,destination)  # Auto-idle off ignores zero demand; manual RX off wins over positive demand.
                    # Disabled RX2 must still acknowledge tuning: WebGUI bootstrap needs it.
                    collect(2, lambda: 2 in replies, tune(1, 10489601000, 2, session))
                    assert replies[2][8] == 10489601000 and replies[2][13] == 0
                    # Monitor settings trigger resets; null DSP must not be dereferenced.
                    patch = {key: int(values[key]) + 1 for key in
                             ('rx3_frequency_hz', 'rx4_frequency_hz', 'rx5_frequency_hz')}
                    # Tuning changes the revision, so refresh before the patch.
                    collect(2, lambda: 11 in settings, settings_control(11))
                    revision = settings[11][2]
                    collect(2, lambda: 12 in settings, settings_control(12, session, revision, patch))
                    assert settings[12][3] == 0, settings[12]
                    collect(.3)
                    expected = {stream for stream, on in zip((1, 2, 5, 6, 8), enabled) if on}
                    assert set(audio) == expected, (mask, set(audio), expected)
                    assert spectra == {0, 1, 2}, 'Disabling demodulation removed a waterfall'
                    for stream, packets in audio.items():
                        assert len(packets) >= 5, (mask, stream, len(packets))
                        for a, b in zip(packets, packets[1:]):
                            if a[5] == b[5]:
                                assert b[7] == a[7] + 240, 'Enabled receiver PCM interrupted'
                    threads = {p.joinpath('comm').read_text().strip()
                               for p in Path(f'/proc/{backend.pid}/task').iterdir()}
                    expected_threads = {'qo100_backend', 'qo100-tx', 'qo100-rx1'}
                    if enabled[1] or enabled[2]:
                        expected_threads.add('qo100-rx2-3')
                    if enabled[3] or enabled[4]:
                        expected_threads.add('qo100-rx4-5')
                    assert threads == expected_threads, (mask, threads)
                    saved = config.with_suffix('.settings').read_text()
                    assert '_enabled=' not in saved, 'GUI persistence took ownership of manual switches'
                finally:
                    backend.terminate()
                    output, _ = backend.communicate(timeout=3)
                    if sys.exc_info()[0]:
                        print(output)
                assert backend.returncode == 0, output
    print('RX2..RX5 switches: PCM absent, empty workers absent, all waterfalls/control/settings and remaining PCM preserved')


if __name__ == '__main__':
    main()
