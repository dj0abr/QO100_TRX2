#!/usr/bin/env python3
"""Build the pinned SpeexDSP archive; download sources only into a temporary directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

VERSION = '1.2.1'
URL = f'https://downloads.xiph.org/releases/speex/speexdsp-{VERSION}.tar.gz'
SHA256 = '8c777343e4a6399569c72abc38a95b24db56882c83dbdb6c6424a5f4aeb54d3d'
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    arch = {'amd64': 'x86_64', 'arm64': 'aarch64'}.get(platform.machine().lower(), platform.machine().lower())
    if platform.system() != 'Linux' or arch not in ('x86_64', 'aarch64') or args.jobs < 1:
        parser.error('Requires Linux x86-64/aarch64 and positive --jobs')
    bundle = ROOT/'external'/f'speexdsp-{VERSION}'
    flags = ['--disable-shared', '--enable-static', '--disable-examples', '--disable-sse',
             '--disable-neon', '--with-fft=smallft', '--with-pic']
    cflags = '-O2 -march=' + ('x86-64' if arch == 'x86_64' else 'armv8-a')
    with tempfile.TemporaryDirectory(prefix='qo100-speex-build-') as temp:
        temp = Path(temp)
        archive = temp/'source.tar.gz'
        urllib.request.urlretrieve(URL, archive)
        digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
        if digest(archive) != SHA256:
            raise RuntimeError('SpeexDSP source checksum mismatch')
        with tarfile.open(archive) as tar:
            tar.extractall(temp, filter='data')
        source = temp/f'speexdsp-{VERSION}'
        subprocess.run([str(source/'configure'), *flags], cwd=source,
                       env={**os.environ, 'CFLAGS': cflags}, check=True)
        subprocess.run(['make', '-j', str(args.jobs)], cwd=source, check=True)
        include, libs = bundle/'include/speex', bundle/f'lib/linux-{arch}'
        include.mkdir(parents=True, exist_ok=True)
        libs.mkdir(parents=True, exist_ok=True)
        copied = []
        for name in ('speex_preprocess.h', 'speexdsp_types.h', 'speexdsp_config_types.h'):
            dest = include/name
            shutil.copy2(source/'include/speex'/name, dest)
            copied.append(dest)
        for src, dest in ((source/'COPYING', bundle/'LICENSE'),
                          (source/'libspeexdsp/.libs/libspeexdsp.a', libs/'libspeexdsp.a')):
            shutil.copy2(src, dest)
            copied.append(dest)
        manifest = {'version': VERSION, 'upstream': 'https://github.com/xiph/speexdsp',
                    'source_url': URL, 'source_sha256': SHA256, 'platform': f'Linux {arch}',
                    'configure': flags, 'CFLAGS': cflags,
                    'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
                    'sha256': {str(p.relative_to(bundle)): digest(p) for p in copied}}
        (bundle/f'manifest-linux-{arch}.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(f'Bundled SpeexDSP {VERSION}: static archive, public headers, license; temporary sources removed')


if __name__ == '__main__':
    main()
