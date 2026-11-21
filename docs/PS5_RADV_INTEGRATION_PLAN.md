# PS5 RADV integration plan

Status: source-level integration plan. No PS5 SDK build or console validation has been performed.

## Decision

Keep the existing ReLink Vulkan renderer as the first graphics backend to evaluate, but integrate it as an in-process renderer linked with the PS5 RADV archive. Do not treat the PS5 environment as a desktop Linux Vulkan installation and do not attempt to load a system Vulkan ICD.

The public PS5 Vulkan Template demonstrates the relevant shape: the title links RADV into its executable, initializes Vulkan through `vk_icdGetInstanceProcAddr`, and presents through a PS5-specific display path. This is a reference architecture, not a drop-in ReLink port.

## Exact ReLink seams

### 1. Renderer loading: `host/rx_backend.c`

Current behavior:
- `load_vulkan()` locates a sibling DLL/shared library with `GetModuleFileNameA` and `LoadLibraryA`.
- It resolves `mkwvk_init` and the optional `relink_program_set` with `GetProcAddress`.
- `rx_close_asked()` resolves `mkwvk_close_asked` from the retained `HMODULE`.
- `nx2/build.py` describes the Vulkan backend as a separate `relink_vk.dll` / `relink_vk.so` product.

Required PS5 change:
- Add an explicit compile-time static-renderer mode, not a runtime guess based on a missing DLL.
- In that mode, call the linked `mkwvk_init(&vk_host)`, `relink_program_set`, and `mkwvk_close_asked` entry points directly.
- Do not change desktop loader behavior.
- Build the renderer sources into the title link, with the PS5 RADV archive and its required link recipe.

This seam is small in isolation, but it cannot be enabled until the PS5 executable build includes the Vulkan renderer objects and the host platform layer compiles.

### 2. Vulkan global dispatch: `gpu/vk/vk_main.cpp`

The PS5 RADV reference exports `vk_icdGetInstanceProcAddr` from the linked driver. The existing `RELINK_PS5_RADV` guarded loader path now uses that entry point instead of `LoadLibraryA`/desktop loader discovery.

That change only initializes global Vulkan dispatch. It does not create a display surface or make the current renderer start on PS5.

### 3. Presentation: `gpu/vk/vk_main.cpp` and `gpu/vk/vk_loader.h`

Current `start()` has two paths:
- Win32 surface creation on Windows.
- An SDL-provided `MkwVkWindow` surface on non-Windows builds.

The PS5 path needs its own surface/presentation implementation matching the RADV build's PS5 WSI, including the required display extensions and the platform's VideoOut integration. The exact implementation must follow the pinned RADV/SDK headers and the reference title; do not invent Vulkan structure fields or copy a desktop swapchain path.

### 4. Host startup and platform services

The Linux host is not a generic console host:
- `host/hos_main.c` starts the game on a worker thread and delegates window/event handling to the desktop display implementation.
- `host/posix/display_sdl.c` owns SDL window creation, event pumping, renderer startup, resize and close handling.
- `host/pad_sdl.c` uses SDL controller APIs.
- Audio, filesystem, clocks, synchronization and guest-memory management are also provided by the host layer.

A PS5 title needs explicit implementations for lifecycle, display, controller input, audio, files, memory reservation/mapping, threads and synchronization. The PS5 Vulkan Template's platform layer is evidence that these services can be wrapped for a homebrew title, but its code is not a compatible replacement for ReLink's host ABI.

### 5. Build integration: `nx2/build.py` and toolchain

The current build defines `LINUX = not toolchain.windows()`; therefore an unrecognized PS5 target would silently enter the desktop Linux path. The build also produces separate game and Vulkan renderer artifacts.

A PS5 target must be explicit and separate from `LINUX`. Its build contract must identify:
- The exact PS5 payload SDK/toolchain and compiler target.
- PS5-compatible C/C++ runtime and platform libraries.
- The pinned RADV archive and its matching linker recipe.
- The title entry point, link format, packaging and signing/conversion process.
- ReLink's renderer and host source sets, compiled into the title where required.

The PS5 reference link recipe has nontrivial requirements beyond adding a RADV archive: whole-archive linkage for driver entry points, SDK C++ runtime/builtins/platform libraries, thread wrappers, memory allocator wrappers, and local symbol handling. Do not reduce this to a guessed `-lvulkan` flag.

## Ordered implementation gates

1. Pin a mutually compatible PS5 payload SDK, PS5 Mesa/RADV revision and linker recipe. Record the exact upstream commits and licenses.
2. Add a PS5 target identity and static renderer link mode while preserving Windows/Linux defaults.
3. Implement the PS5 host lifecycle/display/input/audio/filesystem/memory services required to start ReLink.
4. Implement the PS5 WSI path against the pinned SDK/RADV interface.
5. Compile a minimal ReLink Vulkan renderer target and resolve actual compiler/linker errors.
6. Integrate NVN replay and verify the required Vulkan features, memory behavior and frame pacing on the original PS5.

Do not disable Vulkan feature requirements to get past initialization. If the pinned RADV stack lacks a required behavior, make a deliberate fallback or renderer change based on that evidence.

## Current stopping boundary

The repository can be prepared for the above seams, but a correct PS5 build cannot be produced from the ReLink source tree alone. The target-specific SDK, compatible RADV archive and title packaging toolchain are external dependencies. No local PS5 compiler or console test is available in this session, so no build or runtime success is claimed.
