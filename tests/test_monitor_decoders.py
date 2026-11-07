"""Four independent RXs, parallel monitor links, persistent Setup tuning and WS text."""
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def main():
    adapter, backend, dummy, defaults, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-monitors-') as temp:
        root = Path(temp)
        backend_port, http_port, audio3, text3, audio4, text4 = [port() for _ in range(6)]
        config = root/'backend.conf'
        config.write_text(private_config(defaults, backend_port)
                          .replace('rx3_frequency_hz = 10489540000', 'rx3_frequency_hz = 10489600000')
                          .replace('rx4_frequency_hz = 10489748821', 'rx4_frequency_hz = 10489748500'))
        logs = [(root/name).open('w') for name in ('backend.log', 'bridge.log', 'dummy.log')]
        def start_radio():
            return subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'], stdout=logs[0], stderr=logs[0])
        radio = start_radio()
        proxy = Proxy(backend_port)
        pcm = {s: [] for s in (1, 2, 5, 6)}
        wire = {5: set(), 6: set()}
        def observe(p):
            h = HEADER.unpack_from(p)
            if h[2] == 5 and h[4] in pcm:
                pcm[h[4]].append(h)
                if h[4] in wire:
                    wire[h[4]].add(p)
            assert h[2] != 2 or h[4] in (0, 1, 2), 'Monitoring FFT created'
            return False
        proxy.drop = observe
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
            '--backend-port', str(proxy.port), '--web-root', str(assets),
            '--rx3-decoder-audio-port', str(audio3), '--rx3-decoder-text-port', str(text3),
            '--rx4-decoder-audio-port', str(audio4), '--rx4-decoder-text-port', str(text4)], stdout=logs[1], stderr=logs[1])
        taps = {}
        clients = []
        decoder = None
        try:
            def available():
                try:
                    return urllib.request.urlopen(f'http://127.0.0.1:{http_port}/health', timeout=.2).status == 200
                except OSError:
                    return False
            wait(available)
            time.sleep(.3)  # Missing decoders/ICMP must not stop the adapter.
            for stream, audio_port in ((5, audio3), (6, audio4)):
                taps[stream] = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                taps[stream].bind(('127.0.0.1', audio_port))
                taps[stream].settimeout(.2)

            def audio(stream, frequency=None, epoch=None):
                deadline = time.monotonic()+4
                while time.monotonic() < deadline:
                    try:
                        p, source = taps[stream].recvfrom(65536)
                    except socket.timeout:
                        continue
                    h = HEADER.unpack_from(p)
                    assert len(p) == 544 and h[:5] == (b'QTR2', 1, 5, 64, stream), h
                    assert (h[9], h[12], h[13], h[14]) == (12000, 240, 2, 240), h
                    assert source == ('127.0.0.1', text3 if stream == 5 else text4)
                    assert p in wire[stream], 'Monitor PCM/metadata changed in adapter'
                    if (frequency is None or h[8] == frequency) and (epoch is None or h[5] == epoch):
                        return h
                raise AssertionError(f'No monitor audio on stream {stream}')

            # Both monitors run without a browser, with uninterrupted sample counters.
            for stream in taps:
                previous = audio(stream)
                for _ in range(8):
                    current = audio(stream)
                    assert current[7] == previous[7]+240 and current[5] == previous[5]
                    previous = current
            owner, viewer = Client(http_port), Client(http_port)
            clients += [owner, viewer]
            owner.role('operator'); viewer.role('viewer', True)
            initial = owner.state(lambda s: s.get('ready') and all(str(c) in s['rx'] for c in (1, 2, 3, 4)))
            viewer.state()

            def output(stream, text):
                taps[stream].sendto(text.encode(), ('127.0.0.1', text3 if stream == 5 else text4))
                for client in clients:
                    reply = client.wait(lambda s: isinstance(s, dict) and s.get('type') == 'decoder_text' and s.get('text') == text)
                    assert reply['stream'] == (3 if stream == 5 else 4), reply

            output(5, 'FT8 äöü <b>plain text</b>\n')
            output(6, 'PSK beacon text\n')
            output(5, '{"type":"tx","frequency_hz":2400400000,"ptt":true}')
            assert not any(p[5] == 6 and HEADER.unpack_from(p)[13] for p in proxy.log), 'Text became a TX command'
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as stranger:
                stranger.sendto(b'WRONG_SOURCE', ('127.0.0.1', text3))
            taps[5].sendto(b'\xff\xfe', ('127.0.0.1', text3))
            taps[6].sendto(b'OVERSIZE'*600, ('127.0.0.1', text4))
            output(6, 'valid PSK after malformed datagrams')
            assert not any(isinstance(s, dict) and (s.get('text') == 'WRONG_SOURCE' or s.get('text', '').startswith('OVERSIZE'))
                           for c in clients for s in c.messages)

            request = 50
            def patch(values, expected=0):
                nonlocal request
                for _ in range(10):
                    latest = next(s for s in reversed(list(owner.messages)) if isinstance(s, dict) and s.get('type') == 'state' and s.get('ready'))
                    request += 1
                    owner.send({'type': 'settings_set', 'id': request, 'revision': latest['revision'], 'values': values})
                    reply = owner.wait(lambda s: isinstance(s, dict) and s.get('id') == request and s.get('type') in ('ack', 'error'))
                    if reply.get('code') == 'busy' or reply.get('status') == 2:
                        time.sleep(.1)
                        continue
                    assert reply.get('status') == expected, reply
                    return owner.state(lambda s: int(s.get('revision', 0)) >= int(reply['revision']) and all(s.get('settings', {}).get(k) == str(v) for k, v in values.items())) if expected == 0 else reply
                raise AssertionError('Settings remained busy')

            changed = patch({'rx3_frequency_hz': 10489600321})
            updated3 = audio(5, 10489600321, initial['rx']['3']['epoch']+1)
            assert updated3[7] < 24000, 'Monitoring filter/sample clock not restarted'
            assert audio(6)[5] == initial['rx']['4']['epoch'], 'RX3 change reset RX4'
            patch({'rx4_frequency_hz': 10489748821})
            audio(6, 10489748821, initial['rx']['4']['epoch']+1)
            # Validation is atomic and frequencies retain all bits above 32-bit range.
            saved = config.with_suffix('.settings').read_bytes()
            for invalid in (10489000000, -1, 10489600321.5, '18446744073709551615'):
                patch({'rx3_frequency_hz': invalid, 'rx4_frequency_hz': 10489748000}, expected=1)
                assert config.with_suffix('.settings').read_bytes() == saved
            # Normal RX2 tuning cannot move/reset either monitor; no RX2 decoder link exists.
            request += 1
            owner.send({'type': 'tune', 'id': request, 'stream': 2, 'frequency_hz': 10489610000})
            owner.wait(lambda s: isinstance(s, dict) and s.get('id') == request and s.get('type') == 'ack' and s.get('status') == 0)
            assert audio(5, 10489600321)[5] == initial['rx']['3']['epoch']+1
            assert audio(6, 10489748821)[5] == initial['rx']['4']['epoch']+1
            for channel in (3, 4, 5, 6):
                request += 1
                owner.send({'type': 'tune', 'id': request, 'stream': channel, 'frequency_hz': 10489600000})
                owner.wait(lambda s: isinstance(s, dict) and s.get('id') == request and s.get('code') == 'invalid')
            for channel in (1, 2):
                wait(lambda: len(pcm[channel]) > 30)
                assert len(pcm[channel]) > 30
                assert all(b[7] == a[7]+240 and b[5] == a[5] for a, b in zip(pcm[channel], pcm[channel][1:])), 'Monitor setting interrupted RX1/RX2'
            for client in clients:
                assert not any(isinstance(p, bytes) and p[5] == 5 and HEADER.unpack_from(p)[4] in (5, 6) for p in client.messages)
            # Restart only FT8 transport while PSK and the station continue.
            taps[5].close()
            decoder = subprocess.Popen([str(dummy), '--port', str(audio3), '--interval-ms', '60'], stdout=logs[2], stderr=logs[2])
            for client in clients:
                wait(lambda: 'abcdef\n' in ''.join(s['text'] for s in list(client.messages) if isinstance(s, dict) and s.get('type') == 'decoder_text' and s['stream'] == 3), timeout=4)
            output(6, 'PSK survives FT8 restart')
            radio.terminate(); radio.wait(timeout=5)
            radio = start_radio()
            state = owner.state(lambda s: s.get('ready') and s.get('session') not in (0, initial['session']) and
                                s.get('settings', {}).get('rx3_frequency_hz') == '10489600321' and
                                s.get('settings', {}).get('rx4_frequency_hz') == '10489748821', timeout=7)
            assert not state['tx']['ptt']
            audio(6, 10489748821)
            print('Monitoring: parallel byte-identical RX3/RX4 PCM and text, headless routing, independent retune, validation, '
                  'RX1/RX2 continuity, decoder restart, durable settings/backend restart and no browser PCM passed')
        finally:
            for tap in taps.values(): tap.close()
            for client in clients: client.close()
            for process in (decoder, bridge, radio):
                if process and process.poll() is None:
                    process.terminate(); process.wait(timeout=8)
            proxy.close()
            for log in logs: log.close()
            if sys.exc_info()[0]:
                print((root/'backend.log').read_text()); print((root/'bridge.log').read_text())
        assert bridge.returncode == radio.returncode == decoder.returncode == 0


if __name__ == '__main__':
    main()
