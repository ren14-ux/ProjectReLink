#!/usr/bin/env bash
# Build the diagnostic as a PS5 title directory (PPSA00000), not an ELF deliverable.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=${PS5_PAYLOAD_SDK:-}
native_tool=${PS5_NATIVE_TOOL:-}
libc_prx=${PS5_LIBC_PRX:-}
revision=b83202be73e930050e00de0f1b4d9ec46c0391bf
title_id=PPSA00000
work="$root/build/ps5-guest-probe"
app="$root/dist/$title_id"
param="$root/tools/ps5/param.json"

[[ -n "$sdk" && -x "$sdk/bin/prospero-clang" ]] || { echo "PS5_PAYLOAD_SDK must point to the pinned SDK" >&2; exit 2; }
[[ -f "$sdk/.ps5-sdk-revision" && "$(tr -d '\r\n' < "$sdk/.ps5-sdk-revision")" == "$revision" ]] || {
  echo "PS5 SDK revision mismatch; expected $revision" >&2; exit 2;
}
[[ -n "$native_tool" && -x "$native_tool" ]] || {
  echo "PS5_NATIVE_TOOL must point to the PS5_Vulkan ps5-native-tool executable" >&2; exit 2;
}
[[ -n "$libc_prx" && -f "$libc_prx" ]] || {
  echo "PS5_LIBC_PRX must point to a compatible libc.prx runtime module" >&2; exit 2;
}
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

mkdir -p "$work"
rm -f "$work/guest-window-probe.elf" "$work/eboot.elf"
"$sdk/bin/prospero-clang" -O2 -Wall -Wextra -Werror -std=c11 -fPIE -pie \
  -I"$sdk/target/include" \
  -o "$work/guest-window-probe.elf" \
  "$root/tools/ps5/guest_window_probe_main.c" \
  "$root/tools/ps5/guest_window_probe.c"

"$native_tool" link --in "$work/guest-window-probe.elf" --out "$work/eboot.elf" \
  --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 \
  --companion-sdk 0x08050001 --file-name eboot.elf

rm -rf "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$native_tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$param" "$app/sce_sys/param.json"
cp "$libc_prx" "$app/sce_module/libc.prx"
"$native_tool" self --inspect --file "$app/eboot.bin" >/dev/null
"$native_tool" self --inspect --file "$app/sce_module/libc.prx" >/dev/null
printf 'PS5 title folder created: %s\n' "$app"
printf 'Contents: eboot.bin, sce_sys/param.json, sce_module/libc.prx\n'
