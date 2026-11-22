#!/usr/bin/env bash
# Installs everything Project ReLink needs to be built on Ubuntu (24.04 is what it was made and tested on) or Debian:
# the compiler and libraries from apt, a Python environment for the build's modules, and the Vulkan header packages.
# Nothing of the game is involved.
#
#   scripts/linux/install-deps.sh             everything (asks for sudo for the apt part)
#   scripts/linux/install-deps.sh --no-apt    only the Python environment and the header packages
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

say() { printf 'install-deps: %s\n' "$*"; }
die() { printf 'install-deps: %s\n' "$*" >&2; exit 1; }

use_apt=1
for arg in "$@"; do
    case "$arg" in
        --no-apt) use_apt=0 ;;
        -h|--help) sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option: $arg" ;;
    esac
done

[ "$(uname -m)" = x86_64 ] || die "ReLink needs an x86-64 PC (it is built for the CPU it runs on, with AVX2); this is $(uname -m)"

if [ "$use_apt" = 1 ]; then
    command -v apt-get >/dev/null || die "apt-get not found: this script is for Ubuntu and Debian. On another distribution install the same things (see docs/linux.md) and run it with --no-apt"
    if [ -r /etc/os-release ]; then
        . /etc/os-release
        if [ "${ID:-}" = ubuntu ] && [ "${VERSION_ID%%.*}" -lt 24 ] 2>/dev/null; then
            say "warning: Ubuntu $VERSION_ID is older than 24.04; its clang and Vulkan headers may be too old (LLVM 18 or newer is wanted)"
        fi
    fi
    sudo=""
    [ "$(id -u)" = 0 ] || { command -v sudo >/dev/null || die "not root and no sudo: run as root, or install the packages by hand (docs/linux.md)"; sudo=sudo; }
    packages=(
        # the compiler: clang and lld build the lifted game; build-essential brings libc and libstdc++'s headers
        clang lld llvm build-essential pkg-config cmake ninja-build git curl ca-certificates
        # the window, the controllers, the sound (SDL2), and ALSA behind it
        libsdl2-dev libasound2-dev libx11-dev libgl-dev
        # Vulkan: the loader and tools, and a layer for debugging; the drivers (Mesa, NVIDIA) are the system's own
        libvulkan-dev vulkan-tools mesa-vulkan-drivers spirv-headers spirv-tools libvulkan-memory-allocator-dev
        # FFmpeg and dav1d: the game's videos (nn::codec, AV1 for the movies)
        libavcodec-dev libavutil-dev libswresample-dev libswscale-dev libdav1d-dev
        # the Insert menu draws its text with a system font (host/posix/gdi_posix.c)
        fontconfig fonts-dejavu-core
        python3 python3-venv python3-pip
    )
    say "installing packages with apt"
    $sudo apt-get update
    DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y --no-install-recommends "${packages[@]}"
fi

for tool in clang clang++ ld.lld git python3; do
    command -v "$tool" >/dev/null || die "$tool is missing"
done
say "compiler: $(clang --version | head -1)"

if [ ! -x "$RELINK_VENV/bin/python" ]; then
    say "making the Python environment in $RELINK_VENV"
    mkdir -p "$RELINK_HOME"
    python3 -m venv "$RELINK_VENV"
fi
say "installing the build's Python modules"
"$RELINK_VENV/bin/python" -m pip install --quiet --upgrade pip
"$RELINK_VENV/bin/python" -m pip install --quiet "${RELINK_PIP_PACKAGES[@]}"
"$RELINK_VENV/bin/python" -I -c "import lz4, numpy, capstone, unicorn, zstandard, PIL" || die "a Python module did not install"

"$(dirname "${BASH_SOURCE[0]}")/setup-cpm.sh"

say "done. Next: scripts/linux/build-game.sh --nup <your game file.nup>   (docs/linux.md)"
