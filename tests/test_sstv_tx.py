"""SSTV upload, PCM pacing and stop, using a synthetic radio and an RX2 audio echo."""
from collections import deque
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def packet(kind, stream, epoch=0, index=0, session=0, nonce=0, flags=0):
    return HEADER.pack(b'QTR2', 1, kind, 64, stream, epoch, nonce, index, 0, 0, 0, flags, 0, 0, 0, session)


def main():
    bridge_bin, radio_bin, defaults, assets = map(Path, sys.argv[1:5])
    full = '--full' in sys.argv[5:]
    with tempfile.TemporaryDirectory(prefix='qo100-sstv-tx-') as tmp:
        root = Path(tmp)
        backend_port, http, audio_port, bridge_port = (port() for _ in range(4))
        config = root/'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        proxy = Proxy(backend_port)
        echo = deque()
        cursor = 0
        proxy.drop_once = False

        def echo_audio(data):
            nonlocal cursor
            fields = HEADER.unpack_from(data)
            if fields[2] != 5 or fields[4] != 2:
                return False
            # Copy only newly sent TX2 PCM, in order, into the RX2 audio stream.
            echo.extend(p[64:] for p in proxy.log[cursor:] if p[5] == 8 and p[11] == 7)
            cursor = len(proxy.log)
            proxy.socket.sendto(data[:64] + (echo.popleft() if echo else bytes(480)), proxy.peer)
            return True

        proxy.drop = echo_audio
        with (root/'radio.log').open('w+') as radio_log, (root/'bridge.log').open('w+') as bridge_log:
            radio = subprocess.Popen([str(radio_bin), '--config', str(config), '--source', 'synthetic'],
                                     stdout=radio_log, stderr=radio_log)
            bridge = subprocess.Popen([str(bridge_bin), '--listen', '127.0.0.1', '--port', str(http),
                '--backend-port', str(proxy.port), '--web-root', str(assets),
                '--digital-audio-port', str(audio_port), '--digital-bridge-port', str(bridge_port)],
                stdout=bridge_log, stderr=bridge_log)
            clients = []
            digital = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            digital.bind(('127.0.0.1', audio_port))
            digital.connect(('127.0.0.1', bridge_port))
            digital.settimeout(0.2)
            try:
                def online():
                    try:
                        with urllib.request.urlopen(f'http://127.0.0.1:{http}/health', timeout=.3) as response:
                            return response.status == 200
                    except OSError:
                        return False

                wait(online)
                for role in ('operator', 'viewer'):
                    client = Client(http)
                    clients.append(client)
                    if full:
                        client.messages = deque(client.messages, maxlen=65000)
                    client.role(role)
                    client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': False})
                owner, viewer = clients
                initial = owner.state()
                frequency = initial['tx2']['frequency_hz']
                session = initial['session']
                assert initial['sstv_mode'] == 'auto'
                viewer.send({'type': 'sstv_subscribe', 'enabled': True})

                def tx_packets():
                    return [p for p in proxy.log if p[5] == 8 and p[11] == 7]

                def request(client, id):
                    client.send({'type': 'sstv_tx_start', 'id': id, 'frequency_hz': frequency, 'level': 50})
                    return client.wait(lambda p: isinstance(p, dict) and p.get('id') == id and
                                       p.get('type') in ('ack', 'error'))

                def upload(id):
                    raster = bytes((128, 77, 204)) * (320 * 256)
                    for offset in range(0, len(raster), 1024):
                        owner.send(packet(23, 7, id, offset, session) + raster[offset:offset+1024])

                assert request(viewer, 2)['code'] == 'read_only'
                nonce = 54321
                digital.send(packet(19, 7, nonce=nonce))

                def acknowledged():
                    try:
                        return HEADER.unpack_from(digital.recv(1200))[6] == nonce
                    except (TimeoutError, ConnectionRefusedError):
                        return False

                wait(acknowledged)
                digital.send(packet(20, 7, session=session, nonce=nonce, flags=2))
                wait(lambda: any(p[5] == 20 and HEADER.unpack_from(p)[11] & 2 for p in proxy.log))
                assert request(owner, 3)['code'] == 'tx2_busy'
                digital.send(packet(20, 7, session=session, nonce=nonce, flags=0))
                wait(lambda: [HEADER.unpack_from(p)[11] for p in proxy.log if p[5] == 20][-1] == 1)

                assert request(owner, 4)['status'] == 0
                owner.send({'type': 'sstv_tx_stop'})
                owner.state(lambda s: s.get('sstv_tx', {}).get('result') == 'stopped')
                assert not tx_packets(), 'Incomplete image triggered TX'

                assert request(owner, 5)['status'] == 0
                upload(5)
                owner.state(lambda s: s.get('sstv_tx', {}).get('phase') == 'sending')
                begin = viewer.wait(lambda p: isinstance(p, dict) and p.get('type') == 'sstv_begin')
                assert begin['mode'] == 'M1', begin
                line = viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 21)
                assert all(abs(a-b) < 5 for a,b in zip(line[64+480:64+483], (128,77,204))), 'wrong RGB output'
                audio = tx_packets()
                assert all(HEADER.unpack_from(p)[8] == frequency for p in audio)
                assert all(HEADER.unpack_from(p)[7] == i*240 for i,p in enumerate(audio))
                def peak(p):
                    return max(abs(x) for x in struct.unpack('!240h', p[64:]))

                assert max(map(peak, audio)) == 12288
                tx_epoch = HEADER.unpack_from(audio[0])[5]

                if full:
                    owner.state(lambda s: s.get('sstv_tx', {}).get('result') == 'complete', timeout=125)
                    done = viewer.wait(lambda p: isinstance(p, dict) and p.get('type') == 'sstv_end', timeout=4)
                    assert done['complete'], done
                    assert len([p for p in list(viewer.messages) if isinstance(p, bytes) and p[5] == 21]) == 256
                    assert all(p[64:] == bytes(480) for p in tx_packets()[-12:])
                else:
                    viewer.send({'type': 'sstv_tx_level', 'id': 6, 'level': 100})
                    denied = viewer.wait(lambda p: isinstance(p, dict) and p.get('id') == 6)
                    assert denied['code'] == 'read_only'
                    for level, expected in ((25, 6144), (0, 0), (75, 18432)):
                        count = len(tx_packets())
                        owner.send({'type': 'sstv_tx_level', 'level': level})

                        def new_level():
                            packets = tx_packets()[count:]
                            return len(packets) >= 2 and all(abs(peak(p) - expected) <= 1 for p in packets[-2:])

                        wait(new_level, timeout=1)
                    for i, p in enumerate(tx_packets()):
                        fields = HEADER.unpack_from(p)
                        assert fields[5] == tx_epoch and fields[7] == i * 240

                    owner.send({'type': 'sstv_tx_stop'})
                    owner.state(lambda s: s.get('sstv_tx', {}).get('phase') == 'idle' and
                                s.get('tx2', {}).get('epoch', 0) > tx_epoch)
                    count = len(tx_packets())
                    time.sleep(.35)
                    assert len(tx_packets()) == count, 'TX2 audio continued after Stop'
                print('SSTV TX: automatic mode, ownership, busy detection, PCM level, full-duplex RX and ' +
                      ('complete image with tail' if full else 'live level changes and abort with new epoch') + ' passed')
            except Exception:
                for log in (radio_log, bridge_log):
                    log.seek(0)
                    print(log.read())
                raise
            finally:
                for client in clients:
                    client.close()
                digital.close()
                for process in (bridge, radio):
                    process.terminate()
                    process.wait(timeout=6)
                proxy.close()


if __name__ == '__main__':
    main()
