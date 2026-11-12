"""Exercise RTTY over a private synthetic backend and a TX2-to-RX2 PCM echo."""
from collections import deque
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

from test_sstv_tx import packet
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def json_type(kind):
    return lambda message: isinstance(message, dict) and message.get('type') == kind


def main():
    bridge_bin, backend_bin, defaults, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-rtty-bridge-') as tmp:
        root = Path(tmp)
        backend_port, http, audio_port, bridge_port = (port() for _ in range(4))
        config = root / 'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        proxy = Proxy(backend_port)
        proxy.drop_once = False
        echo = deque()
        cursor = 0
        drop_rx = False

        def tx_packets():
            return [p for p in proxy.log if p[5] == 8 and p[11] == 7]

        def echo_audio(data):
            nonlocal cursor, drop_rx
            if data[5] != 5 or data[11] != 2:
                return False
            end = len(proxy.log)
            echo.extend(p[64:] for p in proxy.log[cursor:end] if p[5] == 8 and p[11] == 7)
            cursor = end
            pcm = echo.popleft() if echo else bytes(480)
            if drop_rx:
                drop_rx = False
            else:
                proxy.socket.sendto(data[:64] + pcm, proxy.peer)
            return True

        proxy.drop = echo_audio
        with (root / 'radio.log').open('w+') as radio_log, (root / 'bridge.log').open('w+') as bridge_log:
            radio = subprocess.Popen([str(backend_bin), '--config', str(config), '--source', 'synthetic'],
                                     stdout=radio_log, stderr=radio_log)
            bridge = subprocess.Popen([str(bridge_bin), '--listen', '127.0.0.1', '--port', str(http),
                '--backend-port', str(proxy.port), '--web-root', str(assets),
                '--digital-audio-port', str(audio_port), '--digital-bridge-port', str(bridge_port)],
                stdout=bridge_log, stderr=bridge_log)
            clients = []
            digital = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            digital.bind(('127.0.0.1', audio_port))
            digital.connect(('127.0.0.1', bridge_port))
            digital.settimeout(.2)
            try:
                def online():
                    try:
                        return urllib.request.urlopen(f'http://127.0.0.1:{http}/health', timeout=.3).status == 200
                    except OSError:
                        return False

                wait(online)
                for role in ('operator', 'viewer'):
                    client = Client(http)
                    clients.append(client)
                    client.role(role)
                    client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': False})
                owner, viewer = clients
                initial = owner.state()
                frequency = initial['tx2']['frequency_hz']
                session = initial['session']
                assert initial['rtty_tx']['phase'] == 'idle'
                assert initial['rtty_reverse'] is False
                request_id = 0

                def request(client, kind, **values):
                    nonlocal request_id
                    request_id += 1
                    client.send({'type': kind, 'id': request_id, **values})
                    return client.wait(lambda p: isinstance(p, dict) and p.get('id') == request_id and
                                       p.get('type') in ('ack', 'error'))

                def send_text(client=owner, text='CQ CQ 599', level=50):
                    return request(client, 'rtty_tx_start', frequency_hz=frequency, text=text, level=level)

                def start_sstv():
                    return request(owner, 'sstv_tx_start', frequency_hz=frequency, level=50)

                def activity_flags():
                    return next((HEADER.unpack_from(p)[11] for p in reversed(proxy.log) if p[5] == 20), None)

                assert send_text(viewer)['code'] == 'read_only'
                assert request(viewer, 'rtty_reverse', enabled=True)['code'] == 'read_only'
                viewer.send({'type': 'rtty_subscribe', 'enabled': True})
                reset = viewer.wait(json_type('rtty_reset'))
                assert reset['session'] == session
                wait(lambda: activity_flags() == 1)
                spectrum = viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 24)
                fields = HEADER.unpack_from(spectrum[:64])
                assert fields[4] == 2 and fields[9] == 12000 and fields[12] == 342
                assert fields[14] == 1024 and fields[15] == session and len(spectrum) == 64 + 342
                messages = list(viewer.messages)
                assert not any(isinstance(p, bytes) and p[5] == 5 and p[11] == 2
                               for p in messages[messages.index(reset) + 1:])

                def reset_count():
                    return sum(map(json_type('rtty_reset'), list(viewer.messages)))

                resets = reset_count()
                assert request(owner, 'rtty_reverse', enabled=True)['status'] == 0
                owner.state(lambda s: s.get('rtty_reverse') is True)
                assert request(owner, 'rtty_reverse', enabled=False)['status'] == 0
                owner.state(lambda s: s.get('rtty_reverse') is False and s is not initial)
                wait(lambda: reset_count() >= resets + 2)

                def spectrum_count():
                    return sum(isinstance(p, bytes) and p[5] == 24 for p in list(viewer.messages))

                spectra = spectrum_count()
                wait(lambda: spectrum_count() > spectra)
                resets = reset_count()
                drop_rx = True
                wait(lambda: reset_count() > resets)

                assert send_text(text='@')['code'] == 'invalid'
                assert send_text(text=' ')['code'] == 'invalid'
                assert send_text(text='Ä' * 257)['code'] == 'invalid'
                assert send_text(level=0)['code'] == 'invalid'
                assert start_sstv()['status'] == 0
                assert request(owner, 'rtty_tx_stop')['status'] == 0
                assert send_text()['code'] == 'tx2_busy'
                owner.send({'type': 'sstv_tx_stop'})
                owner.state(lambda s: s['sstv_tx'].get('result') == 'stopped')

                nonce = 54321
                digital.send(packet(19, 7, nonce=nonce))

                def acknowledged():
                    try:
                        data = digital.recv(1200)
                        return data[5] == 7 and HEADER.unpack_from(data)[6] == nonce
                    except (TimeoutError, ConnectionRefusedError):
                        return False

                wait(acknowledged)
                digital.send(packet(20, 7, session=session, nonce=nonce, flags=2))
                wait(lambda: activity_flags() == 3)
                assert send_text()['code'] == 'tx2_busy'
                digital.send(packet(20, 7, session=session, nonce=nonce, flags=0))
                wait(lambda: activity_flags() == 1)

                message = 'CQ CQ DE DJ0ABR 599'
                accepted = send_text(text=message.lower())
                assert accepted['status'] == 0 and accepted['text'] == message
                sending = owner.state(lambda s: s['rtty_tx']['phase'] == 'sending')
                assert sending['rtty_tx']['tx_id'] == accepted['id']
                assert request(owner, 'rtty_tx_stop', tx_id=accepted['id'] + 1)['status'] == 0
                tx_epoch = sending['tx2']['epoch']
                assert start_sstv()['code'] == 'tx2_busy'
                owner.send({'type': 'sstv_tx_stop'})
                assert request(viewer, 'rtty_tx_stop')['code'] == 'read_only'
                # Inject a valid external PCM packet in the current TX2 epoch.
                foreign_index = 240 * 5000
                digital.send(HEADER.pack(b'QTR2', 1, 8, 64, 7, tx_epoch, 0, foreign_index,
                                         frequency, 12000, 0, 0, 240, 2, 240, session) + bytes(480))

                def received_text(client):
                    return ''.join(p['text'] for p in list(client.messages) if json_type('rtty_text')(p))

                wait(lambda: received_text(viewer) == message)
                assert not received_text(owner), 'RTTY text was forwarded without a subscription'
                owner.state(lambda s: s['rtty_tx'].get('result') == 'complete' and s['tx2']['epoch'] > tx_epoch)
                audio = tx_packets()
                assert all(HEADER.unpack_from(p[:64])[5:9] == (tx_epoch, 0, i * 240, frequency)
                           for i, p in enumerate(audio)), 'Foreign PCM or incorrect TX2 epoch/index'
                assert max(abs(sample) for p in audio for sample in struct.unpack('!240h', p[64:])) == 12288
                assert all(p[64:] == bytes(480) for p in audio[-12:])

                owner.messages.clear()
                assert send_text(text='CQ ' * 40)['status'] == 0
                sending = owner.state(lambda s: s['rtty_tx']['phase'] == 'sending')
                tx_epoch = sending['tx2']['epoch']
                assert request(owner, 'rtty_tx_level', level=0)['status'] == 0
                wait(lambda: all(p[64:] == bytes(480) for p in tx_packets()[-2:]))
                assert request(owner, 'rtty_tx_stop', tx_id=sending['rtty_tx']['tx_id'])['status'] == 0
                owner.state(lambda s: s['rtty_tx'].get('result') == 'stopped' and s['tx2']['epoch'] > tx_epoch)
                count = len(tx_packets())
                time.sleep(.25)
                assert len(tx_packets()) == count

                owner.messages.clear()
                assert send_text(text='CQ ' * 40)['status'] == 0
                owner.state(lambda s: s['rtty_tx']['phase'] == 'sending')
                tuned = frequency + 100
                assert request(owner, 'tx', channel=2, ptt=False, frequency_hz=tuned)['status'] == 0
                owner.state(lambda s: s['rtty_tx'].get('result') == 'retuned' and s['tx2']['frequency_hz'] == tuned)

                frequency = tuned
                owner.messages.clear()
                assert request(viewer, 'rtty_tx_open', frequency_hz=frequency, level=50)['code'] == 'read_only'
                opened = request(owner, 'rtty_tx_open', frequency_hz=frequency, level=50)
                assert opened['status'] == 0
                tx_id = opened['id']
                sending = owner.state(lambda s: s['rtty_tx']['phase'] == 'sending')
                tx_epoch = sending['tx2']['epoch']
                assert sending['rtty_tx']['tx_id'] == tx_id

                def hold_until(predicate):
                    next_ping = 0

                    def check():
                        nonlocal next_ping
                        if time.monotonic() >= next_ping:
                            owner.send({'type': 'rtty_tx_keepalive', 'tx_id': tx_id})
                            next_ping = time.monotonic() + .2
                        return predicate()

                    wait(check)

                def live_packets():
                    return [p for p in tx_packets() if HEADER.unpack_from(p)[5] == tx_epoch]

                hold_until(lambda: len(live_packets()) >= 65)
                idle = [sample for p in live_packets() for sample in struct.unpack('!240h', p[64:])]
                for i in range(5):
                    start = 12 * 264 + i * 1980
                    space = idle[start:start + 264]
                    mark = idle[start + 264:start + 1980]
                    assert space[12:] != space[:-12] and max(space) >= 12287, 'Missing Space start bit'
                    assert mark[12:] == mark[:-12] and max(mark) >= 11800, 'Missing Mark data/stop bits'
                rx_before = received_text(viewer)
                assert request(owner, 'rtty_tx_write', tx_id=tx_id + 1, text='X')['code'] == 'not_ready'
                assert request(owner, 'rtty_tx_write', tx_id=tx_id, text='Z' * 513)['code'] == 'tx_queue_full'
                for key, encoded in [('c', 'C'), ('q', 'Q'), (' ', ' '), ('ä', 'AE'), ('1', '1')]:
                    ack = request(owner, 'rtty_tx_write', tx_id=tx_id, text=key)
                    assert ack['status'] == 0 and ack['text'] == encoded
                hold_until(lambda: received_text(viewer) == rx_before + 'CQ AE1')
                assert request(owner, 'rtty_tx_stop', tx_id=tx_id + 1)['status'] == 0
                idle_start = len(live_packets())
                hold_until(lambda: len(live_packets()) >= idle_start + 12)
                lines = ['Amsat Upconverter 6W PA', '2m Dish', 'Adalm Pluto', 'Github QO100 TRX2']
                example = ('\\' + '\n').join(lines) + '\n' + 'Z' * 400
                ack = request(owner, 'rtty_tx_write', tx_id=tx_id, text=example)
                assert ack['status'] == 0 and ack['text'] == '\n'.join(lines).upper() + '\n' + 'Z' * 400
                assert request(owner, 'rtty_tx_stop', tx_id=tx_id)['status'] == 0
                owner.state(lambda s: s['rtty_tx'].get('result') == 'stopped' and
                            s['rtty_tx'].get('tx_id') == tx_id and s['tx2']['epoch'] > tx_epoch)
                count = len(tx_packets())
                time.sleep(.25)
                assert len(tx_packets()) == count, 'Queued text kept transmitting after PTT release'

                opened = request(owner, 'rtty_tx_open', frequency_hz=frequency, level=50)
                assert opened['status'] == 0
                tx_id = opened['id']
                sending = owner.state(lambda s: s['rtty_tx']['phase'] == 'sending' and s['rtty_tx'].get('tx_id') == tx_id)
                owner.state(lambda s: s['rtty_tx'].get('result') == 'ptt_timeout' and
                            s['rtty_tx'].get('tx_id') == tx_id and s['tx2']['epoch'] > sending['tx2']['epoch'])

                owner.messages.clear()
                assert send_text(text='CQ ' * 40)['status'] == 0
                sending = owner.state(lambda s: s['rtty_tx']['phase'] == 'sending')
                owner.close()
                clients.remove(owner)
                viewer.state(lambda s: s['rtty_tx'].get('result') == 'disconnected' and
                             s['tx2']['epoch'] > sending['tx2']['epoch'])
                viewer.send({'type': 'rtty_subscribe', 'enabled': False})
                wait(lambda: activity_flags() == 0)
                print('RTTY subscriptions, RX2 spectrum and gaps, operator rights, TX2 exclusion, echo, live PTT and abort passed')
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
