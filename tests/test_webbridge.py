"""Exercise real HTTP/WS + a private synthetic backend; never touches the Pluto."""
from pathlib import Path
import json
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import re
from ws_client import Client

HEADER = struct.Struct('!4sBBHIIQQQIIIHHII')
_ports = {40900, 40808, 40809, 40810, 8080}


def port():
    while True:
        with socket.socket() as tcp, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            tcp.bind(('127.0.0.1', 0))
            candidate = tcp.getsockname()[1]
            if candidate in _ports:
                continue
            try:
                udp.bind(('127.0.0.1', candidate))
            except OSError:
                continue
            _ports.add(candidate)
            return candidate


def private_config(defaults, backend_port):
    value = re.sub(r'(?m)^\s*gui_port\s*=.*$', f'gui_port = {backend_port}', defaults.read_text())
    value = re.sub(r'(?m)^\s*gui_bind\s*=.*$', 'gui_bind = 127.0.0.1', value)
    assert f'gui_port = {backend_port}' in value
    return value


def wait(predicate, timeout=5):
    end = time.monotonic()+timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.02)
    assert predicate(), 'Condition timed out'


class Proxy:
    """Record GUI-side UDP and drop selected replies to verify retransmission."""
    def __init__(self, backend):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1', port()))
        self.port = self.socket.getsockname()[1]
        self.backend = ('127.0.0.1', backend)
        self.peer = None
        self.log = []
        self.drop = None
        self.drop_once = True
        self.running = True
        self.socket.settimeout(.1)
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while self.running:
            try:
                packet, address = self.socket.recvfrom(65536)
                if address == self.backend:
                    if self.drop and self.drop(packet):
                        if self.drop_once:
                            self.drop = None
                        continue
                    if self.peer:
                        self.socket.sendto(packet, self.peer)
                else:
                    self.peer = address
                    self.log.append(packet)
                    self.socket.sendto(packet, self.backend)
            except socket.timeout:
                pass
            except OSError:
                break

    def close(self):
        self.running = False
        self.socket.close()


