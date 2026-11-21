#!/usr/bin/env bash
# Build a PS5 title folder (PPSA00000), using the native title pipeline.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
vulkan_root=${PS5_VULKAN_ROOT:-$root/../PS5_Vulkan}
vulkan_revision=$(awk '!/^#/ && NF { print; exit }' "$root/deps/ps5-vulkan-toolchain.commit")
sdk_revision=$(awk '!/^#/ && NF { print; exit }' "$root/deps/ps5-payload-sdk.commit")
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
[[ -n "$vulkan_revision" && -n "$sdk_revision" ]] || { echo "missing pinned dependency revision" >&2; exit 2; }
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
linker_script="$vulkan_root/tooling/psbc/ps5-pie-unwind.ld"
for file in "$wrapper" "$native/app_crt.cpp" "$linker_script" "$native/app-symbols.map" \
           "$sdk/target/lib/libps5platform.a" "$sdk/target/lib/libc++.a" \
           "$sdk/target/lib/libc++abi.a" "$sdk/target/lib/libunwind.a"; do
  [[ -f "$file" ]] || { echo "missing native title input: $file" >&2; exit 2; }
done

# Rebuild the native converter/runtime when absent or when the signed runtime
# does not match the reference repository's checksum manifest.
runtime_valid=0
if [[ -x "$tool" && -f "$libc" && -f "$vulkan_root/runtime/libc.prx.sha256" ]] &&
   (cd "$vulkan_root/runtime" && sha256sum --check --strict --quiet libc.prx.sha256); then
  runtime_valid=1
fi
if [[ "$runtime_valid" -ne 1 ]]; then
  bash "$vulkan_root/tools/rebuild-libc.sh"
fi
[[ -x "$tool" && -f "$libc" ]] || { echo "native title tool or libc.prx was not produced" >&2; exit 2; }

python3 - "$param" "$title_id" <<'PY'
import json, re, sys
with open(sys.argv[1], encoding="utf-8") as source:
    p = json.load(source)
title = sys.argv[2]
if p.get("titleId") != title or not re.fullmatch(r"PPSA\d{5}", title):
    raise SystemExit("param.json titleId does not match the expected PPSA title folder")
if not re.fullmatch(r"\d{5}", p.get("conceptId", "")):
    raise SystemExit("param.json conceptId must contain five digits")
if not re.fullmatch(r"[A-Z]{2}\d{4}-" + re.escape(title) + r"_00-[A-Z0-9]{16}", p.get("contentId", "")):
    raise SystemExit("param.json contentId is malformed or does not contain the title ID")
PY

shopt -s nullglob
sdk_stubs=("$sdk"/target/lib/*.so)
(( ${#sdk_stubs[@]} > 0 )) || { echo "no PS5 SDK import stubs found" >&2; exit 2; }

compiler=${PS5_CLANG:-$(command -v clang || command -v clang-18 || true)}
[[ -n "$compiler" ]] || { echo "clang was not found; set PS5_CLANG" >&2; exit 2; }
builtins="$("$compiler" --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
[[ -f "$builtins" ]] || { echo "missing Clang x86_64 builtins: $builtins" >&2; exit 2; }

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

"$sdk/bin/prospero-lld" -T "$linker_script" -L "$native" --eh-frame-hdr \
  --version-script "$native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$work/llvm-pie.elf" \
  "$work/obj/app_crt.o" "$work/obj/main.o" "$work/obj/guest_window_probe.o" \
  --start-group "$sdk/target/lib/libc++.a" "$sdk/target/lib/libc++abi.a" \
  "$sdk/target/lib/libunwind.a" "$builtins" "$sdk/target/lib/libps5platform.a" --end-group \
  --as-needed "${sdk_stubs[@]}"

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
