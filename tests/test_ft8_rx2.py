"""Real PCM through RX2/libft8/TX2 and the independent RX3 monitor."""
from contextlib import ExitStack
from datetime import datetime, timezone
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


RATE = 12000
BLOCK = 240


class Audio:
    def __init__(self, proxy, state):
        self.proxy = proxy
        self.state = state
        self.frames = {}
        self.running = True
        self.thread = threading.Thread(target=self.run)
        self.thread.start()

    def close(self):
        self.running = False
        self.thread.join(timeout=2)

    def schedule(self, stream, slot, pcm):
        base = round(slot * RATE)
        for offset in range(0, len(pcm), BLOCK):
            self.frames[stream, base + offset] = struct.pack('!240h', *pcm[offset:offset + BLOCK])

    def run(self):
        start = math.ceil(time.time() * 50) * BLOCK
        tick = start
        silence = bytes(2 * BLOCK)
        while self.running:
            time.sleep(max(0, (tick + BLOCK) / RATE - time.time()))
            for stream, rx in ((2, '2'), (5, '3')):
                source = self.state['rx'][rx]
                header = HEADER.pack(b'QTR2', 1, 5, 64, stream, source['epoch'], (tick + BLOCK) * 10**9 // RATE,
                                     tick - start, source['frequency_hz'], RATE, 0, 0, BLOCK, 2, BLOCK, self.state['session'])
                try:
                    self.proxy.socket.sendto(header + self.frames.pop((stream, tick), silence), self.proxy.peer)
                except OSError:
                    return
            tick += BLOCK


