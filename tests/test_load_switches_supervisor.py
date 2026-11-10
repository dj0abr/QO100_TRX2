"""Decoder startup switches affect processes, links, preflight and supervision."""
from pathlib import Path
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from test_webbridge import port, private_config, wait
from ws_client import Client


def run_case(script, binaries, defaults, assets, changes, enabled, linger=False, restart=None):
    with tempfile.TemporaryDirectory(prefix='qo100-stack-switches-') as temp:
        root = Path(temp)
        bin_dir = root / 'build/bin'
        bin_dir.mkdir(parents=True)
        (root / 'config').mkdir()
        shutil.copy2(script, root / 'start_all.sh')
        for program in ('qo100_backend', 'pluto', 'qo100_webbridge'):
            (bin_dir / program).symlink_to(binaries / program)
        names = ('qo100_ft8_decoder', 'qo100_beacon_decoder', 'qo100_mm_decoder')
        for name, marker, on in zip(names, ('FT8 receiver: RX3', 'PSK beacon receiver: RX4', 'MM beacon receiver: RX5'), enabled):
            if on:
                program = bin_dir / name
                program.write_text(f'#!/usr/bin/python3\nimport signal\nprint({marker!r},flush=True)\nwhile True: signal.pause()\n')
                program.chmod(0o755)
        if enabled[0]:
            (bin_dir / 'ft8-decoder').symlink_to('/usr/bin/true')
        if enabled[2]:
            (bin_dir / 'qo100_mm_probe').symlink_to('/usr/bin/true')
        # Binaries of disabled decoders deliberately do not exist.
        backend_port, http, audio3, text3, audio4, text4, audio5, mm_http = [port() for _ in range(8)]
        text = private_config(defaults, backend_port)
        text='\n'.join(f'{line.partition("=")[0].strip()} = {changes[line.partition("=")[0].strip()]}'
                       if line.partition('=')[0].strip() in changes else line for line in text.splitlines())+'\n'
        (root / 'config/backend.conf').write_text(text)
        (root / 'config/webbridge.conf').write_text(f'''listen = 127.0.0.1
port = {http}
backend_port = {backend_port}
web_root = {assets}
rx3_decoder_audio_port = {audio3}
rx3_decoder_text_port = {text3}
rx4_decoder_audio_port = {audio4}
rx4_decoder_text_port = {text4}
rx5_decoder_audio_port = {audio5}
mm_http_port = {mm_http}
''')
        held = []
        # Occupied disabled ports must neither fail preflight nor be opened by the bridge.
        for on, endpoints in zip(enabled, (((socket.SOCK_DGRAM, audio3), (socket.SOCK_DGRAM, text3)),
                                           ((socket.SOCK_DGRAM, audio4), (socket.SOCK_DGRAM, text4)),
                                           ((socket.SOCK_DGRAM, audio5), (socket.SOCK_STREAM, mm_http)))):
            if not on:
                for kind, endpoint in endpoints:
                    sock = socket.socket(socket.AF_INET, kind)
                    sock.bind(('127.0.0.1', endpoint))
                    if kind == socket.SOCK_STREAM:
                        sock.listen()
                    held.append(sock)
        log = root / 'supervisor.log'
        seen = set()
        with log.open('w') as output:
            stack = subprocess.Popen(['bash', str(root / 'start_all.sh'), '--source', 'synthetic', '--no-digital-audio'],
                                     stdout=output, stderr=output)
            client = None

            def children():
                result = {}
                path = Path(f'/proc/{stack.pid}/task/{stack.pid}/children')
                for pid in path.read_text().split() if path.exists() else ():
                    try:
                        command = Path(f'/proc/{pid}/cmdline').read_bytes().decode().replace('\0', ' ')
                    except FileNotFoundError:
                        continue
                    for name in ('qo100_backend', 'qo100_webbridge', *names):
                        if str(bin_dir / name) + ' ' in command:
                            result[name] = int(pid)
                            seen.add(int(pid))
                return result

            try:
                wait(lambda: stack.poll() is not None or 'Dieses Terminal offen lassen.' in log.read_text(), timeout=12)
                assert stack.poll() is None, log.read_text()
                expected = {'qo100_backend', 'qo100_webbridge'} | {name for name, on in zip(names, enabled) if on}
                before = children()
                assert set(before) == expected, (changes, before, expected, log.read_text())
                for name, key in zip(names[:2],('rx3_decoder_cpu_percent','rx4_decoder_cpu_percent')):
                    if name in before:
                        command=Path(f'/proc/{before[name]}/cmdline').read_bytes().decode().split('\0')
                        assert command[command.index('--cpu-percent')+1]==str(changes.get(key,10)),command
                client = Client(http)
                client.role('operator')
                state = client.state(lambda s: s.get('ready'))
                assert not state['tx']['ptt'], state
                if linger:
                    time.sleep(2.2)
                    assert children() == before, 'Disabled decoder started during supervision'
                if restart is not None:
                    name = names[restart]
                    os.kill(before[name], signal.SIGTERM)
                    wait(lambda: children().get(name, before[name]) != before[name], timeout=6)
                    after = children()
                    assert all(after[key] == pid for key, pid in before.items() if key != name)
                    assert set(after) == expected
                    key='rx3_decoder_cpu_percent' if restart==0 else 'rx4_decoder_cpu_percent' if restart==1 else None
                    if key:
                        command=Path(f'/proc/{after[name]}/cmdline').read_bytes().decode().split('\0')
                        assert command[command.index('--cpu-percent')+1]==str(changes.get(key,10)),command
            finally:
                if client:
                    client.close()
                if stack.poll() is None:
                    stack.terminate()
                    stack.wait(timeout=12)
                for sock in held:
                    sock.close()
                if sys.exc_info()[0]:
                    print(log.read_text())
            assert stack.returncode == 143, log.read_text()
            assert not any(Path(f'/proc/{pid}').exists() for pid in seen), 'Supervisor leaked a child'


def main():
    script, binaries, defaults, assets = map(Path, sys.argv[1:])
    cases = [
        ({'external_decoders_enabled': 0}, (False, False, False), True, None),
        ({'rx3_decoder_enabled': 0, 'rx5_decoder_enabled': 0,'rx4_decoder_cpu_percent':5}, (False, True, False), False, 1),
        ({'rx3_enabled': 0, 'rx5_enabled': 0}, (False, True, False), False, None),
        ({'rx4_decoder_enabled': 0}, (True, False, True), False, 2),
        ({f'rx{rx}_enabled': 0 for rx in range(2, 6)}, (False, False, False), False, None),
        ({f'rx{rx}_decoder_enabled': 0 for rx in range(3, 6)}, (False, False, False), False, None),
    ]
    for changes, enabled, linger, restart in cases:
        run_case(script, binaries, defaults, assets, changes, enabled, linger, restart)
    print('Decoder switches: master/individual/disabled RX, absent binaries, occupied disabled ports, WebGUI ready, optional-child restart and cleanup passed')


if __name__ == '__main__':
    main()
