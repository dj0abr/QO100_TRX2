#!/usr/bin/env python3
"""Rebuild the pinned binary bundle; keep all downloaded sources outside the project."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

VERSION = '1.8.3'
COMMIT = '10041f70cebbe3b97887e75bb41e48b73dda1b23'
URL = f'https://codeload.github.com/jgaeddert/liquid-dsp/tar.gz/refs/tags/v{VERSION}'
SHA256 = '18fa83b73db8bb6fe6ea0376e4b5aecf8645970f4604d10d9dadbf609f3f95e2'
ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    arch = platform.machine().lower()
    arch = {'amd64': 'x86_64', 'arm64': 'aarch64'}.get(arch, arch)
    if platform.system() != 'Linux' or arch not in ('x86_64', 'aarch64'):
        parser.error('This helper supports Linux x86-64 and aarch64')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    bundle = ROOT/'external'/f'liquid-dsp-{VERSION}'
    flags = ['-DCMAKE_BUILD_TYPE=Release',
             f'-DCMAKE_C_FLAGS=-march={"x86-64" if arch == "x86_64" else "armv8-a"}',
             f'-DCMAKE_CXX_FLAGS=-march={"x86-64" if arch == "x86_64" else "armv8-a"}',
             '-DBUILD_EXAMPLES=OFF', '-DBUILD_AUTOTESTS=OFF', '-DBUILD_BENCHMARKS=OFF',
             '-DBUILD_SHARED_LIBS=ON', '-DBUILD_STATIC_LIBS=ON', '-DFIND_FFTW=OFF',
             '-DENABLE_SIMD=ON', '-DENABLE_LOGGING=OFF', '-DENABLE_TIMESTAMPS=OFF']
    with tempfile.TemporaryDirectory(prefix='qo100-liquid-build-') as temp:
        temp = Path(temp)
        archive = temp/'source.tar.gz'
        urllib.request.urlretrieve(URL, archive)
        if digest(archive) != SHA256:
            raise RuntimeError('Downloaded source checksum mismatch')
        with tarfile.open(archive) as tar:
            tar.extractall(temp, filter='data')
        source, build = temp/f'liquid-dsp-{VERSION}', temp/'build'
        subprocess.run(['cmake', '-S', str(source), '-B', str(build), *flags], check=True)
        subprocess.run(['cmake', '--build', str(build), '-j', str(args.jobs)], check=True)
        include, libs = bundle/'include/liquid', bundle/f'lib/linux-{arch}'
        include.mkdir(parents=True, exist_ok=True)
        libs.mkdir(parents=True, exist_ok=True)
        files = {source/'include/liquid.h': include/'liquid.h', source/'LICENSE': bundle/'LICENSE',
                 build/'libliquid.a': libs/'libliquid.a', build/f'libliquid.so.{VERSION}': libs/f'libliquid.so.{VERSION}'}
        for src, dest in files.items():
            shutil.copy2(src, dest)
        for name, target in [('libliquid.so.1', f'libliquid.so.{VERSION}'), ('libliquid.so', 'libliquid.so.1')]:
            link = libs/name
            link.unlink(missing_ok=True)
            link.symlink_to(target)
        manifest = {'version': VERSION, 'upstream': 'https://github.com/jgaeddert/liquid-dsp',
                    'tag': f'v{VERSION}', 'commit': COMMIT, 'source_url': URL, 'source_sha256': SHA256,
                    'platform': f'Linux {arch} GNU libc',
                    'compiler': subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
                    'build_flags': flags,
                    'sha256': {str(p.relative_to(bundle)): digest(p) for p in files.values()}}
        (bundle/f'manifest-linux-{arch}.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(f'Updated {bundle}: binaries, header and license only; temporary sources removed')


if __name__ == '__main__':
    main()
