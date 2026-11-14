"""Waterfall Text PCM, orientation and TX2 ownership with a synthetic backend."""
from contextlib import ExitStack
import cmath
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def verify_pcm(packets, frequency):
    for index, packet in enumerate(packets):
        fields = HEADER.unpack_from(packet)
        assert fields[7:11] == (index * 240, frequency, 12000, 0), fields
        assert fields[5] == HEADER.unpack_from(packets[0])[5]
    samples = [sample for p in packets for sample in struct.unpack('!240h', p[64:])]
    mask_r = (511, 33, 33, 33, 97, 146, 268)  # R, left to right, least significant bit at the top
    underscore = (1 << 8,) * 7  # Bottom row of the unrotated font
    masks = (*mask_r, 0, *underscore, 0, 511, 17, 17, 17, 17, 17, 1)  # R_F
    row_samples = 1200
    period = 480
    for index, mask in enumerate(masks):
        row = samples[index * row_samples:(index + 1) * row_samples]
        assert len(row) == row_samples and row[0] == row[-1] == 0
        if not mask:
            assert not any(row)
            continue
        window = row[360:360 + period]
        for band in range(9):
            for tone in range(7):
                hz = 525 + band * 225 + tone * 25
                angle = 2j * math.pi * hz / 12000
                amplitude = abs(sum(sample * cmath.exp(angle * n) for n, sample in enumerate(window))) / period
                assert (amplitude > 50) == bool(mask & (1 << band)), (index, hz, amplitude)
    assert max(map(abs, samples)) <= 12288
    tail = samples[len(masks) * row_samples:]
    assert len(tail) == 13 * 240 and not any(tail)


def main():
    bridge_bin, radio_bin, defaults, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-waterfall-tx-') as tmp, ExitStack() as stack:
        root = Path(tmp)
        backend_port, http_port = port(), port()
        config = root / 'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        proxy = Proxy(backend_port)
        stack.callback(proxy.close)
        for name, command in [
            ('radio', [radio_bin, '--config', config, '--source', 'synthetic']),
            ('bridge', [bridge_bin, '--listen', '127.0.0.1', '--port', str(http_port),
                        '--backend-port', str(proxy.port), '--web-root', assets]),
        ]:
            log = stack.enter_context((root / f'{name}.log').open('w+'))
            process = subprocess.Popen(command, stdout=log, stderr=log)

            def stop(process=process, log=log):
                process.terminate()
                process.wait(timeout=6)
                if sys.exc_info()[0]:
                    log.seek(0)
                    print(log.read())

            stack.callback(stop)

        def online():
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{http_port}/waterfall-font.json', timeout=.3) as response:
                    return response.status == 200
            except OSError:
                return False

        wait(online)
        owner, viewer = Client(http_port), Client(http_port)
        for client, role in ((owner, 'operator'), (viewer, 'viewer')):
            stack.callback(client.close)
            client.role(role)
            client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': False})
        frequency = owner.state()['tx2']['frequency_hz']

        def request(client, request_id, kind='waterfall_tx_start', **overrides):
            message = dict(type=kind, id=request_id, frequency_hz=frequency, level=50)
            if kind == 'waterfall_tx_start':
                message.update(text='rf', row_ms=100)
            client.send({**message, **overrides})
            return client.wait(lambda p: isinstance(p, dict) and p.get('id') == request_id)

        def tx_state(tx_id, **fields):
            return viewer.state(lambda s: s['waterfall_tx'].get('tx_id') == tx_id and
                                all(s['waterfall_tx'].get(key) == value for key, value in fields.items()))

        def tx_packets():
            return [p for p in proxy.log if p[5] == 8 and HEADER.unpack_from(p)[4] == 7]

        def activity(start=0):
            return [HEADER.unpack_from(p)[11] for p in proxy.log[start:] if p[5] == 20]

        wait(lambda: activity() and activity()[-1] == 0)
        assert request(viewer, 1)['code'] == 'read_only'
        assert request(owner, 2, row_ms=123)['code'] == 'invalid'
        assert request(owner, 3, text='RF!')['code'] == 'invalid'
        assert request(owner, 4, text='   ')['code'] == 'invalid'
        assert not tx_packets()

        # Waterfall Text may send anywhere within the backend's TX2 tuning range.
        frequency = 2400040000
        owner.send({'type': 'tx', 'id': 5, 'channel': 2, 'frequency_hz': frequency})
        owner.state(lambda s: s['tx2'].get('frequency_hz') == frequency)

        start = len(proxy.log)
        accepted = request(owner, 8, text='r_f')
        assert accepted.get('status') == 0, accepted
        tx_state(8, phase='sending')
        assert request(owner, 9, kind='sstv_tx_start')['code'] == 'tx2_busy'
        owner.send({'type': 'waterfall_tx_stop', 'tx_id': 7})
        tx_state(8, phase='idle', result='complete')
        wait(lambda: activity(start) and activity(start)[-1] == 0)
        assert set(activity(start)) == {0, 2}, activity(start)
        verify_pcm(tx_packets(), frequency)

        for id_, stop_message, result in (
            (10, {'type': 'waterfall_tx_stop', 'tx_id': 10}, 'stopped'),
            (11, {'type': 'tx', 'channel': 2, 'frequency_hz': frequency + 100}, 'retuned'),
            (12, None, 'disconnected'),
        ):
            accepted = request(owner, id_, row_ms=400)
            assert accepted.get('status') == 0, accepted
            sending = tx_state(id_, phase='sending')
            if stop_message:
                owner.send(stop_message)
            else:
                owner.close()
            viewer.state(lambda s: s['waterfall_tx'].get('result') == result and
                         s['tx2']['epoch'] != sending['tx2']['epoch'])
            count = len(tx_packets())
            time.sleep(.06)
            assert len(tx_packets()) == count
            if result == 'retuned':
                frequency += 100
        print('Waterfall Text: glyph orientation, bands, timing, TX2 activity, ownership and cancellation passed')


if __name__ == '__main__':
    main()
