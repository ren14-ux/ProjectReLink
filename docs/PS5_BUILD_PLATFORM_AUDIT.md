# PS5 port: build-system and host-platform boundary

Status: source audit only. No PS5 SDK, compiler, linker, runtime, or graphics sample has been supplied or verified. This document records concrete build blockers found in the current upstream source; it does not claim that a PS5 build exists.

## Executive finding

The current native build has two supported target families: Windows and Linux/POSIX. It does not have a PS5 target abstraction. A Linux-hosted PS5 cross-compiler cannot be selected by setting a generic target variable: the build script's non-Windows branch is the desktop Linux build, not a generic ELF target.

The correct next implementation boundary is a distinct target/platform layer for process startup, guest memory, threads/synchronization, input, audio, files, and presentation. Vulkan must be treated separately: even a working native PS5 host executable would not make the desktop Vulkan renderer compatible.

## Evidence in source

### 1. Target selection is Windows-versus-everything-else

- `third_party/a64-kit/tools/toolchain.py`: `windows()` recognizes Windows or `RX_TARGET=windows`; there is no PS5 target.
- `native_target()` emits either an x86_64 Windows target or no explicit target. It has no AArch64/PS5 cross-target selection.
- `nx2/build.py`: `LINUX = not toolchain.windows()`, and the non-Windows executable/library names are `relink` and `relink_vk.so`.
- The Linux branch links `-lpthread`, `-ldl`, and `-lm`, and its build comments describe SDL2 and a POSIX window/sound path. These are Linux assumptions, not a PS5 platform contract.

A PS5 build needs an explicit target identity and a separate toolchain description (compiler triple, sysroot/SDK headers, linker, startup objects, libraries, and output format). Reusing `LINUX` would silently select the wrong platform.

### 2. The native host layer is deeply Windows-shaped

- `host/hos.h` defines the guest Horizon OS emulation API; it is not itself a PS5 host abstraction.
- `host/hos_main.c` includes Windows APIs on Windows and a custom POSIX compatibility layer on Linux. The POSIX path uses `mmap`, `prctl` (under Linux), file descriptors, and Linux/POSIX headers.
- `host/posix/display_sdl.c` is explicitly an SDL/Linux display implementation. It creates a Vulkan surface through SDL and uses Windows-like compatibility types/functions from `host/posix`.
- `host/pad_sdl.c` uses SDL controller APIs and Windows synchronization primitives exposed through the compatibility layer.
- `host/rx_backend.c` loads the renderer as a DLL/shared object and chooses the Vulkan backend or in-process WebGPU backend. It is not a PS5 application entry point or platform renderer.

This means the port needs a deliberate PS5 host implementation or a proven compatibility layer for each host service. It is not enough to change a compiler flag.

### 3. The guest executable and renderer are separate build products

`nx2/build.py` builds the lifted game host executable and renderer as separate products. Its source collection functions build:
- `gpu/render` plus Suyu shader-recompiler components for the WebGPU renderer;
- `gpu/vk` plus Eden/Sirit components for the Vulkan renderer.

A PS5 port must choose and implement a graphics boundary before trying to package the app. The existing renderer loader expects desktop shared-library conventions and desktop window/surface integration.

### 4. CPU and SIMD assumptions must be audited independently

The README currently lists an AVX2-capable PC CPU as a requirement. `nx2/build.py` has an ASTC encoder SIMD selection that chooses x86 AVX/SSE flags on x86 and a plain C++ path otherwise, with NEON explicitly disabled in the shown flags. The PS5 CPU is AArch64, so this code does not prove ARM64 readiness. The build must audit all compiler flags, inline assembly, intrinsics, atomics, and third-party libraries for AArch64 before a PS5 executable can be expected to link or run.

## Recommended implementation sequence

1. **SDK/toolchain contract:** obtain a known-working Wobkot PS5 homebrew sample and the exact SDK/toolchain used to build it. Record compiler, linker, sysroot, startup objects, libraries, application packaging format, and required entry-point conventions.
2. **Target selection:** add an explicit PS5 target mode. Keep Windows and Linux behavior unchanged. Do not alias PS5 to Linux.
3. **Minimal process spike:** compile and package a tiny PS5 app that starts, logs, allocates memory, creates a thread, reads controller input, and exits cleanly. This isolates the host ABI from ReLink's large lifted game.
4. **Host platform layer:** implement only the required host services behind a PS5-specific source set. Prioritize memory reservation/mapping, threads/locks/condition variables, clocks, filesystem paths, logging, controller input, audio, and lifecycle.
5. **Graphics spike:** independently prove one frame can be presented using a graphics API actually available to the homebrew environment. This is the gating milestone for the Vulkan question.
6. **Renderer decision:** only after step 5, choose between a viable Vulkan compatibility implementation and a native PS5 renderer. Do not disable Vulkan feature checks or lower the Vulkan API version as a substitute for missing functionality.
7. **Full integration:** connect NVN replay to the proven graphics backend, then build the lifted game and package it.

