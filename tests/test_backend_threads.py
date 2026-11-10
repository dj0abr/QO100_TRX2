"""Verify real backend worker names and graceful stop from any DSP thread."""
import ctypes
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

libc = ctypes.CDLL(None, use_errno=True)
libc.tgkill.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]
libc.tgkill.restype = ctypes.c_int
names = ('qo100_backend', 'qo100-rx1', 'qo100-rx2-3', 'qo100-rx4-5', 'qo100-tx')

with tempfile.TemporaryDirectory(prefix='qo100-thread-test-') as temp:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    config = Path(temp) / 'backend.conf'
    config.write_text(Path(sys.argv[2]).read_text().replace('gui_port = 40900', f'gui_port = {port}'))
    for target in names:
        for sig in (signal.SIGINT, signal.SIGTERM):
            backend = subprocess.Popen([sys.argv[1], '--config', str(config), '--source', 'synthetic'],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                deadline = time.monotonic() + 2
                threads = {}
                while time.monotonic() < deadline:
                    assert backend.poll() is None, 'Backend exited during worker startup'
                    threads = {p.joinpath('comm').read_text().strip(): int(p.name)
                               for p in Path(f'/proc/{backend.pid}/task').iterdir()}
                    if set(threads) == set(names):
                        break
                    time.sleep(.01)
                assert set(threads) == set(names), threads
                # Let several real synthetic RX/TX blocks execute before stopping.
                time.sleep(.06)
                assert libc.tgkill(backend.pid, threads[target], sig) == 0, ctypes.get_errno()
                output, _ = backend.communicate(timeout=2)
                assert backend.returncode == 0 and 'Stopping; IQ samples=' in output, output
                assert not Path(f'/proc/{backend.pid}').exists(), 'Backend/worker cleanup failed'
            finally:
                if backend.poll() is None:
                    backend.kill()
                    backend.communicate(timeout=2)
print('Five named backend threads: SIGINT/SIGTERM delivered to each thread; graceful stop and cleanup passed')