def main():
    adapter, backend, defaults, assets = map(Path, sys.argv[1:])
    clients = []
    with tempfile.TemporaryDirectory(prefix='qo100-webbridge-') as temp:
        root = Path(temp)
        backend_port, http_port = port(), port()
        config = root/'backend.conf'
        config.write_text(private_config(defaults, backend_port))
        backend_log = (root/'backend.log').open('w')
        adapter_log = (root/'adapter.log').open('w')
        def start_backend():
            return subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'],
                                    stdout=backend_log, stderr=backend_log)
        process = start_backend()
        proxy = Proxy(backend_port)
        bridge = subprocess.Popen([str(adapter), '--listen', '127.0.0.1', '--port', str(http_port),
                                   '--backend-port', str(proxy.port), '--web-root', str(assets), '--idle-seconds', '4'],
                                  stdout=adapter_log, stderr=adapter_log)
        def browser(**kwargs):
            c = Client(http_port, **kwargs)
            clients.append(c)
            return c
        serial = 0
        def command(c, kind, **values):
            nonlocal serial
            assert process.poll() is None, 'Private synthetic backend stopped'
            serial += 1
            c.send({'type': 'activity'})
            c.send({'type': kind, 'id': serial, **values})
            answer = c.wait(lambda s: isinstance(s, dict) and s.get('id') == serial and s.get('type') in ('ack', 'error'))
            assert answer['type'] == 'ack' and answer['status'] == 0, answer
            return answer
        try:
            url = f'http://127.0.0.1:{http_port}'
            def available():
                try:
                    return urllib.request.urlopen(url+'/health', timeout=.2).status == 200
                except OSError:
                    return False
            wait(available)
            assert b'QO-100' in urllib.request.urlopen(url).read()
            for asset in ['app.js', 'audio-worklet.js', 'style.css']:
                assert urllib.request.urlopen(url+'/'+asset).status == 200
            assert int(urllib.request.urlopen(urllib.request.Request(url+'/app.js', method='HEAD')).headers['Content-Length']) > 100
            for path in ['/../CMakeLists.txt', '/%2e%2e/CMakeLists.txt']:
                try:
                    urllib.request.urlopen(url+path)
                    raise AssertionError('Path traversal accepted')
                except urllib.error.HTTPError as e:
                    assert e.code in (400, 404)
            owner = browser()
            owner.role('operator')
            initial = owner.state()
            assert initial['settings']['lnb_reference_hz'] == '25000000'
            viewer = browser()
            viewer.role('viewer', True)
            viewer.state()
            for stream in [0, 1, 2]:
                viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 2 and struct.unpack_from('!I', p, 8)[0] == stream)
            for stream in [1]:
                viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 5 and len(p) == 544 and struct.unpack_from('!I', p, 8)[0] == stream)
            # No viewer command, query, or audio may be forwarded to the backend.
            marker = 10489771234
            for i, body in enumerate([
                {'type': 'tune', 'stream': 1, 'frequency_hz': marker},
                {'type': 'tx', 'frequency_hz': 2400400000, 'ptt': True},
                {'type': 'settings_get'}, {'type': 'settings_set', 'revision': '1', 'values': {'gui_audio_gain': 17.123}},
                {'type': 'loop', 'mode': 1}, {'type': 'filter', 'query': True},
            ]):
                viewer.send({**body, 'id': 1000+i})
                viewer.wait(lambda s: isinstance(s, dict) and s.get('id') == 1000+i and s.get('code') == 'read_only')
            viewer.send(bytes(544))
            viewer.wait(lambda s: isinstance(s, dict) and s.get('code') == 'read_only' and s.get('id') is None)
            assert not any(p[5] == 3 and HEADER.unpack_from(p)[8] == marker for p in proxy.log)
            assert not any(b'17.123' in p for p in proxy.log)
            assert not any(p[5] == 8 for p in proxy.log)
            # Drop exactly one tune ACK; retry must retain the same backend request ID.
            frequency = 10489748500
            proxy.drop = lambda p: p[5] == 4 and HEADER.unpack_from(p)[8] == frequency
            command(owner, 'tune', stream=1, frequency_hz=frequency)
            viewer.state(lambda s: s.get('rx', {}).get('1', {}).get('frequency_hz') == frequency)
            attempts = [p for p in proxy.log if p[5] == 3 and HEADER.unpack_from(p)[8] == frequency]
            assert len(attempts) >= 2 and len({p[12:16] for p in attempts}) == 1
            command(owner, 'tune', stream=2, frequency_hz=10489610000)
            command(owner, 'filter', bandwidth_hz=1800, center_hz=1100)  # Legacy command stays supported.
            command(owner, 'filter', low_hz=301, high_hz=2700)
            edges = viewer.state(lambda s: s.get('filter', {}).get('low_hz')==301)
            assert edges['filter']['low_hz']==301 and edges['filter']['high_hz']==2700,edges
            assert edges['filter']['center_hz']==1500.5,edges
            command(owner, 'filter', query=True)
            queried = viewer.state(lambda s: s.get('filter', {}).get('low_hz')==301)
            assert queried['filter']['low_hz']==301 and queried['filter']['high_hz']==2700,queried
            command(owner, 'nr', percent=60)
            command(owner, 'compressor', percent=100.7)
            saved = command(owner, 'settings_get')
            command(owner, 'settings_set', revision=saved['revision'], values={'gui_audio_gain': 5.5})
            viewer.state(lambda s: s.get('settings', {}).get('gui_audio_gain') == '5.5')
            assert 'gui_audio_gain=5.5' in config.with_suffix('.settings').read_text()
            # Hardware-related settings revoke PTT even if the button remains held.
            command(owner, 'loop', mode=1)
            command(owner, 'tx', frequency_hz=2400400000, ptt=True)
            revision = command(owner, 'settings_get')['revision']
            command(owner, 'settings_set', revision=revision, values={'tx_attenuation_db': 9})
            viewer.state(lambda s: s.get('ready') and s.get('settings', {}).get('tx_attenuation_db') == '9'
                         and s.get('tx', {}).get('ptt') is False and s.get('loop', {}).get('mode') == 0)
            # A stalled spectator must not hold up audio/control for the other clients.
            command(owner, 'nr', percent=61)
            viewer.messages.clear()
            slow = browser(read=False)
            slow.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            viewer.wait(lambda s: isinstance(s, dict) and s.get('type') == 'role' and s.get('clients') == 3)
            viewer.wait(lambda s: isinstance(s, dict) and s.get('type') == 'role' and s.get('clients') == 2, 4)
            command(owner, 'nr', percent=60)
            # Audio-loop TX is observable byte-for-byte by all clients, with no RF.
            # State is now published immediately, including the earlier PTT test.
            owner.messages.clear()
            command(owner, 'loop', mode=1)
            command(owner, 'tx', frequency_hz=2400400000, ptt=True)
            tx = owner.state(lambda s: s.get('tx', {}).get('ptt'))
            pcm = struct.pack('!240h', *[i*251-30000 for i in range(240)])
            raw = HEADER.pack(b'QTR2', 1, 8, 64, 3, tx['tx']['epoch'], 0, 0, 2400400000, 12000, 0, 0, 240, 2, 240, tx['session'])+pcm
            owner.send(raw)
            echo = viewer.wait(lambda p: isinstance(p, bytes) and p[5] == 5 and HEADER.unpack_from(p)[4] == 4)
            assert echo[64:] == pcm
            # Heartbeat alone cannot keep PTT on. It requires a fresh explicit PTT action again.
            viewer.messages.clear()
            viewer.state(lambda s: s.get('tx', {}).get('ptt') is False, 2)
            after_off = len(proxy.log)
            owner.send({'type': 'ptt_keepalive'})
            time.sleep(.2)
            assert not [p for p in proxy.log[after_off:] if p[5] == 6 and HEADER.unpack_from(p)[13] == 1]
            command(owner, 'tx', frequency_hz=2400400000, ptt=True)
            viewer.messages.clear()
            proxy.drop_once = False
            proxy.drop = lambda p: p[5] == 7 and HEADER.unpack_from(p)[11] == 0
            owner.close()
            viewer.role('viewer', False)
            fresh = browser()
            fresh.role('operator')
            fresh.state(lambda s: not s.get('ready'))
            fresh.send({'type': 'tx', 'id': 999, 'frequency_hz': 2400400000, 'ptt': True})
            fresh.wait(lambda s: isinstance(s, dict) and s.get('id') == 999 and s.get('code') == 'not_ready')
            proxy.drop = None
            proxy.drop_once = True
            fresh.state()
            viewer.state(lambda s: s.get('ready') and s.get('tx', {}).get('ptt') is False and s.get('loop', {}).get('mode') == 0)
            # IDs belong to this browser, while backend IDs continue across role changes.
            fresh.send({'type': 'tune', 'id': 1, 'stream': 1, 'frequency_hz': 10489760000})
            fresh.wait(lambda s: isinstance(s, dict) and s.get('type') == 'ack' and s.get('id') == 1)
            # Backend restart: retain roles, rediscover the new SID and never restore PTT.
            old_session = fresh.state()['session']
            command(fresh, 'loop', mode=1)
            command(fresh, 'tx', frequency_hz=2400400000, ptt=True)
            process.terminate()
            process.wait(timeout=5)
            process = start_backend()
            # Keep operator interaction alive while the backend bootstraps.
            for _ in range(25):
                fresh.send({'type': 'activity'})
                time.sleep(.1)
            fresh.state(lambda s: s.get('ready') and s.get('session') != old_session and s.get('tx', {}).get('ptt') is False)
            # Operator inactivity is independent of heartbeat, RX audio and FFT traffic.
            fresh.messages.clear()
            viewer.messages.clear()
            fresh.wait(lambda s: isinstance(s, dict) and s.get('type') == 'closed' and s.get('reason') == 'idle', 6)
            wait(lambda: fresh.closed)
            viewer.send({'type': 'state'})
            viewer.state()
            viewer.role('viewer', False)
            final = browser(heartbeat=False)
            final.role('operator')
            final.state()
            for _ in range(35):
                final.send({'type': 'activity'})
                time.sleep(.2)
                if final.closed:
                    break
            final.wait(lambda s: isinstance(s, dict) and s.get('type') == 'closed' and s.get('reason') == 'heartbeat_timeout')
            print('HTTP assets/HEAD/traversal; roles; viewer isolation; FFT/RX audio; command retries; settings; '
                  'slow viewer; TX audio loop; PTT watchdog; confirmed-safe ownership transfer; fresh-login ownership; '
                  'backend restart; inactivity and heartbeat timeout passed')
        finally:
            for c in clients:
                c.close()
            bridge.terminate()
            bridge.wait(timeout=6)
            process.terminate()
            process.wait(timeout=5)
            proxy.close()
            backend_log.close()
            adapter_log.close()
            print((root/'adapter.log').read_text())
        assert bridge.returncode == 0
        assert process.returncode == 0


if __name__ == '__main__':
    main()