## Vulkan-specific implication

The Vulkan feature inventory remains a separate blocker. The current backend requests Vulkan 1.3 and depends on dynamic rendering, synchronization2, timeline semaphores, buffer device address, and platform presentation integration, among other optional/conditional features. A PS5 GPU's hardware capability alone does not provide a Vulkan loader, ICD, surface implementation, or matching extension behavior in the Wobkot runtime. Each must be demonstrated by the actual SDK/runtime.

## Newly identified PS5 Vulkan path (research update, 2026-11-21)

Public PS5 homebrew work changes the graphics assessment materially:

- `mihawk-99/PS5_VulkanTemplate` documents a native x86-64 PS5 title using RADV (Mesa) linked into the title, with PS5 VideoOut presentation through `VK_KHR_display`. Its documented stack separates the PS5 payload/platform SDK, PS5 Mesa/winsys, RADV build/link tooling, and the title. The template reports working samples including dynamic rendering and mesh shaders. This is a much stronger starting point than assuming a stock system Vulkan loader exists.
- `mpereiraesaa/ps5-vulkan` documents an experimental gfx1013 Vulkan 1.3 implementation with dynamic rendering, synchronization2, timeline semaphores, and native VideoOut presentation. It explicitly documents bounded support and non-conformance; its own README says there is no Vulkan loader/ICD and native builds need its PS5 payload SDK plus companion graphics support library.
- The PS5_VulkanTemplate documentation says RADV is linked into each title; a title cannot simply load a system Vulkan driver. This matters because ReLink currently dynamically loads `mkw_vk.dll` / `relink_vk.so`. A PS5 integration likely needs a statically linked renderer/driver arrangement or an in-process loader shim, not the existing desktop DLL loading path.

These repositories are external technical references, not dependencies already present in this fork. Their reported hardware results are claims from their maintainers; they do not establish that ReLink's workload is compatible.

### Corrected CPU note

The PS5 CPU target is x86-64. The earlier statement in this document that the PS5 CPU is AArch64 was incorrect and is superseded. ReLink's x86-oriented code and AVX2 assumptions therefore need validation against the PS5 toolchain/ABI, but a wholesale ARM64 port is not the task. The toolchain still needs a PS5-specific target (the cited template documents `x86_64-sie-ps5`), startup/runtime integration, and platform libraries.

### Revised graphics investigation

Do not begin by replacing the renderer with a native AGC backend. First evaluate the existing Vulkan backend against the PS5 RADV stack:

1. Obtain/pin the PS5_VulkanTemplate stack and its compatible PS5 payload SDK / RADV build as external build dependencies; inspect their exact license and revision requirements before integration.
2. Compare ReLink's required and optional Vulkan features with the target RADV feature/limit report, not just the advertised API version. In particular check mesh/task shader behavior, buffer device address, external-memory import, sparse binding, descriptor/pipeline features, subgroup-size behavior, and all shader-stage/format requirements.
3. Replace desktop surface creation with the PS5 VideoOut WSI path and ensure ReLink's renderer is linked in the way RADV expects.
4. Replace or adapt the desktop shared-library boundary in `host/rx_backend.c`; do not assume `dlopen` of a separately built Vulkan DLL works in the PS5 title environment.
5. Build a minimal ReLink renderer smoke target first, then integrate NVN replay. A Vulkan 1.3 version string alone is not acceptance evidence.

The Vulkan compatibility direction is now **RADV-on-PS5 first**, subject to exact feature/limit and license checks. A native AGC backend remains a fallback only if a required ReLink feature cannot be supported or implemented on RADV.

## What can be done without the SDK

Source analysis, build-graph mapping, and isolated portable-code cleanup can proceed. A correct PS5 platform implementation, renderer integration, signed/packageable homebrew artifact, and meaningful build validation cannot be completed responsibly without the target toolchain and a working PS5 graphics/API reference.

## Current status

- Branch: `ps5/option-a-vulkan-compat`
- Scope of this change: documentation only
- PS5 compilation: not performed
- PS5 device test: not performed
- Required next input for implementation: the Wobkot SDK/toolchain package or a small known-working PS5 homebrew graphics sample and its build instructions
