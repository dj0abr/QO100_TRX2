"""Recorded beacon -> RX4 UDP -> live service -> adapter -> two browsers."""
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import urllib.request
from test_webbridge import HEADER, Proxy, port, private_config, wait
from test_beacon_decoder import recording12
from ws_client import Client


def main():
    adapter, backend, receiver, defaults, assets = map(Path, sys.argv[1:])
    audio = recording12()[:40 * 12000]
    with tempfile.TemporaryDirectory(prefix='qo100-beacon-') as temp:
        root = Path(temp)
        backend_port, http_port, audio_port, text_port = [port() for _ in range(4)]
        config = root/'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        logs = [(root/name).open('w') for name in ('backend.log', 'adapter.log', 'receiver.log')]
        radio = subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'],
                                 stdout=logs[0], stderr=logs[0])
        proxy = Proxy(backend_port)
        proxy.drop = lambda p: p[5] == 5 and HEADER.unpack_from(p)[4] == 6
        proxy.drop_once = False
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
            '--backend-port', str(proxy.port), '--web-root', str(assets),
            '--rx4-decoder-audio-port', str(audio_port), '--rx4-decoder-text-port', str(text_port)],
            stdout=logs[1], stderr=logs[1])
        decoder = subprocess.Popen([str(receiver), '--port', str(audio_port)], stdout=logs[2], stderr=logs[2])
        clients = []
        def text(client):
            return ''.join(m['text'] for m in list(client.messages)
                           if isinstance(m, dict) and m.get('type') == 'decoder_text')
        try:
            wait(lambda: 'PSK beacon receiver: RX4' in (root/'receiver.log').read_text())
            def available():
                try:
                    return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health', timeout=.2).status == 200
                except OSError:
                    return False
            wait(available)
            owner = Client(http_port); clients.append(owner); owner.role('operator'); state = owner.state()
            viewer = Client(http_port); clients.append(viewer); viewer.role('viewer', True); viewer.state()
            epoch, session = state['rx']['4']['epoch'], state['session']

            def feed(index, frequency=10489748821, end=40*12000, drop=None):
                for start in range(0, end, 240):
                    if start == drop:
                        continue
                    packet = HEADER.pack(b'QTR2', 1, 5, 64, 6, epoch, 0, index+start, frequency,
                                         12000, 0, 0, 240, 2, 240, session)
                    proxy.socket.sendto(packet + audio[start:start+240].astype('>i2').tobytes(), proxy.peer)
                    if start % 2400 == 0:
                        time.sleep(.005)

            feed(0)
            for client in clients:
                wait(lambda: 'FEC/RS OK' in text(client) and 'CRC OK' in text(client), timeout=18)
                assert 'HI de Qatar-OSCAR 100' in text(client)
                wait(lambda: any(isinstance(p, bytes) and p[5] == 5 and HEADER.unpack_from(p)[4] == 1
                                 for p in client.messages))
                assert not any(isinstance(p, bytes) and p[5] == 5 and HEADER.unpack_from(p)[4] == 6
                               for p in client.messages), 'RX4 PCM leaked to browser'
            # Let the latest overlapping window finish: only one copy of each real frame.
            wait(lambda: (root/'receiver.log').read_text().count('Window ') >= 2, timeout=12)
            time.sleep(.3)
            first = text(owner)
            assert first.count('[PSK M | CRC OK') == 1, first
            assert first.count('[PSK M | FEC/RS OK') == 1, first
            feed(0)  # old sample indices must be ignored
            time.sleep(.3)
            assert text(owner) == first
            # A soft retune invalidates even with the same session and epoch.
            feed(40*12000, frequency=10489748820, end=12000)
            # Gap + recovery: the same content at a new time is a new broadcast.
            feed(42*12000)
            for client in clients:
                wait(lambda: text(client).count('[PSK M | FEC/RS OK') == 2, timeout=18)
            assert decoder.poll() is None
            print('Beacon UDP: CRC/FEC text to operator and viewer, RX1 continuous, no RX4 PCM '
                  'in browser, overlap/replay suppression, retune and gap recovery passed')
        finally:
            for client in clients:
                client.close()
            for process in (decoder, bridge, radio):
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=8)
            proxy.close()
            for log in logs:
                log.close()
            if sys.exc_info()[0]:
                print((root/'receiver.log').read_text())
                print((root/'adapter.log').read_text())
        assert decoder.returncode == bridge.returncode == radio.returncode == 0


if __name__ == '__main__':
    main()
