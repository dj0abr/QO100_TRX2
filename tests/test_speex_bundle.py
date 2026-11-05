"""Check the pinned headers/archive and that runtime uses no system SpeexDSP."""
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

root, binary = map(Path, sys.argv[1:])
arch = {'amd64':'x86_64','arm64':'aarch64'}.get(platform.machine().lower(),platform.machine().lower())
bundle=root/'external/speexdsp-1.2.1'
manifest=json.loads((bundle/f'manifest-linux-{arch}.json').read_text())
assert manifest['version']=='1.2.1'
assert manifest['source_sha256']=='8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d'
for name,expected in manifest['sha256'].items():
    assert hashlib.sha256((bundle/name).read_bytes()).hexdigest()==expected,name
dynamic=subprocess.check_output(['readelf','-d',str(binary)],text=True)
assert 'libspeexdsp.so' not in dynamic
symbols=subprocess.check_output(['nm','-g',str(binary)],text=True)
assert ' T speex_preprocess_run' in symbols
assert not list(bundle.rglob('*.c'))
print('SpeexDSP 1.2.1 bundle intact, statically linked; no implementation sources in project')
