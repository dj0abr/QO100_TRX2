"""Demand-driven RX2/TX2 through real bridge/backend; private synthetic IQ only."""
from pathlib import Path
import os
import shutil
import threading
import numpy as np
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from test_webbridge import HEADER, Proxy, port, private_config, wait
from ws_client import Client


def main():
    bridge_bin, backend_bin, defaults, assets, audio_bin = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-idle-') as temp:
        root = Path(temp)
        bp, http, ap, dp = [port() for _ in range(4)]
        conf = root/'backend.conf'
        conf.write_text(private_config(defaults, bp))
        logs = [(root/name).open('w') for name in ('backend.log', 'bridge.log')]
        radio = subprocess.Popen([str(backend_bin), '--config', str(conf), '--source', 'synthetic'], stdout=logs[0], stderr=logs[0])
        proxy = Proxy(bp)
        received = []
        proxy.drop = lambda p: received.append((time.monotonic(), HEADER.unpack_from(p))) or False
        bridge = subprocess.Popen([str(bridge_bin), '--listen', '127.0.0.1', '--port', str(http),
            '--backend-port', str(proxy.port), '--web-root', str(assets),
            '--digital-audio-port', str(ap), '--digital-bridge-port', str(dp)], stdout=logs[1], stderr=logs[1])
        digital = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        digital.bind(('127.0.0.1', ap)); digital.connect(('127.0.0.1', dp)); digital.setblocking(False)
        clients = []; nonce = 54321; status = None

        def pump():
            nonlocal status
            while True:
                try: packet = digital.recv(65536)
                except (BlockingIOError, ConnectionRefusedError): break
                h = HEADER.unpack_from(packet)
                if h[2] == 7 and h[6] == nonce: status = h

        def local_usage(flags, bad_nonce=False, bad_session=False):
            digital.send(HEADER.pack(b'QTR2', 1, 20, 64, 7, 0, nonce+int(bad_nonce), 0, 0, 0, 0,
                flags, 0, 0, 0, initial['session']+int(bad_session)))

        def subscribe(client, enabled):
            client.send({'type': 'audio_subscription', 'stream': 2, 'enabled': enabled})

        def observe(rx, tx, external=None, seconds=1.4):
            start = time.monotonic(); next_report = start
            while time.monotonic()-start < seconds:
                pump()
                if external is not None and time.monotonic() >= next_report:
                    local_usage(external); next_report += .15
                time.sleep(.005)
            pump()
            recent = [h for at, h in received if at > time.monotonic()-.25]
            assert any(h[2] == 5 and h[4] == 2 for h in recent) == rx, ('RX2', rx, recent[-5:])
            assert status is not None and bool(status[11]) == tx, ('TX2', tx, status)
            assert {h[4] for h in recent if h[2] == 5} >= {1, 5, 6, 8}, 'Other receivers interrupted'
            assert {h[4] for h in recent if h[2] == 2} == {0, 1, 2}, 'Waterfall interrupted'

        try:
            def available():
                try: return urllib.request.urlopen(f'http://127.0.0.1:{http}/health', timeout=.2).status == 200
                except OSError: return False
            wait(available)
            owner = Client(http); clients.append(owner); owner.role('operator'); initial = owner.state()
            assert initial['settings']['digital_activity_control'] == '1'
            viewer = Client(http); clients.append(viewer); viewer.role('viewer'); viewer.state()
            # No adapter: all current browsers explicitly decline RX2 audio.
            subscribe(owner, False); subscribe(viewer, False)
            time.sleep(1.3)
            recent = [h for at, h in received if at > time.monotonic()-.2]
            assert not any(h[2] == 5 and h[4] == 2 for h in recent), 'Unused RX2 still runs'
            before = [h for _, h in received if h[2] == 5 and h[4] == 1][-1]
            digital.send(HEADER.pack(b'QTR2', 1, 19, 64, 7, 0, nonce, 0, 0, 0, 0, 0, 0, 0, 0, 0))
            wait(lambda: (pump() or True) and status is not None)
            observe(True, True, seconds=2.5)  # Older adapters have no periodic usage reports.
            observe(False, False, 0)
            idle_epoch = [h for _, h in received if h[2] == 5 and h[4] == 2][-1][5]
            start = time.monotonic()
            subscribe(viewer, True)
            wait(lambda: any(at > start and h[2] == 5 and h[4] == 2 and h[5] != idle_epoch and h[7] == 0 for at, h in received))
            observe(True, False, 0)
            subscribe(owner, True); subscribe(viewer, False)
            observe(True, False, 0)
            subscribe(owner, False)
            observe(False, False, 0)
            # External directions are independent and remain available headless.
            owner.close(); viewer.close(); clients.clear()
            observe(True, False, 1)
            observe(False, True, 2)
            observe(False, False, 0)
            # An old adapter identity or backend session cannot wake either chain.
            for _ in range(4):
                local_usage(3, bad_nonce=True); local_usage(3, bad_session=True); time.sleep(.05)
            observe(False, False, 0)
            # Positive demand is a lease: a vanished adapter cannot pin DSP awake.
            observe(True, True, 3)
            observe(False, False, seconds=3.2)
            after = [h for _, h in received if h[2] == 5 and h[4] == 1][-1]
            assert before[5] == after[5] and after[7] > before[7], 'RX1 was reset by digital idle'
            # Older browsers without the subscription message retain RX2 audio.
            legacy = Client(http); clients.append(legacy); legacy.state()
            observe(True, False)
            legacy.close(); clients.clear(); digital.close()
            # Complete real path: PA subscriptions -> adapter nonce/usage ->
            # bridge -> backend readiness -> PCM, including wake from ready=false.
            if all(shutil.which(name) for name in ('pulseaudio','pactl','parec','pacat')):
                native = root/'native'
                env = {**os.environ,'PULSE_SERVER':'unix:'+str(native),
                    'PULSE_RUNTIME_PATH':str(root/'pulse-runtime'),
                    'DBUS_SESSION_BUS_ADDRESS':'unix:path='+str(root/'no-dbus')}
                pulse_log=(root/'pulse.log').open('w'); audio_log=(root/'audio.log').open('w')
                pulse=subprocess.Popen(['pulseaudio','-n','--daemonize=no','--exit-idle-time=-1','--use-pid-file=no',
                    '--disable-shm=yes','--load=module-native-protocol-unix socket='+str(native)+' auth-anonymous=1'],
                    env=env,stdout=pulse_log,stderr=pulse_log)
                adapter=recorder=player=None; writing=threading.Event(); captured=[]
                prefix=f'qo100_idle_{os.getpid()}'
                def demand():
                    values=[HEADER.unpack_from(packet)[11] for packet in proxy.log if packet[5]==20]
                    return values[-1] if values else None
                try:
                    wait(native.exists)
                    adapter=subprocess.Popen([str(audio_bin),'--port',str(ap),'--bridge-port',str(dp),
                        '--device-prefix',prefix],env=env,stdout=audio_log,stderr=audio_log)
                    def devices():
                        return prefix in subprocess.check_output(['pactl','list','short','sinks'],env=env).decode()
                    wait(lambda:'Digital audio: Input fromQO100=' in (root/'audio.log').read_text())
                    wait(devices); wait(lambda:demand()==0)
                    recorder=subprocess.Popen(['parec','--raw','--format=s16le','--rate=12000','--channels=1',
                        '--latency-msec=40','--device='+prefix+'_fromQO100'],env=env,stdout=subprocess.PIPE,stderr=audio_log)
                    def read():
                        while True:
                            data=recorder.stdout.read(480)
                            if not data: return
                            captured.append(data)
                    threading.Thread(target=read,daemon=True).start()
                    wait(lambda:demand()==1)
                    wait(lambda:sum(map(len,captured))>=36000)
                    pcm=np.frombuffer(b''.join(captured),dtype='<i2')[-12000:].astype(float)
                    spectrum=np.abs(np.fft.rfft(pcm*np.hanning(len(pcm))))
                    assert abs(np.argmax(spectrum)-1700)<10 and np.max(np.abs(pcm))>500,'Actual RX2 audio did not wake'
                    recorder.terminate();recorder.wait(timeout=3);wait(lambda:demand()==0)
                    time.sleep(.9)
                    start=time.monotonic(); log_start=len(proxy.log)
                    player=subprocess.Popen(['pacat','--playback','--raw','--format=s16le','--rate=12000','--channels=1',
                        '--latency-msec=40','--device='+prefix+'_toQO100'],env=env,stdin=subprocess.PIPE,stderr=audio_log)
                    writing.set()
                    def write():
                        try:
                            while writing.is_set(): player.stdin.write(bytes(480));player.stdin.flush();time.sleep(.02)
                        except (BrokenPipeError,ValueError): pass
                    threading.Thread(target=write,daemon=True).start()
                    wait(lambda:demand()==2)
                    def transmitted(): return [packet for packet in proxy.log[log_start:] if packet[5]==8]
                    wait(lambda:len(transmitted())>15)
                    tx_packets=transmitted()
                    assert HEADER.unpack_from(tx_packets[0])[7]==0,'Actual TX2 wake reused old sample indices'
                    assert all(not any(packet[64:]) for packet in tx_packets),'Actual TX2 wake replayed stale PCM'
                    time.sleep(.4)
                    assert not any(at>start+.3 and h[2]==5 and h[4]==2 for at,h in received),'TX-only client woke RX2'
                    writing.clear();player.terminate();player.wait(timeout=3);wait(lambda:demand()==0)
                    print('Real audio stack: automatic RX2 tone and silent TX2 wake from idle, independent closing and fresh TX timeline passed')
                finally:
                    writing.clear()
                    for process in (adapter,player,recorder,pulse):
                        if process and process.poll() is None: process.terminate();process.wait(timeout=6)
                    pulse_log.close();audio_log.close()
                    if sys.exc_info()[0]:
                        print((root/'audio.log').read_text());print((root/'pulse.log').read_text())
            print('Digital idle: RX/TX independent demand, browser/viewer arbitration, fresh RX epoch, lease expiry, stale rejection, legacy browser and uninterrupted RX1/monitors/waterfalls passed')
        finally:
            for client in clients: client.close()
            digital.close(); proxy.close()
            for process in (bridge, radio):
                if process.poll() is None: process.terminate(); process.wait(timeout=8)
            for log in logs: log.close()
            if sys.exc_info()[0]:
                for name in ('backend.log', 'bridge.log'): print((root/name).read_text())


if __name__ == '__main__':
    main()
