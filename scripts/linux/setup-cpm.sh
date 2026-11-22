#!/usr/bin/env bash
# Makes the header packages the renderer builds against, in the layout nx2/build.py reads (RX_CPM):
#
#   $RELINK_HOME/cpm/vulkanheaders/<version>/include            Vulkan-Headers (fetched from Khronos' GitHub, pinned)
#   $RELINK_HOME/cpm/spirv-headers/<version>/include            Ubuntu's spirv-headers (apt)
#   $RELINK_HOME/cpm/vulkanmemoryallocator/<version>/include    Ubuntu's libvulkan-memory-allocator-dev (apt)
#
# Safe to run again: it rebuilds the folder. Needs git and the apt packages install-deps.sh installs.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

say() { printf 'setup-cpm: %s\n' "$*"; }
die() { printf 'setup-cpm: %s\n' "$*" >&2; exit 1; }

[ -d /usr/include/spirv/unified1 ] || die "spirv-headers is not installed (sudo apt install spirv-headers)"
[ -f /usr/include/vk_mem_alloc.h ] || die "libvulkan-memory-allocator-dev is not installed (sudo apt install libvulkan-memory-allocator-dev)"

vk_version() { sed -n 's/^#define VK_HEADER_VERSION \([0-9]*\)$/\1/p' "$1/vulkan/vulkan_core.h" | head -1; }
want="${RELINK_VULKAN_HEADERS_TAG##*.}"   # (v1.4.309 -> 309)

src="$RELINK_HOME/src/Vulkan-Headers"
mkdir -p "$RELINK_HOME/src"
vkinc=""
if [ -d "$src/.git" ] && [ "$(git -C "$src" describe --tags --exact-match 2>/dev/null || true)" = "$RELINK_VULKAN_HEADERS_TAG" ]; then
    vkinc="$src/include"
elif git clone -q --depth 1 --branch "$RELINK_VULKAN_HEADERS_TAG" https://github.com/KhronosGroup/Vulkan-Headers.git "$src.new" 2>/dev/null; then
    rm -rf "$src"
    mv "$src.new" "$src"
    vkinc="$src/include"
else
    rm -rf "$src.new"
    # (offline: the system's headers, when they are as new as the pinned ones)
    have="$(vk_version /usr/include || true)"
    if [ -n "$have" ] && [ "$have" -ge "$want" ]; then
        say "could not fetch Vulkan-Headers $RELINK_VULKAN_HEADERS_TAG; using the system's (header version $have)"
        vkinc=/usr/include
    else
        die "could not fetch Vulkan-Headers $RELINK_VULKAN_HEADERS_TAG from github.com, and the system's are too old (${have:-none}; need $want or newer)"
    fi
fi

rm -rf "$RELINK_CPM"
vkver="1.4.$(vk_version "$vkinc")"
mkdir -p "$RELINK_CPM/vulkanheaders/$vkver/include" \
         "$RELINK_CPM/spirv-headers/system/include" \
         "$RELINK_CPM/vulkanmemoryallocator/system/include"
cp -r "$vkinc/vulkan" "$RELINK_CPM/vulkanheaders/$vkver/include/"
[ -d "$vkinc/vk_video" ] && cp -r "$vkinc/vk_video" "$RELINK_CPM/vulkanheaders/$vkver/include/"
ln -s /usr/include/spirv "$RELINK_CPM/spirv-headers/system/include/spirv"
ln -s /usr/include/vk_mem_alloc.h "$RELINK_CPM/vulkanmemoryallocator/system/include/vk_mem_alloc.h"

say "ready: $RELINK_CPM (Vulkan headers $vkver)"
