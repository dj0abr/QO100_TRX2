"""Verify the standalone C++ GUI example against an isolated synthetic backend."""
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile


def main():
    example, backend, defaults = map(Path, sys.argv[1:])
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix='qo100-cpp-gui-') as temp:
        root = Path(temp)
        config = root/'backend.conf'
        config.write_text(defaults.read_text().replace('gui_port = 40900', f'gui_port = {port}'))
        with (root/'backend.log').open('w') as log:
            process = subprocess.Popen([str(backend), '--config', str(config), '--source', 'synthetic'],
                                       stdout=log, stderr=log)
            try:
                result = subprocess.run([str(example), '127.0.0.1', '--port', str(port), '--seconds', '1.5',
                                         '--rx-hz', '10489748500', '--set', 'gui_audio_gain=5.5'],
                                        text=True, capture_output=True, timeout=16)
                print(result.stdout)
                assert result.returncode == 0, result.stderr
                assert 'SETTINGS saved revision=' in result.stdout
                match = re.search(r'RX1 packets=(\d+) RX2 packets=(\d+) wide_fragments=(\d+) zoom_fragments=(\d+)', result.stdout)
                assert match and all(int(n) > 20 for n in match.groups()), result.stdout
                saved = dict(line.split('=', 1) for line in config.with_suffix('.settings').read_text().splitlines())
                assert saved['gui_audio_gain'] == '5.5'
                assert saved['initial_rx_hz'] == '10489748500'
            finally:
                process.terminate()
                process.wait(timeout=5)
        assert process.returncode == 0, (root/'backend.log').read_text()
        assert 'software PTT=1' not in (root/'backend.log').read_text()
    print('C++ client: standalone codec, discovery, durable settings, RX tuning, PCM and both FFT streams passed')


if __name__ == '__main__':
    main()