def main():
    bridge, backend, monitor, codec, defaults, web = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-ft-') as tmp, ExitStack() as stack:
        root = Path(tmp)
        backend_port, http, audio3, text3 = (port() for _ in range(4))
        config = root / 'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        station = root / 'station.json'
        logbook = root / 'ft4-ft8.adi'
        now = datetime.now(timezone.utc)
        logbook.write_text(f"Existing log <EOH>\n<CALL:5>K2ABC <QSO_DATE:8>{now:%Y%m%d} <TIME_ON:6>{now:%H%M%S} <EOR>\n")
        raw = root / 'test.pcm'

        def waveform(mode, message):
            subprocess.run([codec, 'encode', mode, raw, message], check=True, timeout=5)
            return tuple(value for value, in struct.iter_unpack('<h', raw.read_bytes()))

        def launch(name, *args):
            log = stack.enter_context((root / name).open('w'))
            env = {**os.environ, 'QO100_QRZ_API_KEY': ''}
            process = subprocess.Popen(args, stdout=log, stderr=log, env=env)

            def stop():
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=8)

            stack.callback(stop)
            return process

        proxy = Proxy(backend_port)
        stack.callback(proxy.close)
        proxy.drop_once = False
        proxy.drop = lambda p: p[5] == 5 and HEADER.unpack_from(p)[4] in (2, 5)
        launch('radio.log', backend, '--config', config, '--source', 'synthetic')
        launch('bridge.log', bridge, '--listen', '127.0.0.1', '--port', str(http),
               '--backend-port', str(proxy.port), '--web-root', web, '--ft-enabled', '1',
               '--ft-station-path', station, '--ft-log-path', logbook,
               '--rx3-decoder-audio-port', str(audio3), '--rx3-decoder-text-port', str(text3))
        launch('rx3.log', monitor, '--port', str(audio3))
        wait(lambda: 'FT8 receiver: RX3' in (root / 'rx3.log').read_text())

        def connect(role):
            client = Client(http)
            stack.callback(client.close)
            client.role(role)
            return client

        owner, viewer = connect('operator'), connect('viewer')
        initial = owner.state(lambda s: s.get('ready') and s['ft']['available'])
        assert not initial['ft']['configured'] and not initial['ft']['tx_ready']
        viewer.state()
        for client in (owner, viewer):
            client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': False})

        command_id = 0

        def request(client, what, **fields):
            nonlocal command_id
            command_id += 1
            client.send({'type': what, 'id': command_id, **fields})
            return client.wait(lambda p: isinstance(p, dict) and p.get('id') == command_id)

        def command(what, **fields):
            result = request(owner, what, **fields)
            assert result.get('status') == 0, result
            return result

        def state_ft(predicate, timeout=6):
            latest = None
            def matches():
                nonlocal latest
                latest = next((s for s in reversed(list(owner.messages))
                               if isinstance(s, dict) and s.get('type') == 'state'), None)
                return latest and predicate(latest['ft'])
            wait(matches, timeout=timeout)
            return latest

        def event(kind, predicate, timeout=10):
            return owner.wait(lambda p: isinstance(p, dict) and p.get('type') == kind and predicate(p), timeout)

        assert request(viewer, 'ft_arm', armed=True)['code'] == 'read_only'
        owner.send({'type': 'ft8_subscribe', 'enabled': True})
        assert request(owner, 'ft_arm', armed=True)['type'] == 'error'
        assert not station.exists()
        wait(lambda: any(p[5] == 20 and HEADER.unpack_from(p)[11] & 1 for p in proxy.log))
        time.sleep(.25)
        owner.send({'type': 'state'})
        radio = state_ft(lambda ft: ft['utc_ms'] and not ft['configured'])
        rx = Audio(proxy, radio)
        stack.callback(rx.close)

        try:
            ft8_pcm = waveform('FT8', 'CQ K1ABC FN42')
            slot = math.ceil((time.time() + .6) / 15) * 15
            rx.schedule(2, slot, ft8_pcm)
            rx.schedule(5, slot, ft8_pcm)
            row = event('ft_decode', lambda p: p['text'] == 'CQ K1ABC FN42', timeout=32)
            assert row['slot_utc_ms'] == slot * 1000 and row['mode'] == 'FT8' and not row['worked_recently'], row
            viewer.wait(lambda p: isinstance(p, dict) and p.get('type') == 'decoder_text'
                        and p['stream'] == 3 and 'CQ K1ABC FN42' in p['text'], timeout=8)
            assert not any(isinstance(p, dict) and p.get('type') == 'ft_decode' for p in list(viewer.messages))
            print('RX2 without station settings and independent RX3 decoded the same FT8 slot', flush=True)

            command('ft_config', callsign='DJ0ABR', locator='JN68', tx_hz=1500, tx_level=25)
            assert '"callsign": "DJ0ABR"' in station.read_text()
            state_ft(lambda ft: ft['configured'] and ft['tx_ready'])
            command('ft_mode', mode='FT4')
            state_ft(lambda ft: ft['mode'] == 'FT4' and ft['tx_ready'])
            command('ft_arm', armed=True)
            state_ft(lambda ft: ft['armed'])
            assert request(owner, 'ft_start_qso', decode_id=row['decode_id'])['code'] == 'invalid'
            command('ft_auto', auto='answer-cq')
            state_ft(lambda ft: ft['auto'] == 'answer-cq')
            first = math.ceil((time.time() + .7) / 7.5) * 7.5
            incoming = (
                (first, 'CQ K2ABC FN42'),
                (first + 7.5, 'CQ K1ABC FN42'),
                (first + 15, 'K1ABC DJ0ABR JN68'),
                (first + 22.5, 'DJ0ABR K1ABC -10'),
                (first + 37.5, 'DJ0ABR K1ABC RR73'),
            )
            for receive_slot, message in incoming:
                rx.schedule(2, receive_slot, waveform('FT4', message))
            recent = event('ft_decode', lambda p: p['source'] == 'K2ABC', timeout=16)
            assert recent['worked_recently'], recent
            tx = event('ft_tx', lambda p: p['destination'] == 'K1ABC', timeout=18)
            assert tx['mode'] == 'FT4' and tx['slot_utc_ms'] == (first + 15) * 1000 and tx['text'] == 'K1ABC DJ0ABR JN68', tx
            assert 300 <= tx['audio_start_ms'] - tx['slot_utc_ms'] <= 400, tx
            echo = event('ft_decode', lambda p: p['slot_utc_ms'] == tx['slot_utc_ms'] and p['text'] == tx['text'])
            assert echo['source'] == 'DJ0ABR' and isinstance(echo['snr_db'], int), echo
            event('ft_notice', lambda p: p['kind'] == 'adif' and p['outcome'] == 'completed', timeout=38)
            record = logbook.read_text()
            for field in ('<CALL:5>K1ABC', '<MODE:4>MFSK<SUBMODE:3>FT4', '<BAND:4>13cm',
                          '<BAND_RX:3>3cm', '<PROP_MODE:3>SAT<SAT_NAME:6>QO-100', '<RST_RCVD:3>-10'):
                assert field in record, record
            for field, hz in [('FREQ', radio['tx2']['frequency_hz']), ('FREQ_RX', radio['rx']['2']['frequency_hz'])]:
                mhz = f'{hz / 1e6:.6f}'
                assert f'<{field}:{len(mhz)}>{mhz}' in record, record
            assert request(viewer, 'ft_adif')['code'] == 'read_only'
            ticket = command('ft_adif')['url']
            url = f'http://127.0.0.1:{http}' + ticket
            with urllib.request.urlopen(url, timeout=3) as response:
                assert response.read().decode() == record
            try:
                urllib.request.urlopen(url, timeout=3)
                raise AssertionError('ADIF ticket is reusable')
            except urllib.error.HTTPError as error:
                assert error.code == 403
            print('FT4 automatic QSO, imported CQ filter and uplink/downlink/satellite ADIF passed', flush=True)

            def tx_packets():
                return [p for p in proxy.log if p[5] == 8 and HEADER.unpack_from(p)[4] == 7]

            def play_captured(mode, packets, message):
                assert packets
                metadata = [HEADER.unpack_from(p) for p in packets]
                assert all(header[7] == i * BLOCK and header[10] == 2 for i, header in enumerate(metadata))
                samples = b''.join(struct.pack('<240h', *struct.unpack('!240h', p[64:])) for p in packets)
                delay = 3600 if mode == 'FT4' else 6000
                raw.write_bytes(bytes(2 * delay) + samples)
                assert message in subprocess.check_output([codec, 'decode', mode, raw], text=True, timeout=8)

            packets = tx_packets()
            first_sent = [packet for i, packet in enumerate(packets)
                          if i == 0 or HEADER.unpack_from(packet)[5] == HEADER.unpack_from(packets[0])[5]]
            play_captured('FT4', first_sent, 'K1ABC DJ0ABR JN68')

            command('ft_mode', mode='FT8')
            state_ft(lambda ft: ft['mode'] == 'FT8' and not ft['armed'] and ft['tx_ready'])
            command('ft_arm', armed=True)
            state_ft(lambda ft: ft['armed'])
            before = len(tx_packets())
            if time.time() % 15 < 1:
                time.sleep(1)
            command('ft_cq', parity=(int(time.time()) // 15) % 2)
            state_ft(lambda ft: ft['busy'] and ft.get('next_tx_planned'))
            command('ft_abort')
            state_ft(lambda ft: ft['armed'] and not ft['busy'] and ft['auto'] == 'off')
            assert len(tx_packets()) == before, 'Aborted preparation emitted PCM'

            return_pcm = waveform('FT8', 'TNX 73')
            command('ft_free_text', text='TNX 73')
            tx = event('ft_tx', lambda p: p['text'] == 'TNX 73', timeout=18)
            assert tx['mode'] == 'FT8' and 500 <= tx['audio_start_ms'] - tx['slot_utc_ms'] <= 680, tx
            # Model the downlink by delaying the RX waveform by thirteen PCM blocks.
            rx.schedule(2, tx['slot_utc_ms'] / 1000 + .26, return_pcm[:-13 * BLOCK])
            echo = event('ft_decode', lambda p: p['slot_utc_ms'] == tx['slot_utc_ms'] and p['text'] == tx['text'], timeout=17)
            assert isinstance(echo['snr_db'], int) and abs(echo['dt_s'] - .26) < .2, echo
            event('ft_notice', lambda p: p['kind'] == 'free-text' and p['outcome'] == 'completed', timeout=16)
            play_captured('FT8', tx_packets()[before:], 'TNX 73')
            state_ft(lambda ft: not ft['busy'] and ft['armed'])

            command('ft_mode', mode='FT4')
            state_ft(lambda ft: ft['mode'] == 'FT4' and ft['tx_ready'])
            command('ft_arm', armed=True)
            state_ft(lambda ft: ft['armed'])
            command('ft_cq')
            event('ft_tx', lambda p: p['text'] == 'CQ DJ0ABR JN68', timeout=11)
            owner.send({'type': 'ft8_subscribe', 'enabled': False})
            state_ft(lambda ft: not ft['armed'])
            time.sleep(.25)
            count = len(tx_packets())
            time.sleep(.45)
            assert len(tx_packets()) == count, 'TX2 continued emitting after leaving the FT mode'
            assert not radio['tx']['ptt'], 'This test must use TX2 rather than TX1'
            print('RX2 decoded the FT4 and FT8 transmit slots with SNR', flush=True)
            print('FT4/FT8 TX2 waveform, UTC start, queued abort and active cancellation passed', flush=True)
        except BaseException:
            for item in list(owner.messages):
                if isinstance(item, dict) and item.get('type') in ('ft_decode', 'ft_tx', 'ft_notice'):
                    print(item, flush=True)
            for path in (root / 'radio.log', root / 'bridge.log', root / 'rx3.log'):
                print(path.name + '\n' + path.read_text()[-6000:], flush=True)
            raise


if __name__ == '__main__':
    main()
