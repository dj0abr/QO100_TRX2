"""Switch RX2/TX2 between FT8 and FT4 and verify both waveforms offline."""
from contextlib import ExitStack
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import wave

from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def main():
    bridge, backend, receiver, generator, core, defaults, web = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-ft8-rx2-') as tmp, ExitStack() as stack:
        root = Path(tmp)
        backend_port, http, audio2, text2, audio3, text3 = (port() for _ in range(6))
        config = root / 'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        raw = root / 'input.raw'
        message = 'CQ K1ABC FN42'
        subprocess.run([generator, raw, message, '1500', '0.2', '-12'], check=True, timeout=10)
        pcm = struct.unpack('<180000h', raw.read_bytes())

        def launch(name, *args):
            log = stack.enter_context((root / name).open('w'))
            process = subprocess.Popen(args, stdout=log, stderr=log)

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
               '--backend-port', str(proxy.port), '--web-root', web,
               '--rx2-decoder-audio-port', str(audio2), '--rx2-decoder-text-port', str(text2),
               '--rx3-decoder-audio-port', str(audio3), '--rx3-decoder-text-port', str(text3))
        for rx, audio in [(2, audio2), (3, audio3)]:
            launch(f'rx{rx}.log', receiver, '--rx', str(rx), '--port', str(audio), '--time-offset-ms', '0')
            wait(lambda: 'waiting for a complete UTC slot' in (root / f'rx{rx}.log').read_text())

        def connect(role):
            client = Client(http)
            stack.callback(client.close)
            client.role(role)
            return client

        owner, viewer = connect('operator'), connect('viewer')
        state = owner.state(lambda s: s.get('ready') and 'epoch' in s['tx2']
                            and all(str(rx) in s['rx'] for rx in (2, 3)))
        viewer.state()
        assert state['ft8_enabled']
        for client in (owner, viewer):
            client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': False})
        def activity():
            return next((HEADER.unpack_from(p)[11] for p in reversed(list(proxy.log)) if p[5] == 20), None)

        wait(lambda: activity() == 0)
        owner.send({'type': 'ft8_subscribe', 'enabled': True})
        wait(lambda: activity() == 1)

        slot = (int(time.time()) // 15 + 1) * 15
        def feed_ft8(start, stop):
            for offset in range(start, stop, 240):
                timestamp = slot * 10**9 + (offset + 240) * 10**9 // 12000
                payload = struct.pack('!240h', *pcm[offset:offset + 240])
                for rx, stream in [(2, 2), (3, 5)]:
                    if rx == 2 and 84000 <= offset < 84720:
                        continue
                    meta = state['rx'][str(rx)]
                    packet = HEADER.pack(b'QTR2', 1, 5, 64, stream, meta['epoch'], timestamp, offset,
                                         meta['frequency_hz'], 12000, 0, 0, 240, 2, 240, state['session'])
                    proxy.socket.sendto(packet + payload, proxy.peer)
                if offset % 3840 == 0:
                    time.sleep(.006)

        # Withhold the rest of the slot until the early RX2 result reaches the browser.
        early_end = ((41 * 3456 + 239) // 240) * 240
        feed_ft8(0, early_end)
        result = owner.wait(lambda p: isinstance(p, dict) and p.get('type') == 'ft8_decode', timeout=15)
        assert result['message'] == message and result['slot_utc'] == slot and result['mode'] == 'FT8', result
        assert result['dial_hz'] == state['rx']['2']['frequency_hz'] and abs(result['frequency_hz'] - 1500) < 1
        feed_ft8(early_end, 180000)
        wait(lambda: '"samples":180000' in (root / 'rx2.log').read_text(), timeout=15)
        for client in (owner, viewer):
            client.wait(lambda p: isinstance(p, dict) and p.get('type') == 'decoder_text'
                        and p['stream'] == 3 and message in p['text'], timeout=15)
        assert not any(isinstance(p, dict) and p.get('type') == 'ft8_decode' for p in list(viewer.messages))
        assert sum(isinstance(p, dict) and p.get('type') == 'ft8_decode' and p.get('slot_utc') == slot
                   and p.get('message') == message for p in list(owner.messages)) == 1

        frequency = state['tx2']['frequency_hz']

        def request(client, kind, id, **values):
            client.send({'type': kind, 'id': id, 'frequency_hz': frequency, 'level': 25, **values})
            return client.wait(lambda p: isinstance(p, dict) and p.get('id') == id
                               and p['type'] in ('ack', 'error'))

        def tx_packets():
            return [p for p in proxy.log if p[5] == 8 and HEADER.unpack_from(p)[4] == 7]

        def arm(client, id, parity, payload=message):
            return request(client, 'ft8_tx_start', id, parity=parity, message=payload, audio_hz=1500)

        parity = (int(time.time() + 3) // 15 + 1) % 2
        assert arm(viewer, 1, parity)['code'] == 'read_only'
        assert arm(owner, 2, parity, 'TEST DE DJ0ABR')['code'] == 'ft8_free_text_length'
        assert arm(owner, 3, parity, 'CQ DJ0ABR JN58')['status'] == 0
        assert request(owner, 'rtty_tx_start', 4, text='TEST')['code'] == 'tx2_busy'
        owner.send({'type': 'ft8_tx_stop'})
        owner.state(lambda s: s['ft8_tx'].get('result') == 'stopped')
        owner.state(lambda s: s['tx2'].get('epoch', state['tx2']['epoch']) != state['tx2']['epoch'])
        assert not tx_packets(), 'Cancelled slot emitted PCM'

        # A command received just after the UTC boundary can still send the full waveform.
        next_slot = int(time.time()) // 15 + 1
        target = next_slot * 15
        time.sleep(max(0, target + .040 - time.time()))
        free_text = 'TEST DJ0ABR'
        answer = arm(owner, 5, next_slot % 2, free_text)
        assert answer['status'] == 0 and answer['slot_utc'] == target, answer
        dispatch = answer['slot_utc'] + .420
        assert not tx_packets()
        wait(tx_packets, timeout=max(2, dispatch - time.time() + 2))
        assert dispatch - .05 <= time.time() <= dispatch + .16, 'PCM start missed its UTC window'
        owner.state(lambda s: s['ft8_tx'].get('result') == 'complete', timeout=16)

        packets = tx_packets()
        assert len(packets) == 632 + 13
        assert all(HEADER.unpack_from(p)[7:9] == (i * 240, frequency) for i, p in enumerate(packets))
        assert all(p[64:] == bytes(480) for p in packets[-13:])
        samples = [s for p in packets for s in struct.unpack('!240h', p[64:])]
        assert 8100 < max(samples) < 8200, 'Wrong transmit gain'
        raw.write_bytes(bytes(12000) + struct.pack(f'<{len(samples)}h', *samples))
        output = subprocess.check_output([core, '--format', 's16le', raw], text=True, timeout=8)
        assert any(json.loads(line).get('message') == free_text for line in output.splitlines()), output
        assert request(viewer, 'ft_mode', 6, mode='FT4')['code'] == 'read_only'
        assert request(owner, 'ft_mode', 7, mode='FT4')['status'] == 0
        owner.state(lambda s: s.get('ft_mode') == 'FT4')
        capture = Path(__file__).resolve().parents[1] / 'external/ft8/vendor/wsjtx/samples/FT4/000000_000002.wav'
        with wave.open(str(capture)) as recording:
            ft4_pcm = struct.unpack('<72576h', recording.readframes(72576))
        # Only the FT4 decode window is sent, rounded up to a 240-sample UDP block.
        ft4_blocks = (len(ft4_pcm) + 239) // 240
        ft4_pcm += (0,) * (ft4_blocks * 240 - len(ft4_pcm))
        ft4_slot = slot + 22.5
        meta = state['rx']['2']
        for start in range(0, len(ft4_pcm), 240):
            timestamp = int(ft4_slot * 10**9) + (start + 240) * 10**9 // 12000
            packet = HEADER.pack(b'QTR2', 1, 5, 64, 2, meta['epoch'], timestamp, 180000 + start,
                                 meta['frequency_hz'], 12000, 0, 0, 240, 2, 240, state['session'])
            proxy.socket.sendto(packet + struct.pack('!240h', *ft4_pcm[start:start + 240]), proxy.peer)
            if start % 3840 == 0:
                time.sleep(.006)
        ft4_result = owner.wait(lambda p: isinstance(p, dict) and p.get('type') == 'ft8_decode'
                                and p.get('mode') == 'FT4' and p['message'] == 'KB0VHA KA1YQC R 539 MA',
                                timeout=15)
        assert ft4_result['slot_utc'] == ft4_slot and ft4_result['utc'].endswith('.5'), ft4_result

        next_slot = int(time.time() // 7.5) + 1
        target = next_slot * 7.5
        time.sleep(max(0, target + .040 - time.time()))
        answer = request(owner, 'ft8_tx_start', 8, parity=next_slot % 2, message=free_text, audio_hz=1500, mode='FT4')
        assert answer['status'] == 0 and answer['slot_utc'] == target, answer
        dispatch = answer['slot_utc'] + .420
        before = len(packets)
        wait(lambda: len(tx_packets()) > before, timeout=max(2, dispatch - time.time() + 2))
        assert dispatch - .05 <= time.time() <= dispatch + .16, 'FT4 PCM start missed its UTC window'
        assert request(owner, 'ft_mode', 9, mode='FT8')['code'] == 'tx2_busy'
        owner.state(lambda s: s.get('ft_mode') == 'FT4' and s['ft8_tx'].get('result') == 'complete', timeout=9)
        ft4_packets = tx_packets()[before:]
        assert len(ft4_packets) == 252 + 13
        assert all(HEADER.unpack_from(p)[7:9] == (i * 240, frequency) for i, p in enumerate(ft4_packets))
        samples = [sample for p in ft4_packets for sample in struct.unpack('!240h', p[64:])]
        raw.write_bytes((bytes(12000) + struct.pack(f'<{len(samples)}h', *samples)).ljust(72576 * 2, bytes(1)))
        output = subprocess.check_output([core, '--mode', 'FT4', '--format', 's16le', raw], text=True, timeout=8)
        assert any(json.loads(line).get('message') == free_text for line in output.splitlines()), output
        assert request(owner, 'ft_mode', 10, mode='FT8')['status'] == 0
        owner.state(lambda s: s.get('ft_mode') == 'FT8' and s['ft8_tx'].get('phase') == 'idle')
        # Past the acceptance deadline, keep parity and use the next eligible slot.
        next_slot = int(time.time()) // 15 + 1
        target = next_slot * 15
        time.sleep(max(0, target + .300 - time.time()))
        answer = arm(owner, 11, next_slot % 2, free_text)
        assert answer['status'] == 0 and answer['slot_utc'] == target + 30, answer
        owner.send({'type': 'ft8_tx_stop'})
        owner.state(lambda s: s['ft8_tx'].get('result') == 'stopped')
        assert len(tx_packets()) == before + len(ft4_packets)
        print('RX2 FT8 and FT4 decode, RX3 FT8 monitor, late TX2 waveform round-trips and slot fallback passed')


if __name__ == '__main__':
    main()
