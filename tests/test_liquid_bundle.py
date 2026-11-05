"""Check shipped binary integrity, ABI version, and static linkage of both programs."""
import ctypes
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

project, backend, pluto = map(Path, sys.argv[1:])
arch = {'amd64': 'x86_64', 'arm64': 'aarch64'}.get(platform.machine().lower(), platform.machine().lower())
bundle = project/'external/liquid-dsp-1.8.3'
manifest = json.loads((bundle/f'manifest-linux-{arch}.json').read_text())
assert manifest['version'] == '1.8.3'
assert manifest['commit'] == '10041f70cebbe3b97887e75bb41e48b73dda1b23'
for relative, expected in manifest['sha256'].items():
    assert hashlib.sha256((bundle/relative).read_bytes()).hexdigest() == expected, relative
lib = ctypes.CDLL(str(bundle/f'lib/linux-{arch}/libliquid.so.1.8.3'))
lib.liquid_libversion.restype = ctypes.c_char_p
assert lib.liquid_libversion() == b'1.8.3'
for binary in (backend, pluto):
    dynamic = subprocess.check_output(['readelf', '-d', str(binary)], text=True)
    assert 'libliquid.so' not in dynamic, f'{binary} unexpectedly uses system/shared liquid'
print('Pinned 1.8.3 archive/header/shared library intact; backend and driver link liquid statically.')
