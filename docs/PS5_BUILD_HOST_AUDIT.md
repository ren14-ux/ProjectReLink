# PS5 build and host-platform audit

Status: source audit of the current ReLink branch. This document deliberately does not add an `RX_TARGET=ps5` switch: the current build and host code do not yet have a valid PS5 target contract. No SDK compile or console validation is claimed.

## Executive finding

A PS5 target cannot safely be introduced by changing only `toolchain.native_target()` or setting `RX_TARGET=ps5`. The current build has a binary Windows-versus-Linux model, and the host executable has a Windows-versus-POSIX startup model. PS5 is neither of those configurations.

The next implementation unit must be a dedicated PS5 platform layer plus a PS5 build/link driver. Until those exist, preserving the current Windows/Linux paths is safer than making PS5 appear to build while it silently inherits Linux or Windows assumptions.

## Build-system facts

### Target classification

- `nx2/build.py` sets `LINUX = not toolchain.windows()` at module import.
- `toolchain.windows()` returns true only on Windows or when `RX_TARGET=windows`; every other `RX_TARGET` value is currently treated as non-Windows.
- `EXE` and `VK_LIB` are selected from that single boolean: Linux gets `relink` and `relink_vk.so`; otherwise Windows gets `relink-game.exe` and `relink_vk.dll`.
- `target()`, `HOT_ORDER()`, `HOST_LIBS`, host source selection, Vulkan compile flags, renderer linking and final executable linking all branch on `LINUX`, not on a target enum.
- The Linux standalone link emits an ELF executable and separately links a shared Vulkan renderer with `-shared`, `-ldl`, `-lpthread` and `-lm`.
- The Windows standalone link emits a PE executable and later links a separate DLL. Neither path is a PS5 title link.
- The renderer compilation block is currently written for a separate DLL/SO. The existing static-loader code in `host/rx_backend.c` is only a guarded source seam; the build does not yet include the renderer objects in the game executable or link RADV.

### Toolchain facts

`third_party/a64-kit/tools/toolchain.py` currently exposes:
- `windows()`: Windows/MSVC or llvm-mingw selection.
- `native_target()`: x86-64 Windows target on Windows/cross-Windows, otherwise native host defaults.
- `native_link()`: Windows linker/runtime behavior; no PS5 linker recipe.
- `find()`: emsdk clang/LLVM or PATH discovery, with llvm-mingw special handling.

There is no PS5 compiler target, SDK root discovery, SDK wrapper selection, sysroot, linker script, PS5 import-library handling, package/signing tool discovery or PS5 runtime-library configuration. The exact PS5 SDK and RADV pins are recorded in `deps/ps5-payload-sdk.commit`, `deps/ps5-mesa.commit` and `docs/PS5_PAYLOAD_SDK.md`; pins alone do not supply the installed SDK or built RADV archive.

## Host-platform facts

### Startup and threading

- `host/hos_main.c` contains the common game boot sequence in `game_main()`, but that function is currently declared `static`.
- The Windows entry point calls the game directly after Windows-specific path, environment and process setup.
- The non-Windows entry point is guarded by `#ifndef _WIN32`; it calls `hos_display_init()`, starts a 64 MiB game thread, and runs `hos_display_loop()` on the process main thread.
- That non-Windows path is specifically the desktop SDL architecture, not a generic POSIX or console entry point.
- A PS5 entry point therefore needs an explicit, reviewed startup contract. It cannot simply reuse the Linux `main()` because that path starts SDL and the desktop event loop; nor should it define `_WIN32` merely to reach the Windows entry point.

### Display and input

- `host/posix/display_sdl.c` owns SDL video initialization, window creation, event polling, resize/focus handling, mouse state and controller pumping.
- It starts the Vulkan renderer through an SDL `MkwVkWindow` bridge, which the new PS5 Vulkan display branch intentionally bypasses.
- `host/hos_sdl.c` dynamically loads SDL2/SDL3 and uses Windows-style module/procedure APIs (`LoadLibraryA`, `GetProcAddress`, `HMODULE`).
- `host/pad_sdl.c` depends on that SDL controller abstraction for gamepad buttons, sticks, rumble and motion sensors.
- `host/display.c` is the Windows display implementation and depends on Windows UI/window APIs. Neither display implementation is a PS5-native implementation.
- A first PS5 host milestone needs controller input and display lifecycle; mouse/window resize/focus behavior can be omitted only after checking which ReLink menus and runtime paths require those services.

