"""Decode real FT8 and PSK audio concurrently through one bridge and two browsers."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
from test_webbridge import HEADER, Proxy, port, private_config, wait
from test_beacon_decoder import recording12
from ws_client import Client


def main():
    adapter, backend, ft8_receiver, beacon_receiver, generator, defaults, assets = map(Path, sys.argv[1:])
    beacon = recording12()[:40*12000]
    with tempfile.TemporaryDirectory(prefix='qo100-parallel-decodes-') as temp:
        root = Path(temp)
        backend_port, http_port, audio3, text3, audio4, text4 = [port() for _ in range(6)]
        config = root/'backend.conf'; config.write_text(private_config(defaults, backend_port))
        raw = root/'ft8.raw'
        subprocess.run([str(generator), str(raw), 'CQ K1ABC FN42', '1500', '0.2', '-12'], check=True, timeout=10)
        ft8 = struct.unpack('<180000h', raw.read_bytes())
        logs = [(root/name).open('w') for name in ('backend.log', 'bridge.log', 'ft8.log', 'psk.log')]
        radio = subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'], stdout=logs[0], stderr=logs[0])
        proxy = Proxy(backend_port)
        proxy.drop = lambda p: p[5] == 5 and HEADER.unpack_from(p)[4] in (5, 6)
        proxy.drop_once = False
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
            '--backend-port', str(proxy.port), '--web-root', str(assets),
            '--rx3-decoder-audio-port', str(audio3), '--rx3-decoder-text-port', str(text3),
            '--rx4-decoder-audio-port', str(audio4), '--rx4-decoder-text-port', str(text4)], stdout=logs[1], stderr=logs[1])
        decoder3 = subprocess.Popen([str(ft8_receiver), '--port', str(audio3)], stdout=logs[2], stderr=logs[2])
        decoder4 = subprocess.Popen([str(beacon_receiver), '--port', str(audio4)], stdout=logs[3], stderr=logs[3])
        clients = []
        try:
            wait(lambda: 'waiting for a complete UTC slot' in (root/'ft8.log').read_text() and 'PSK beacon receiver: RX4' in (root/'psk.log').read_text())
            def available():
                try:
                    return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health', timeout=.2).status == 200
                except OSError:
                    return False
            wait(available)
            owner = Client(http_port); clients.append(owner); owner.role('operator'); state = owner.state()
            viewer = Client(http_port); clients.append(viewer); viewer.role('viewer', True); viewer.state()
            for start in range(0, len(beacon), 240):
                for channel, stream, pcm in ((3, 5, ft8), (4, 6, beacon)):
                    if start >= len(pcm):
                        continue
                    rx = state['rx'][str(channel)]
                    timestamp = 1700000010*10**9+(start+240)*10**9//12000
                    packet = HEADER.pack(b'QTR2', 1, 5, 64, stream, rx['epoch'], timestamp, start,
                                         rx['frequency_hz'], 12000, 0, 0, 240, 2, 240, state['session'])
                    packet += struct.pack('!240h', *pcm[start:start+240])
                    proxy.socket.sendto(packet, proxy.peer)
                if start % 2400 == 0:
                    time.sleep(.005)
            def text(client, channel):
                return ''.join(m['text'] for m in list(client.messages)
                               if isinstance(m, dict) and m.get('type') == 'decoder_text' and m['stream'] == channel)
            for client in clients:
                wait(lambda: 'CQ K1ABC FN42' in text(client, 3) and 'CRC OK' in text(client, 4) and 'FEC/RS OK' in text(client, 4), timeout=22)
                assert 'HI de Qatar-OSCAR 100' in text(client, 4)
                assert 'PSK' not in text(client, 3) and 'K1ABC' not in text(client, 4), 'Decoder sources mixed'
                client.wait(lambda p: isinstance(p, bytes) and p[5] == 5 and HEADER.unpack_from(p)[4] == 1)
                assert not any(isinstance(p, bytes) and p[5] == 5 and HEADER.unpack_from(p)[4] in (5, 6) for p in client.messages)
            assert decoder3.poll() is None and decoder4.poll() is None
            print('Parallel real decoders: FT8 message plus CRC/FEC PSK frames at operator and viewer, separate sources, live RX1 and no browser monitor PCM passed')
        finally:
            for client in clients: client.close()
            for process in (decoder3, decoder4, bridge, radio):
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=8)
            proxy.close()
            for log in logs: log.close()
            if sys.exc_info()[0]:
                print((root/'ft8.log').read_text()); print((root/'psk.log').read_text()); print((root/'bridge.log').read_text())
        assert decoder3.returncode == decoder4.returncode == bridge.returncode == radio.returncode == 0


if __name__ == '__main__':
    main()
