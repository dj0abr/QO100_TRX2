"""Real virtual audio devices on a private PulseAudio server; no physical devices or RF."""
from pathlib import Path
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import numpy as np
from test_webbridge import HEADER, port, wait


def main():
    binary = Path(sys.argv[1])
    if not all(shutil.which(p) for p in ('pulseaudio', 'pactl', 'pacat', 'parec')):
        print('Private PulseAudio test requires pulseaudio and pulseaudio-utils'); return 77
    with tempfile.TemporaryDirectory(prefix='qo100-pulse-') as temp:
        root = Path(temp); native = root/'native'
        env = {**os.environ, 'PULSE_SERVER': 'unix:'+str(native),
               'DBUS_SESSION_BUS_ADDRESS': 'unix:path='+str(root/'no-session-bus'),
               'PULSE_RUNTIME_PATH': str(root/'runtime')}
        prefix = f'qo100_test_{os.getpid()}'
        logs = [(root/name).open('w') for name in ('pulse.log', 'adapter.log', 'clients.log')]
        server = subprocess.Popen(['pulseaudio', '-n', '--daemonize=no', '--exit-idle-time=-1', '--use-pid-file=no',
            '--disable-shm=yes', '--load=module-native-protocol-unix socket='+str(native)+' auth-anonymous=1'], stdout=logs[0], stderr=logs[0], env=env)
        bridge_port, audio_port = port(), port()
        udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); udp.bind(('127.0.0.1', bridge_port)); udp.settimeout(.05)
        adapter = player = recorder = None; chunks = []; writing = threading.Event(); writing.set()
        threads = []
        try:
            wait(native.exists)
            adapter = subprocess.Popen([str(binary), '--port', str(audio_port), '--bridge-port', str(bridge_port),
                '--device-prefix', prefix], stdout=logs[1], stderr=logs[1], env=env)
            wait(lambda: 'Digital audio: RX2 input ' in (root/'adapter.log').read_text())
            recorder = subprocess.Popen(['parec', '--raw', '--format=s16le', '--rate=48000', '--channels=1',
                '--latency-msec=40', '--device='+prefix+'_rx2_input'], stdout=subprocess.PIPE, stderr=logs[2], env=env)
            player = subprocess.Popen(['pacat', '--playback', '--raw', '--format=s16le', '--rate=48000', '--channels=1',
                '--latency-msec=40', '--device='+prefix+'_tx2'], stdin=subprocess.PIPE, stderr=logs[2], env=env)
            def read_audio():
                while True:
                    data = recorder.stdout.read(1920)
                    if not data: return
                    chunks.append((time.monotonic(), data))
            def write_audio():
                index = 0
                try:
                    while writing.is_set():
                        pcm = np.rint(.25*32767*np.cos(2*np.pi*1700*(index+np.arange(960))/48000)).astype('<i2')
                        player.stdin.write(pcm.tobytes()); player.stdin.flush(); index += 960
                except (BrokenPipeError, ValueError): pass
            for target in (read_audio, write_audio):
                thread = threading.Thread(target=target, daemon=True); thread.start(); threads.append(thread)
            # Handshake echoes a nonce only after a fresh TX2 epoch is ready.
            nonce = None; tx = []; index = 0; epoch = 7
            def state(ready=True, status=0):
                return HEADER.pack(b'QTR2', 1, 7, 64, 7, 1, nonce, epoch, 2400100000, 12000, 0, int(ready), 0, status, 0, 99)
            start = time.monotonic(); tick = start
            while time.monotonic() < start+3:
                if time.monotonic() >= tick:
                    if nonce is not None: udp.sendto(state(), ('127.0.0.1', audio_port))
                    pcm = np.rint(.2*32767*np.cos(2*np.pi*1000*(index+np.arange(240))/12000)).astype('>i2')
                    rx = HEADER.pack(b'QTR2', 1, 5, 64, 2, 3, time.time_ns(), index, 10489600000, 12000, 0, 0, 240, 2, 240, 99)
                    udp.sendto(rx+pcm.tobytes(), ('127.0.0.1', audio_port)); index += 240; tick += .02
                try: packet, source = udp.recvfrom(65536)
                except socket.timeout: continue
                assert source == ('127.0.0.1', audio_port)
                h = HEADER.unpack_from(packet)
                if h[2] == 19: nonce = h[6]
                elif h[2] == 8:
                    assert (h[4], h[5], h[8], h[9], h[15]) == (7, epoch, 2400100000, 12000, 99)
                    tx.append((h[7], np.frombuffer(packet, dtype='>i2', offset=64).copy()))
            assert len(tx) > 60 and tx[0][0] == 0, (len(tx), [p[0] for p in tx[:4]], adapter.poll())
            assert all(b[0] == a[0]+240 for a, b in zip(tx, tx[1:])), 'TX2 audio timeline has gaps'
            def tone(samples, rate, expected, level):
                samples = samples.astype(float)/32768
                spectrum = np.abs(np.fft.rfft(samples*np.hanning(len(samples))))*4/len(samples)
                peak = np.argmax(spectrum)
                assert abs(peak*rate/len(samples)-expected) < 10
                assert level*.7 < spectrum[peak] < level*1.2, spectrum[peak]
            tone(np.concatenate([p for _, p in tx])[-12000:], 12000, 1700, .25)
            captured = b''.join(data for _, data in chunks)
            assert len(captured) > 96000
            tone(np.frombuffer(captured, dtype='<i2')[-48000:], 48000, 1000, .2)
            # A new epoch starts at zero, and no audio is sent while TX2 is unavailable.
            udp.setblocking(False)
            while True:
                try: udp.recvfrom(65536)
                except BlockingIOError: break
            udp.settimeout(.05)
            udp.sendto(state(False), ('127.0.0.1', audio_port)); time.sleep(.12)
            while True:
                try: udp.recvfrom(65536)
                except socket.timeout: break
            udp.sendto(state(False), ('127.0.0.1', audio_port))
            udp.settimeout(.12)
            try:
                packet, _ = udp.recvfrom(65536)
                assert HEADER.unpack_from(packet)[2] != 8, 'Disabled TX2 emitted audio'
            except socket.timeout: pass
            udp.sendto(state(), ('127.0.0.1', audio_port))
            udp.settimeout(1)
            packet, _ = udp.recvfrom(65536); h = HEADER.unpack_from(packet)
            assert h[2] == 8 and h[5] == epoch and h[7] > 0, h
            epoch += 1; udp.sendto(state(), ('127.0.0.1', audio_port))
            while True:
                packet, _ = udp.recvfrom(65536); h = HEADER.unpack_from(packet)
                if h[2] == 8 and h[5] == epoch:
                    assert h[7] == 0, h
                    break
            udp.sendto(state(status=1), ('127.0.0.1', audio_port)); time.sleep(.08)
            udp.setblocking(False)
            while True:
                try: udp.recvfrom(65536)
                except BlockingIOError: break
            udp.settimeout(.2)
            packet, _ = udp.recvfrom(65536)
            assert HEADER.unpack_from(packet)[2] == 8, 'Rejected tuning command disabled a valid audio path'
            adapter.terminate(); assert adapter.wait(timeout=6) == 0
            devices = subprocess.check_output(['pactl', 'list', 'short', 'sinks'], env=env).decode()
            assert prefix not in devices, 'Owned virtual devices leaked'
            print('Virtual audio: real 48/12-kHz RX2/TX2 tones, continuous TX clock, nonce/epoch reset, disabled silence and device cleanup passed on private PulseAudio')
        finally:
            writing.clear()
            for process in (adapter, player, recorder, server):
                if process and process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=6)
                    except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=3)
            udp.close()
            for log in logs: log.close()
            if sys.exc_info()[0]:
                for name in ('pulse.log', 'adapter.log', 'clients.log'): print((root/name).read_text())
    return 0


if __name__ == '__main__':
    sys.exit(main())