### Other host services

The standalone host source set is not just a renderer wrapper. It includes memory mapping/reservation, guest image loading, thread and synchronization services, filesystem/ROMFS/user paths, audio output/input, video decode, NVN/HOS services, profiling, mod loading and relaunch/update behavior.

Relevant source groups include:
- Memory and guest image: `hos_mem.c`, `hos_main.c`, `hos_paths.h`, generated `hos_image.c`.
- Threading and synchronization: `hos_thread.c`, `nn_os_sync.c`, `nn_syncpt.c`, `hos_setjmp.c`.
- Files and modules: `nn_fs.c`, `mods.c`, `mods_exefs.c`, `nn_libc.c`.
- Audio/video/input: `nn_audio.c`, `nn_audio_in.c`, `nn_video.c`, `nn_codec.c`, `nn_hid.c`, `nn_vi.c`, `pad_sdl.c`.
- Graphics and services: `nvdrv.c`, `nvn_native.c`, `mkw_glue.c`, `nn_gs.c`, `nn_applet.c`.
- Desktop/POSIX shims: `host/posix/win32_posix.c`, `gdi_posix.c`, `linux_stubs.c`, `prof.c`, `display_sdl.c`.

These files need per-API classification against the pinned PS5 SDK. Some may be reusable as-is, some may use SDK-provided POSIX APIs, and some require PS5 service wrappers. The source audit does not establish which SDK symbols are available; that must be determined from the installed pinned SDK headers and libraries, not guessed.

## Required implementation sequence

1. **Define target identity independently of OS host.** Introduce an explicit target predicate/enum and ensure Linux-only and Windows-only flags, source files, output names and link branches cannot leak into PS5. Keep the build script's own host tools native to the build machine.
2. **Add PS5 toolchain discovery.** Require the pinned SDK checkout and its compiler/linker wrappers; fail early with a specific missing-SDK diagnostic. Do not fall back to native clang or the Linux path.
3. **Create a PS5 executable source set and entry point.** Refactor the shared game boot routine so a PS5 entry can call it without pretending to be Windows or SDL/Linux. Decide whether the game runs on the process main thread or a dedicated SDK thread based on the title/runtime contract.
4. **Implement minimum host services.** Start with logging, paths/files, memory reservation/mapping, threads/synchronization, controller input and audio. Keep optional desktop-only services out of the PS5 source set.
5. **Compile the Vulkan renderer in-process.** Build the renderer object set under `RELINK_PS5_RADV` and `RELINK_PS5_STATIC_VULKAN`; do not create a renderer SO/DLL. Link the pinned RADV archive whole and include the SDK C++ runtime, builtins, platform archive, wrapper symbols and linker script from the pinned reference recipe.
6. **Package the title.** Produce the native x86-64 PS5 ELF, convert/package it in the required homebrew format and document the expected Wobkot install path. The SDK toolchain and package flow must be proven from its scripts before wiring command-line flags.
7. **Only then build and test.** First compile/link a minimal title; then the ReLink host; then test Vulkan startup, display, input and game boot on the user's original PS5.

## Explicit non-goals for the next commit

- Do not set `LINUX = false` for PS5 and let it inherit the Windows branch.
- Do not define `_WIN32` as a shortcut to use Windows host code.
- Do not compile `display_sdl.c` or link SDL for the PS5 display path.
- Do not link RADV as a normal `-lvulkan` or dynamically load a PS5 Vulkan shared library.
- Do not disable Vulkan features or remove host services merely to get a link to pass.

## Current status

The PS5 Vulkan dispatch, display-surface path and static renderer-loader seam are present behind compile-time guards. The PS5 build target, host entry point/platform services and in-process renderer/RADV final link are not implemented. No build or hardware test is claimed.
