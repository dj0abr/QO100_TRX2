"""Feed an independent Martin M1 waveform over RX2 UDP and inspect the WebSocket output."""
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import urllib.request

from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


class MartinSignal:
    def __init__(self):
        header = [(1900, 300), (1200, 10), (1900, 300), (1200, 30)]
        header += [(1100 if (44 + 128) & (1 << bit) else 1300, 30) for bit in range(8)]
        header += [(1200, 30)]
        line = [(1200, 4.862), (1500, .572), (1740, 146.432), (1500, .572),
                (2140, 146.432), (1500, .572), (1900, 146.432), (1500, .572)]
        time = 0
        self.tones = []
        for frequency, duration in header + line * 32:
            time += duration
            self.tones.append((frequency, round(time * 12)))
        self.segment = self.index = 0
        self.phase = 0

    def audio(self, offset):
        samples = []
        for _ in range(240):
            while self.segment < len(self.tones) and self.index >= self.tones[self.segment][1]:
                self.segment += 1
            if self.segment == len(self.tones):
                samples.append(0)
            else:
                hz = self.tones[self.segment][0] + offset
                samples.append(round(22000 * math.sin(self.phase)))
                self.phase = (self.phase + math.tau * hz / 12000) % math.tau
            self.index += 1
        return struct.pack('!240h', *samples)


def json_type(kind):
    return lambda message: isinstance(message, dict) and message.get('type') == kind


def media(client, kind):
    return [p for p in list(client.messages) if isinstance(p, bytes) and p[5] == kind]


def main():
    adapter, backend, defaults, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-sstv-rx-test-') as temp:
        root = Path(temp)
        backend_port, http_port = port(), port()
        config = root / 'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        proxy = Proxy(backend_port)
        proxy.drop_once = False
        signal = MartinSignal()
        carrier = None
        audio_clock = []
        packets_to_drop = 0
        dropped = []

        def inject(packet):
            nonlocal packets_to_drop
            if packet[5] != 5 or HEADER.unpack_from(packet)[4] != 2:
                return False
            if carrier is None:
                return True
            fields = HEADER.unpack_from(packet)
            audio_clock.append((fields[5], fields[7]))
            pcm = signal.audio(carrier - fields[8])
            if packets_to_drop:
                packets_to_drop -= 1
                dropped.append(fields[7])
                return True
            proxy.socket.sendto(packet[:64] + pcm, proxy.peer)
            return True

        proxy.drop = inject
        clients = []
        with (root / 'backend.log').open('w+') as radio_log, (root / 'bridge.log').open('w+') as bridge_log:
            radio = subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'],
                                     stdout=radio_log, stderr=radio_log)
            bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
                '--backend-port', str(proxy.port), '--web-root', str(assets)], stdout=bridge_log, stderr=bridge_log)
            try:
                def online():
                    try:
                        with urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health', timeout=.3) as response:
                            return response.status == 200
                    except OSError:
                        return False

                wait(online)
                for role in ('operator', 'viewer'):
                    client = Client(http_port)
                    clients.append(client)
                    client.role(role)
                    client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': False})
                owner, viewer = clients
                initial = owner.state(lambda s: s.get('ready') and '2' in s.get('rx', {}))

                def activity():
                    return [HEADER.unpack_from(p)[11] for p in proxy.log if p[5] == 20][-1:]

                wait(lambda: activity() == [0])
                carrier = initial['rx']['2']['frequency_hz'] + 80
                viewer.send({'type': 'sstv_subscribe', 'enabled': True})
                wait(lambda: activity() == [1])
                spectrum = viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 22)
                assert len(spectrum) == 406 and HEADER.unpack_from(spectrum)[9] == 12000
                # During the leader, 1900 + 80 Hz should be the strongest bin.
                wait(lambda: any(abs(max(range(342), key=lambda i: p[64+i]) * 12000/1024 - 1980) < 16
                                 for p in media(viewer, 22)))
                begin = viewer.wait(json_type('sstv_begin'))
                assert begin['mode'] == 'M1' and not begin['partial'], begin
                viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 21)
                assert not media(owner, 21) and not media(owner, 22)
                assert not any(HEADER.unpack_from(p)[2:5:2] == (5, 2) for p in list(viewer.messages)
                               if isinstance(p, bytes)), 'SSTV unexpectedly enabled browser audio'

                owner.send({'type': 'sstv_subscribe', 'enabled': True})
                assert owner.wait(json_type('sstv_begin')) == begin
                tuned = carrier - 20
                owner.send({'type': 'tune', 'id': 1, 'stream': 2, 'frequency_hz': tuned})
                changed = owner.state(lambda s: s.get('rx', {}).get('2', {}).get('frequency_hz') == tuned)
                epoch = changed['rx']['2']['epoch']
                assert changed['tx2']['frequency_hz'] == initial['tx2']['frequency_hz']

                def line_with_new_offset(p):
                    return (isinstance(p, bytes) and p[5] == 21 and
                            struct.unpack_from('!I', p, 12)[0] == begin['image'] and
                            abs(struct.unpack_from('!i', p, 44)[0] - 20) <= 4)

                line = owner.wait(line_with_new_offset)
                row = HEADER.unpack_from(line)[7]
                assert row > 0 and len(line) == 1024
                assert all(abs(a-b) < 5 for a, b in zip(line[64 + 480:64 + 483], (128, 77, 204)))
                timeline = audio_clock[:]
                assert {entry[0] for entry in timeline} == {epoch}
                assert all(b[1] == a[1] + 240 for a, b in zip(timeline, timeline[1:]))
                packets_to_drop = 2
                wait(lambda: len(dropped) == 2)
                owner.wait(lambda p: line_with_new_offset(p) and
                           HEADER.unpack_from(p)[7] >= row + 2 and struct.unpack_from('!I', p, 48)[0] == 1)
                assert len([p for p in list(viewer.messages) if json_type('sstv_begin')(p)]) == 1

                viewer.send({'type': 'sstv_mode', 'id': 2, 'mode': 'S1'})
                assert viewer.wait(lambda p: json_type('error')(p) and p['id'] == 2)['code'] == 'read_only'
                owner.send({'type': 'sstv_mode', 'id': 3, 'mode': 'S1'})
                assert owner.wait(lambda p: json_type('ack')(p) and p['id'] == 3)['status'] == 0
                viewer.state(lambda s: s.get('sstv_mode') == 'S1')
                for client in clients:
                    client.send({'type': 'sstv_subscribe', 'enabled': False})
                wait(lambda: activity() == [0])
                print('SSTV RX2 subscription, live spectrum and RGB, viewer access and RX2-only retune passed')
            except Exception:
                for log in (radio_log, bridge_log):
                    log.seek(0)
                    print(log.read())
                raise
            finally:
                for client in clients:
                    client.close()
                for process in (bridge, radio):
                    process.terminate()
                    process.wait(timeout=6)
                proxy.close()


if __name__ == '__main__':
    main()
