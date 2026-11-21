#!/usr/bin/env bash
# Build a PS5 title folder (PPSA00000), using the native title pipeline.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
vulkan_root=${PS5_VULKAN_ROOT:-$root/../PS5_Vulkan}
vulkan_revision=7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc
sdk_revision=b83202be73e930050e00de0f1b4d9ec46c0391bf
title_id=PPSA00000
work="$root/build/ps5-guest-probe"
app="$root/dist/$title_id"
param="$root/tools/ps5/param.json"
sdk="$vulkan_root/.deps/native/ps5-payload-sdk"
native="$vulkan_root/tooling/native"
wrapper="$vulkan_root/tooling/prospero-clang18"
tool="$vulkan_root/build/runtime-shim/ps5-native-tool"
libc="$vulkan_root/runtime/libc.prx"

[[ -d "$vulkan_root/.git" ]] || { echo "PS5_VULKAN_ROOT must point to a PS5_Vulkan checkout" >&2; exit 2; }
actual_vulkan_revision=$(git -C "$vulkan_root" rev-parse HEAD)
[[ "$actual_vulkan_revision" == "$vulkan_revision" ]] || {
  echo "PS5_Vulkan revision mismatch: expected $vulkan_revision, found $actual_vulkan_revision" >&2
  exit 2
}
[[ -x "$sdk/bin/prospero-clang" && -x "$sdk/bin/prospero-lld" ]] || {
  echo "Pinned PS5 SDK is missing; prepare the PS5_Vulkan native dependencies first" >&2; exit 2;
}
[[ -f "$sdk/.ps5-sdk-revision" && "$(tr -d '\r\n' < "$sdk/.ps5-sdk-revision")" == "$sdk_revision" ]] || {
  echo "PS5 SDK revision mismatch; expected $sdk_revision" >&2; exit 2;
}
for file in "$wrapper" "$native/app_crt.cpp" "$native/ps5-pie.ld" "$native/app-symbols.map"; do
  [[ -f "$file" ]] || { echo "missing native title input: $file" >&2; exit 2; }
done

# The reference project's reproducible host utility also produces the runtime
# companion module. Rebuild only when either required artifact is absent.
if [[ ! -x "$tool" || ! -f "$libc" ]]; then
  bash "$vulkan_root/tools/rebuild-libc.sh"
fi
[[ -x "$tool" && -f "$libc" ]] || { echo "native title tool or libc.prx was not produced" >&2; exit 2; }

mkdir -p "$work/obj"
rm -f "$work/obj/"*.o "$work/llvm-pie.elf" "$work/eboot.elf"

compile() {
  PS5_PAYLOAD_SDK="$sdk" sh "$wrapper" "$@"
}
compile -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
  -c "$root/tools/ps5/guest_window_probe_main.c" -o "$work/obj/main.o"
compile -std=c11 -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
  -c "$root/tools/ps5/guest_window_probe.c" -o "$work/obj/guest_window_probe.o"
compile -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
  -ffunction-sections -fdata-sections -c "$native/app_crt.cpp" -o "$work/obj/app_crt.o"

"$sdk/bin/prospero-lld" -T "$native/ps5-pie.ld" -L "$native" --eh-frame-hdr \
  --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$work/llvm-pie.elf" \
  "$work/obj/app_crt.o" "$work/obj/main.o" "$work/obj/guest_window_probe.o" \
  --as-needed "$sdk"/target/lib/*.so

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
  --stub-dir "$sdk/target/lib" --module-sdk 0x02000009 \
  --companion-sdk 0x08050001 --file-name eboot.elf

rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
"$tool" self --sign --in "$work/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$param" "$app/sce_sys/param.json"
cp "$libc" "$app/sce_module/libc.prx"
"$tool" self --inspect --file "$app/eboot.bin" >/dev/null
"$tool" self --inspect --file "$app/sce_module/libc.prx" >/dev/null
printf 'PS5 title folder created: %s\n' "$app"
printf 'Title ID: %s\n' "$title_id"
printf 'Executable: %s/eboot.bin (%s bytes)\n' "$app" "$(stat -c %s "$app/eboot.bin")"
