#!/usr/bin/env bash
# Build the complete transceiver, Pluto driver, monitoring decoders and audio adapter.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    echo "Usage: ./build_all.sh [--install-deps]"
    echo "Installs the required Debian/Ubuntu packages with apt-get, then builds the transceiver."
    echo "  --install-deps  Accepted for compatibility. Installation is enabled by default."
    echo "Optional: CMAKE_BUILD_PARALLEL_LEVEL=2 ./build_all.sh"
    echo "Downloads and builds libft8 when no local checkout or installed package is selected."
    echo "Set LIBFT8_ROOT to an installed libft8 package or LIBFT8_SOURCE_DIR to its checkout."
}
while (($#)); do
    case "$1" in
        --install-deps) ;;
        --help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done

# Keep the default memory requirement suitable for a small VM or SBC.
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-$(getconf _NPROCESSORS_ONLN)}"
if [[ ! $jobs =~ ^[1-9][0-9]*$ ]]; then
    echo "CMAKE_BUILD_PARALLEL_LEVEL must be a positive integer." >&2
    exit 2
fi
if [[ -z ${CMAKE_BUILD_PARALLEL_LEVEL:-} ]] && ((jobs > 4)); then jobs=4; fi

if [[ $(uname -s) != Linux ]]; then
    echo "This project requires Linux." >&2
    exit 1
fi
case "$(uname -m)" in
    x86_64|amd64) arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) echo "Use a 64-bit x86 or ARM Linux OS (including on Raspberry Pi/Odroid)." >&2; exit 1 ;;
esac

if ! command -v apt-get >/dev/null; then
    echo "This build script requires a Debian/Ubuntu-based system with apt-get." >&2
    exit 1
fi
apt_command=(apt-get)
if ((EUID != 0)); then
    if ! command -v sudo >/dev/null; then
        echo "Install sudo or ask your administrator to install the build packages." >&2
        exit 1
    fi
    apt_command=(sudo apt-get)
fi
packages=(build-essential cmake pkg-config gfortran git ca-certificates
    python3 python3-numpy python3-scipy
    libiio-dev libad9361-dev libiio-utils libsamplerate0-dev libfftw3-dev
    libboost-dev nlohmann-json3-dev libpulse-dev pulseaudio-utils libcurl4-openssl-dev
    libasound2-plugins dbus-user-session)
# The client utilities do not install a sound server on minimal Debian.
# Preserve an existing PulseAudio/PipeWire setup; add PulseAudio only if neither exists.
package_installed() {
    [[ $(dpkg-query -W -f='${Status}' "$1" 2>/dev/null) == 'install ok installed' ]]
}
if ! package_installed pulseaudio && ! package_installed pipewire-pulse && \
   ! command -v pulseaudio >/dev/null && ! command -v pipewire-pulse >/dev/null; then
    packages+=(pulseaudio)
fi
"${apt_command[@]}" update
"${apt_command[@]}" install -y --no-install-recommends "${packages[@]}"

for program in cmake python3 pkg-config cc c++ gfortran make git; do
    if ! command -v "$program" >/dev/null; then
        echo "Missing $program. Check the package installation above." >&2
        exit 1
    fi
done
python_bin=/usr/bin/python3
if [[ ! -x "$python_bin" ]] || ! "$python_bin" -c 'import numpy, scipy, sqlite3, zlib'; then
    echo "Missing system Python modules. Check the package installation above." >&2
    exit 1
fi

# Bundles for x86-64 ship with the project. Build missing native bundles on
# ARM64 (or after an incomplete copy); the helpers remove temporary sources.
liquid_bundle="$project_dir/external/liquid-dsp-1.8.3"
speex_bundle="$project_dir/external/speexdsp-1.2.1"
if [[ ! -f "$liquid_bundle/lib/linux-$arch/libliquid.a" || ! -f "$liquid_bundle/include/liquid/liquid.h" ]]; then
    echo "Preparing liquid-dsp for $arch (first build requires internet access) ..."
    "$python_bin" "$project_dir/tools/build_liquid.py" --jobs "$jobs"
fi
if [[ ! -f "$speex_bundle/lib/linux-$arch/libspeexdsp.a" || ! -f "$speex_bundle/include/speex/speex_preprocess.h" ]]; then
    echo "Preparing SpeexDSP for $arch (first build requires internet access) ..."
    "$python_bin" "$project_dir/tools/build_speex.py" --jobs "$jobs"
fi

download_libft8() (
    local destination=$1 revision=$2 temporary_source
    mkdir -p -- "$(dirname -- "$destination")"
    temporary_source=$(mktemp -d -- "$destination.XXXXXX")
    trap 'rm -rf -- "$temporary_source"' EXIT
    git init --quiet "$temporary_source"
    GIT_TERMINAL_PROMPT=0 git -C "$temporary_source" fetch --quiet --depth=1 --no-tags \
        https://github.com/dj0abr/libft8.git "$revision"
    git -C "$temporary_source" checkout --quiet --detach "$revision"
    mv -Tn -- "$temporary_source" "$destination"
)

libft8_prefix="${LIBFT8_ROOT:-}"
if [[ -z $libft8_prefix ]]; then
    libft8_source="${LIBFT8_SOURCE_DIR:-$project_dir/../../ft8/libft8}"
    libft8_dir="$project_dir/build/libft8"
    if [[ -z ${LIBFT8_SOURCE_DIR:-} && ! -f "$libft8_source/CMakeLists.txt" ]]; then
        # libft8 0.3.1, tested with the QO100 FT4/FT8 integration.
        libft8_revision=e281d1a411f0980a914c9e0b9fd097f4c0d17500
        libft8_source="$libft8_dir/source-$libft8_revision"
        if [[ ! -d $libft8_source ]]; then
            echo "Downloading libft8 from GitHub ($libft8_revision) ..."
            download_libft8 "$libft8_source" "$libft8_revision"
        fi
    fi
    if [[ ! -f "$libft8_source/CMakeLists.txt" ]]; then
        echo "No CMakeLists.txt in libft8 source directory: $libft8_source" >&2
        exit 1
    fi

    # Each source directory needs its own CMake cache and installed library.
    libft8_source=$(cd -- "$libft8_source" && pwd -P)
    libft8_source_id=$(printf '%s' "$libft8_source" | git hash-object --stdin)
    libft8_build="$libft8_dir/$libft8_source_id/core"
    libft8_prefix="$libft8_dir/$libft8_source_id/install"
    cmake -S "$libft8_source" -B "$libft8_build" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
    cmake --build "$libft8_build" --parallel "$jobs"
    cmake --install "$libft8_build" --prefix "$libft8_prefix"
fi

cmake -S "$project_dir" -B "$project_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DQO100_BUILD_WEBBRIDGE=ON \
    -DPython3_EXECUTABLE="$python_bin" -U libft8_DIR -Dlibft8_ROOT="$libft8_prefix"
cmake --build "$project_dir/build" --parallel "$jobs" \
    --target qo100_backend qo100_webbridge qo100_ft8_decoder qo100_beacon_decoder qo100_mm_decoder qo100_digital_audio

echo "Built backend, Pluto driver, WebSocket adapter, RX3 FT8, RX4 PSK, RX5 MM beacon and RX2/TX2 virtual audio interface."
echo "Start with: $project_dir/start_all.sh"
