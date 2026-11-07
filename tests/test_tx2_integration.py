"""Dual TX through webbridge and the real backend into an RF-free Pluto double."""
from pathlib import Path
import json
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import numpy as np
from test_webbridge import HEADER, port, private_config, wait
from test_tx_integration import DRIVER
from ws_client import Client


def main():
    backend, adapter, defaults, assets = map(Path, sys.argv[1:])
    # This test uses the driver's fixed loopback ports; run in an isolated network namespace.
    with tempfile.TemporaryDirectory(prefix='qo100-tx2-') as temp:
        root = Path(temp); (root/'bin').mkdir()
        shutil.copy2(backend, root/'bin/qo100_backend')
        driver = root/'bin/pluto'
        driver.write_text(DRIVER.replace('(151000,149000,150000)', '(151000,149000,150000,-148300,-151700,-150000,-145300)'))
        driver.chmod(0o755)
        backend_port, http, audio_port, bridge_port = [port() for _ in range(4)]
        config = root/'backend.conf'; config.write_text(private_config(defaults, backend_port))
        logs = [(root/name).open('w') for name in ('backend.log', 'bridge.log')]
        radio = subprocess.Popen([str(root/'bin/qo100_backend'), '--config', str(config)], stdout=logs[0], stderr=logs[0])
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http), '--backend-port', str(backend_port),
            '--web-root', str(assets), '--digital-audio-port', str(audio_port), '--digital-bridge-port', str(bridge_port)], stdout=logs[1], stderr=logs[1])
        digital = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); digital.bind(('127.0.0.1', audio_port)); digital.connect(('127.0.0.1', bridge_port)); digital.setblocking(False)
        owner = viewer = None; request = 0; nonce = 12345; tx2 = None; index2 = 0; index1 = 0
        def pump():
            nonlocal tx2, index2
            while True:
                try: packet = digital.recv(65536)
                except (BlockingIOError, ConnectionRefusedError): break
                h = HEADER.unpack_from(packet)
                if h[2] == 7 and h[4] == 7 and h[6] == nonce:
                    identity = (h[15], h[7], h[8])
                    if tx2 is None or identity != (tx2[15], tx2[7], tx2[8]): index2 = 0
                    tx2 = h
        def hello():
            h = [b'QTR2', 1, 19, 64, 7, 0, nonce, 0, 0, 0, 0, 0, 0, 0, 0, 0]
            digital.send(HEADER.pack(*h)); pump()
            return tx2 is not None and tx2[11] == 1
        def control(kind, values, status=0):
            nonlocal request
            request += 1; owner.send({'type': kind, 'id': request, **values})
            reply = owner.wait(lambda s: isinstance(s, dict) and s.get('id') == request and s.get('type') in ('ack', 'error'))
            assert reply.get('status') == status, reply
        def audio(stream, epoch, session, frequency, index, tone, amplitude):
            values = np.rint(amplitude*32767*np.cos(2*np.pi*tone*(index+np.arange(240))/12000)).astype('>i2')
            return HEADER.pack(b'QTR2', 1, 8, 64, stream, epoch, 0, index, frequency, 12000, 0, 0, 240, 2, 240, session)+values.tobytes()
        def phase(seconds, voice=False, data=True, stale=None):
            nonlocal index1, index2
            begin = time.monotonic(); tick = begin
            while time.monotonic() < begin+seconds:
                pump()
                if time.monotonic() >= tick:
                    if voice:
                        owner.send({'type': 'ptt_keepalive'})
                        owner.send(audio(3, voice_state['tx']['epoch'], voice_state['session'], 2400400000, index1, 1000, .2)); index1 += 240
                    if data and tx2 and tx2[11]:
                        identity = tx2 if stale is None else stale
                        digital.send(audio(7, identity[7], identity[15], identity[8], index2, 1700, .3)); index2 += 240
                    tick += .02
                time.sleep(.001)
            return begin, time.monotonic()
        try:
            def available():
                try: return urllib.request.urlopen(f'http://127.0.0.1:{http}/health', timeout=.2).status == 200
                except OSError: return False
            wait(available)
            owner = Client(http); owner.role('operator'); initial = owner.state()
            viewer = Client(http); viewer.role('viewer', True); viewer.state()
            wait(hello)
            assert tx2[8] == 2400100000 and tx2[11] == 1
            control('compressor', {'percent': 0})
            control('tx', {'frequency_hz': 2400400000, 'ptt': True})
            voice_state = owner.state(lambda s: s.get('tx', {}).get('ptt'))
            both = phase(1.1, voice=True)
            control('tx', {'frequency_hz': 2400400000, 'ptt': False})
            digital_only = phase(.9)
            control('tx', {'channel': 2, 'frequency_hz': 2401000000}, status=1)
            owner.close(); owner = None
            headless = phase(.9)
            owner = Client(http); owner.role('operator'); owner.state()
            old = tx2
            control('tx', {'channel': 2, 'frequency_hz': 2400103000})
            wait(lambda: (pump() or True) and tx2[8] == 2400103000 and tx2[7] != old[7])
            moved = phase(.9)
            assert owner.state(lambda s: s.get('tx2', {}).get('frequency_hz') == 2400103000)['tx']['frequency_hz'] == initial['tx']['frequency_hz']
            stale = phase(.8, stale=old)
            # A new adapter instance has a new nonce and must get a new epoch before index zero.
            old_epoch = tx2[7]; nonce += 1; tx2 = None
            wait(hello); assert tx2[7] > old_epoch
            recovered = phase(.9)
            missing = phase(.8, data=False)
            assert 'initial_tx2_hz=2400103000' in config.with_suffix('.settings').read_text(), 'TX2 frequency was not persisted'
        finally:
            if owner: owner.close()
            if viewer: viewer.close()
            digital.close()
            for p in (bridge, radio):
                if p.poll() is None: p.terminate(); p.wait(timeout=8)
            for log in logs: log.close()
            if sys.exc_info()[0]: print((root/'backend.log').read_text()); print((root/'bridge.log').read_text())
        assert radio.returncode == bridge.returncode == 0
        records = np.array(json.loads(driver.with_suffix('.json').read_text()))
        def window(times):
            result = records[(records[:, 0] > times[0]+.5) & (records[:, 0] < times[1])]
            assert len(result) > 5, times
            return result
        joint = window(both)
        assert .044 < np.median(joint[:, 2]) < .056, joint[:, 2]
        assert .065 < np.median(joint[:, 5]) < .085, joint[:, 5]
        assert np.max(records[:, 1]) < .5, 'Composite output clipped'
        for times in (digital_only, headless):
            measured = window(times)
            assert np.max(measured[:, 2]) < .0003 and .065 < np.median(measured[:, 5]) < .085
        for times in (moved, recovered):
            measured = window(times)
            assert np.max(measured[:, 5]) < .0003 and .065 < np.median(measured[:, 8]) < .085
        for times in (stale, missing):
            assert np.max(window(times)[:, 1]) == 0, 'Old/missing TX2 audio was transmitted'
        assert np.max(joint[:, [3, 4, 6, 7]]) < .0003, 'Carrier or lower sideband leakage'
        print('Dual TX IQ: two independent USB carriers, linear digital level, headroom, no PTT/browser dependency, tuning, '
              'stale rejection, fresh adapter epoch and exact zero on audio loss passed; no IIO/hardware access')


if __name__ == '__main__':
    main()
