"""Private synthetic stack: a monitor restart never restarts the station or its peer."""
from pathlib import Path
import os
import shutil
import signal
import subprocess
import sys
import tempfile
from test_webbridge import port, private_config, wait


def main():
    script, binaries, defaults, assets = map(Path, sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='qo100-supervisor-') as temp:
        root = Path(temp); (root/'build').mkdir(); (root/'config').mkdir()
        shutil.copy2(script, root/'start_all.sh')
        (root/'build/bin').mkdir()
        for binary in binaries.iterdir():
            if binary.name != 'qo100_digital_audio': (root/'build/bin'/binary.name).symlink_to(binary)
        audio_stub = root/'build/bin/qo100_digital_audio'
        audio_stub.write_text('#!/usr/bin/python3\nimport signal\nprint("Digital audio: private supervisor stub",flush=True)\nwhile True: signal.pause()\n')
        audio_stub.chmod(0o755)
        (root/'config/backend.conf').write_text(private_config(defaults, port()))
        backend_port = next(line.split('=')[1].strip() for line in (root/'config/backend.conf').read_text().splitlines() if line.startswith('gui_port ='))
        audio3, text3, audio4, text4, http = [port() for _ in range(5)]
        digital_port, digital_bridge_port = port(), port()
        (root/'config/webbridge.conf').write_text(f'''listen = 127.0.0.1
port = {http}
backend_port = {backend_port}
web_root = {assets}
rx3_decoder_audio_port = {audio3}
rx3_decoder_text_port = {text3}
rx4_decoder_audio_port = {audio4}
rx4_decoder_text_port = {text4}
digital_audio_port = {digital_port}
digital_bridge_port = {digital_bridge_port}
''')
        log = root/'supervisor.log'
        with log.open('w') as output:
            stack = subprocess.Popen(['bash', str(root/'start_all.sh'), '--source', 'synthetic'], stdout=output, stderr=output)
            seen = set()
            def children():
                result = {}
                path = Path(f'/proc/{stack.pid}/task/{stack.pid}/children')
                for pid in path.read_text().split() if path.exists() else ():
                    try:
                        command = Path(f'/proc/{pid}/cmdline').read_bytes().decode().replace('\0', ' ')
                    except FileNotFoundError:
                        continue
                    for name in ('qo100_backend', 'qo100_webbridge', 'qo100_ft8_decoder', 'beacon_decoder.py', 'qo100_digital_audio'):
                        if name in command:
                            result[name] = int(pid); seen.add(int(pid))
                return result
            try:
                wait(lambda: 'RX3 FT8 und RX4 PSK laufen.' in log.read_text(), timeout=15)
                before = children(); assert len(before) == 5, before
                ft8 = 'qo100_ft8_decoder'; psk = 'beacon_decoder.py'
                os.kill(before[ft8], signal.SIGTERM)
                wait(lambda: children().get(ft8, before[ft8]) != before[ft8], timeout=6)
                after = children()
                assert all(after[name] == pid for name, pid in before.items() if name != ft8)
                os.kill(after[psk], signal.SIGTERM)
                wait(lambda: children().get(psk, after[psk]) != after[psk], timeout=6)
                restarted = children()
                assert all(restarted[name] == pid for name, pid in after.items() if name != psk)
                audio = 'qo100_digital_audio'
                os.kill(restarted[audio], signal.SIGTERM)
                wait(lambda: children().get(audio, restarted[audio]) != restarted[audio], timeout=6)
                after_audio = children()
                assert all(after_audio[name] == pid for name, pid in restarted.items() if name != audio)
                os.kill(restarted['qo100_backend'], signal.SIGTERM)
                assert stack.wait(timeout=12) == 1, log.read_text()
                wait(lambda: not any(Path(f'/proc/{pid}').exists() for pid in seen), timeout=4)
                print('Supervisor: FT8/PSK/audio adapter restart independently, core failure cleans up every child; no hardware accessed')
            finally:
                if stack.poll() is None:
                    stack.terminate(); stack.wait(timeout=12)
                if sys.exc_info()[0]: print(log.read_text())


if __name__ == '__main__':
    main()
