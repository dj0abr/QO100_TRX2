# liquid-dsp 1.8.3 binary bundle

Upstream: [liquid-dsp v1.8.3](https://github.com/jgaeddert/liquid-dsp/releases/tag/v1.8.3),
commit `10041f70cebbe3b97887e75bb41e48b73dda1b23`, MIT license (see `LICENSE`).
The release has no prebuilt assets. These binaries were built from the unmodified
release in a temporary directory. No liquid implementation source is included.

Included:

- `include/liquid/liquid.h`: matching public development header.
- `lib/linux-x86_64/libliquid.a`: static library, used by both backend and driver.
- `lib/linux-x86_64/libliquid.so.1.8.3` and symlinks: shared library for optional use.
- `manifest-linux-x86_64.json`: source URL, commit, source hash, build options,
  compiler and SHA-256 checksums of header, license and both binaries.

The ordinary project build reads the local archive directly: no library download,
liquid compilation, global installation, shared-library search path, or system
liquid-dsp package is required. The backend prints its linked version at startup.
The copied Pluto snapshot retains its old `liquid.h` for provenance; all compiled
code includes this bundle's header instead.

## Moving to another computer

Copy the entire project, including this directory. On another **Linux x86-64**
PC, configure a fresh `build/` directory and build normally. The archive contains
baseline x86-64 code plus SIMD kernels selected at runtime; it does not use
`-march=native`. The optional shared library requires GNU libc >= 2.29 and libm.
Other project dependencies (libiio, libad9361, FFTW, libsamplerate, GUI packages)
still need to be installed. Rebuild the project executables on the new computer
rather than copying the CMake cache from the old absolute path.

Binaries are specific to operating system and CPU architecture. For Linux aarch64,
build a matching bundle on that target with the helper below; CMake selects the
`lib/linux-aarch64/` archive once present. Windows/macOS need separate builds and
project/audio/hardware integration. Only Linux x86-64 is included and tested here.

## Optional binary rebuild

Requires a C/C++ compiler, CMake, Python >= 3.12 and network access:

```sh
python3 tools/build_liquid.py
```

Run this from the project root. The helper pins the release archive's SHA-256,
uses generic architecture flags, builds static and shared libraries without FFTW
or logging dependencies, and copies only binary files, header and license into
this directory. It writes a per-platform manifest and removes temporary sources.
A normal `make` never invokes this helper.
