# SpeexDSP 1.2.1 binary bundle

Upstream: [Xiph SpeexDSP 1.2.1](https://www.speex.org/downloads/), BSD license
(see `LICENSE`). The unmodified official release was built in a temporary
directory. This bundle contains only the static library, matching public
preprocessor/type headers, license, and checksum manifest; no implementation
source or downloaded source archive is kept in the project.

- `lib/linux-x86_64/libspeexdsp.a`: Linux x86-64 archive used by the backend.
- `include/speex/`: the public preprocessor interface and matching integer types.
- `manifest-linux-x86_64.json`: source URL/hash, compiler, flags, file hashes.

The build uses floating point and the built-in smallft FFT. There is no FFTW
dependency inside this archive. Generic `-march=x86-64` code is used, without
`-march=native`, AVX, or CPU-specific runtime requirements. A normal project
build uses the local archive directly and needs no installed SpeexDSP package
or network connection. Pluto does not use SpeexDSP.

Copy this entire directory when moving to another Linux x86-64 computer and
build the backend again in a fresh build directory. Other CPU architectures
need a matching archive. Rebuild on a Linux x86-64/aarch64 target with:

```sh
python3 tools/build_speex.py
```

The optional helper requires Python >= 3.12, a C compiler, make, and network
access. It verifies the pinned official source archive, builds it outside the
project, copies only library/header/license files and updates the manifest.
The ordinary CMake build never invokes it.
