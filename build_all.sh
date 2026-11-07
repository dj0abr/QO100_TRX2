#!/usr/bin/env bash
# Build the complete browser-based transceiver, including both child programs.
set -euo pipefail
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

usage() {
    echo "Usage: ./build_all.sh [--install-deps]"
    echo "  --install-deps  Install required Debian/Ubuntu packages using apt-get."
    echo "Optional: CMAKE_BUILD_PARALLEL_LEVEL=2 ./build_all.sh"
}
install_deps=0
while (($#)); do
    case "$1" in
        --install-deps) install_deps=1 ;;
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

if ((install_deps)); then
    if ! command -v apt-get >/dev/null; then
        echo "--install-deps requires a Debian/Ubuntu-based system with apt-get." >&2
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
    packages=(build-essential cmake pkg-config gfortran python3 python3-numpy python3-scipy ca-certificates
        libiio-dev libad9361-dev libsamplerate0-dev libfftw3-dev
        libboost-dev nlohmann-json3-dev libpulse-dev pulseaudio-utils)
    "${apt_command[@]}" update
    "${apt_command[@]}" install -y --no-install-recommends "${packages[@]}"
fi

for program in cmake python3 pkg-config cc c++ gfortran make; do
    if ! command -v "$program" >/dev/null; then
        echo "Missing $program. Run ./build_all.sh --install-deps first." >&2
        exit 1
    fi
done

# Bundles for x86-64 ship with the project. Build missing native bundles on
# ARM64 (or after an incomplete copy); the helpers remove temporary sources.
liquid_bundle="$project_dir/external/liquid-dsp-1.8.3"
speex_bundle="$project_dir/external/speexdsp-1.2.1"
if [[ ! -f "$liquid_bundle/lib/linux-$arch/libliquid.a" || ! -f "$liquid_bundle/include/liquid/liquid.h" ]]; then
    echo "Preparing liquid-dsp for $arch (first build requires internet access) ..."
    python3 "$project_dir/tools/build_liquid.py" --jobs "$jobs"
fi
if [[ ! -f "$speex_bundle/lib/linux-$arch/libspeexdsp.a" || ! -f "$speex_bundle/include/speex/speex_preprocess.h" ]]; then
    echo "Preparing SpeexDSP for $arch (first build requires internet access) ..."
    python3 "$project_dir/tools/build_speex.py" --jobs "$jobs"
fi

cmake -S "$project_dir" -B "$project_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DQO100_BUILD_WEBBRIDGE=ON \
    -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build "$project_dir/build" --parallel "$jobs" \
    --target qo100_backend qo100_webbridge qo100_ft8_decoder qo100_beacon_decoder qo100_digital_audio

echo "Built backend, Pluto driver, WebSocket adapter, RX3 FT8, RX4 PSK and RX2/TX2 virtual audio interface."
echo "Start with: $project_dir/start_all.sh"
