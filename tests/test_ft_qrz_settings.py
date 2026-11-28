"""Exercise write-only QRZ settings with a private Webbridge and dummy keys."""
from contextlib import ExitStack, contextmanager
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

from test_webbridge import port, wait
from ws_client import Client


def main():
    bridge = Path(sys.argv[1])
    with tempfile.TemporaryDirectory(prefix='qo100-qrz-') as tmp:
        root = Path(tmp)
        http, backend = port(), port()
        url = f'http://127.0.0.1:{http}'
        logbook = root / 'ft4-ft8.adi'
        saved = root / 'ft4-ft8.adi.qrz-key'
        legacy = 'legacy-test-key'
        key = 'saved-test-key'

        def running():
            try:
                with urllib.request.urlopen(url + '/health', timeout=.2) as response:
                    return response.status == 200
            except OSError:
                return False

        @contextmanager
        def launch(env_key):
            with ExitStack() as stack:
                process = subprocess.Popen(
                    [bridge, '--listen', '127.0.0.1', '--port', str(http), '--backend-port', str(backend),
                     '--web-root', root, '--ft-enabled', '1', '--ft-log-path', logbook],
                    env={**os.environ, 'QO100_QRZ_API_KEY': env_key})

                def stop():
                    process.terminate()
                    assert process.wait(timeout=5) == 0

                stack.callback(stop)
                wait(running)
                owner = Client(http)
                stack.callback(owner.close)
                owner.role('operator')
                yield owner

        def configured(client, value):
            client.state(lambda s: s['ft']['qrz_configured'] is value)

        def change(client, request_id, value):
            client.send({'type': 'ft_qrz_key', 'id': request_id, 'key': value})
            return client.wait(lambda m: isinstance(m, dict) and m.get('id') == request_id)

        with launch(legacy) as owner:
            configured(owner, True)
            viewer = Client(http)
            try:
                viewer.role('viewer')
                assert change(viewer, 1, key)['code'] == 'read_only'
                assert not saved.exists()
                assert change(owner, 2, key + '\n')['code'] == 'invalid'
                assert not saved.exists()

                saved.mkdir()  # A persistence error must leave the active key installed.
                assert change(owner, 3, '')['type'] == 'error'
                owner.messages.clear()
                owner.send({'type': 'state'})
                configured(owner, True)
                assert not list(root.glob('*.tmp-*.qrz-key'))
                saved.rmdir()

                assert change(owner, 4, f' {key} ')['status'] == 0
                assert saved.read_text() == key + '\n'
                assert stat.S_IMODE(saved.stat().st_mode) == 0o600
                try:
                    urllib.request.urlopen(url + '/' + saved.name, timeout=2)
                    raise AssertionError('HTTP exposed the saved QRZ key')
                except urllib.error.HTTPError as error:
                    assert error.code == 404
                for client in (owner, viewer):
                    client.send({'type': 'state'})
                    configured(client, True)
                    assert all(key not in str(m) and legacy not in str(m) for m in list(client.messages))
            finally:
                viewer.close()

        with launch('') as owner:
            configured(owner, True)  # Reload from disk without an environment key.
            assert change(owner, 5, '')['status'] == 0
            configured(owner, False)

        with launch(legacy) as owner:
            configured(owner, False)  # Explicit removal overrides the legacy fallback.


if __name__ == '__main__':
    main()
