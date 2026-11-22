# Shared settings for the Linux scripts (sourced, not run). Project ReLink on Ubuntu / Debian.
#
#   RELINK_HOME  where the Linux tooling keeps its files: the Python environment and the header packages
#                (default ~/.local/share/relink). Nothing of the game is kept here.

RELINK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
RELINK_HOME="${RELINK_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/relink}"
RELINK_VENV="$RELINK_HOME/venv"
RELINK_CPM="$RELINK_HOME/cpm"

# The header versions the renderer is built and tested against (nx2/build.py reads them from the "CPM" layout:
# <name>/<version>/include). Vulkan-Headers needs 1.4.317 or newer: Ubuntu 24.04's own libvulkan-dev (1.3.275) is too old for
# VK_EXT_shader_float8 and VK_KHR_shader_maximal_reconvergence, which gpu/vk uses.
RELINK_VULKAN_HEADERS_TAG="v1.4.317"

# The Python modules nx2/ imports (README: "Build from source"). Pillow: the icon and glyph tools.
RELINK_PIP_PACKAGES=(lz4 numpy capstone "unicorn>=2.1,<2.2" zstandard pillow)

relink_python() {
    if [ -x "$RELINK_VENV/bin/python" ]; then
        printf '%s\n' "$RELINK_VENV/bin/python"
    else
        echo "relink: the Python environment is not set up yet: run scripts/linux/install-deps.sh first" >&2
        return 1
    fi
}

# What the build reads: the header packages, and nothing else from this script's settings.
export RX_CPM="$RELINK_CPM"
# This is the Linux build: ELF game, relink_vk.so, SDL2 (never RX_TARGET=windows, which cross-builds the Windows game).
unset RX_TARGET
