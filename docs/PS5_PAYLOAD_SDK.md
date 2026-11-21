# PS5 Payload SDK dependency

ReLink's PS5 target uses the PS5-specific payload SDK fork:

- Repository: https://github.com/mihawk-99/PS5_PayloadSDK
- Pinned revision: `b83202be73e930050e00de0f1b4d9ec46c0391bf`
- Pin file: `deps/ps5-payload-sdk.commit`
- Upstream payload SDK binary base at this revision: v0.42
- SDK setup entry point at the pinned revision: `platform/tools/setup-sdk.sh`

## Why this revision

The pin deliberately follows the revision currently used by `mihawk-99/PS5_Vulkan/tools/setup-native-dependencies.sh`, rather than silently tracking the SDK repository's moving `main` branch. This gives ReLink a known reference point for the Prospero compiler wrappers, target sysroot, libc/libc++, link stubs and `libps5platform.a` platform layer that the RADV link recipe expects.

The SDK repository's current `main` has advanced beyond this pin (its README describes the v0.45 base). Do not update this pin to `main` or to a newer SDK revision without also checking the matching PS5_Vulkan RADV link recipe, platform symbols and runtime libraries as a set.

## What the SDK supplies

- x86-64 Prospero compiler, linker and binutils wrappers.
- PS5 target sysroot and the console's CRT, libc/libc++, unwind libraries and import stubs.
- `libps5platform.a` and `ps5platform/` headers for direct-memory heap allocation, shared-memory mappings, thread support, missing/refused libc functions, VideoOut/AGC declarations and platform context details.
- The PS5 CMake and Make toolchain definitions.

The SDK does **not** by itself provide the RADV driver archive or the RADV link recipe. Those come from the matching `mihawk-99/PS5_Vulkan` and `mihawk-99/PS5_Mesa` revisions. In particular, do not link RADV with a guessed `-lvulkan` flag: use `PS5_Vulkan/tools/radv-link.sh` from the same dependency set.

## Integration status

This pin records the selected SDK dependency; it does not yet make ReLink a PS5 title. ReLink still needs:
1. A PS5 build target that invokes the Prospero toolchain and builds the renderer in-process.
2. The RADV archive and exact matching link recipe, including platform heap/thread/libc wrappers and AGC stubs.
3. A PS5 VideoOut WSI path instead of the existing SDL/Win32 surface creation.
4. PS5 host implementations for lifecycle, input, audio, filesystem, memory and synchronization.
5. Packaging through the PS5 native title toolchain.

No PS5 compile or console run is claimed by this dependency pin.
