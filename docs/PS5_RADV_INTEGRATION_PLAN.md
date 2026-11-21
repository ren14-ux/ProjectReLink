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

The PS5 RADV reference exports `vk_icdGetInstanceProcAddr` from the linked driver. The existing `RELINK_PS5_RADV` guarded loader path now uses that entry point instead of `LoadLibraryA`/desktop loader discovery. This initializes global dispatch; it is not sufficient on its own to start the renderer.

### 3. Presentation: `gpu/vk/vk_main.cpp` and `gpu/vk/vk_loader.h`

Current `start()` has three paths:
- Win32 surface creation on Windows.
- An SDL-provided `MkwVkWindow` surface on desktop non-Windows builds.
- A guarded `RELINK_PS5_RADV` path that enumerates display properties, planes and modes; chooses 1920x1080 when available (otherwise the smallest reported mode); creates a `VK_KHR_display` surface; verifies present support on the graphics queue; and creates the swapchain.

The PS5 path forces FIFO presentation and checks the display WSI function pointers, identity transform support, and supported alpha modes. This is source-level integration only: it has not been compiled against the pinned SDK/RADV headers or run on hardware. Failure cleanup and queue-family fallback still need review.

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


## Pinned RADV dependency and link contract

The reference PS5 Vulkan build pins its Mesa fork independently of the payload SDK. ReLink records the same Mesa revision used by `mihawk-99/PS5_Vulkan/tools/build-radv.sh`:

- Mesa fork: `https://github.com/mihawk-99/PS5_Mesa`
- Mesa revision: `1ba9874f478657ccf6da72ce55f0260164cc0bd0`
- RADV build option: `-Dradv-winsys=ps5`
- Expected release archive: `.deps/native/radv-release/lib/libvulkan_radeon.ps5.a`
- SDK revision: see `deps/ps5-payload-sdk.commit`

The upstream title link recipe in `PS5_Vulkan/tools/radv-link.sh` links the RADV archive whole (weak dispatch-table references otherwise leave entry points absent), then groups the SDK's `libc++.a`, `libc++abi.a`, `libunwind.a`, Clang x86-64 builtins, and `libps5platform.a`. It also wraps pthread stack/join functions, stream close/flush, and the platform heap/libc functions. ReLink must carry this complete recipe or a reviewed equivalent; linking only `libvulkan_radeon.ps5.a` is insufficient.

The upstream recipe expects a host Clang resource archive at `lib/linux/libclang_rt.builtins-x86_64.a`, the SDK platform archive, and the PS5 PIE unwind linker script. It generates a version map to keep compatibility aliases local. These are hard link inputs, not optional conveniences.

The reference build script builds Mesa from the pinned source archive using the PS5 cross file and emits a provenance file recording the Mesa revision, SDK revision, assertion mode, and archive SHA-256. ReLink should preserve that provenance and use the release (assertions-off) archive for a game build; the debug archive is intended for driver development and CTS work.

**Important scope boundary:** this pins the RADV source dependency, not a completed ReLink build. ReLink still needs a dedicated PS5 build target, static renderer linkage (including `mkwvk_init`, `mkwvk_close_asked`, and `relink_program_set`), and PS5 host/platform services. The Vulkan display-surface path has been added behind `RELINK_PS5_RADV`, but remains uncompiled and untested.


## Verified PS5 display/WSI contract

The PS5 homebrew reference documents the concrete RADV display path. VideoOut integration belongs to the PS5 RADV winsys; ReLink must not open VideoOut independently while this Vulkan WSI owns the display.

1. Enable `VK_KHR_surface` and `VK_KHR_display` at instance creation. The PS5 branch now does this.
2. Query display properties, planes, supported displays and modes. The current implementation selects the first reported display, a compatible plane, prefers 1920x1080, and otherwise selects the smallest nonzero mode.
3. Create the surface with `vkCreateDisplayPlaneSurfaceKHR`, identity transform and an alpha mode supported by the selected plane. The swapchain uses the selected mode's visible region.
4. Verify that the graphics queue supports presentation, then use FIFO. The swapchain requests at least three images when the caps permit, but its existing general logic may request four in other configurations.
5. Keep display ownership in Vulkan WSI; do not mix this path with a second direct VideoOut buffer/flip loop.

The implementation bypasses the SDL window bridge in `start()` and continues through the existing Vulkan swapchain acquire/submit/present path. Remaining work includes robust present-queue family selection and cleanup on partial startup failure. This is not a compile or console validation claim.


## Latest source correction

The PS5 display functions are part of the `VKX_INSTANCE` dispatch macro in `gpu/vk/vk_loader.h`. The display-surface entry was missing a trailing macro continuation, which detached the subsequent surface-query functions from the macro and could break compilation. The continuation has been restored. This is a source inspection fix; no PS5 compile has been run.


## PS5 renderer startup correction

`host/rx_backend.c` now treats `RELINK_PS5_STATIC_VULKAN` as a single-renderer configuration: it calls the linked `mkwvk_init` directly, starts that renderer, and returns failure if either step fails. It no longer falls through to `wgr_start` (the desktop WebGPU backend) when the PS5 RADV renderer is unavailable. On a PS5 title, silently selecting a desktop fallback would be incorrect and could make renderer startup appear successful while no supported presentation path exists.

This is a source-level control-flow correction only. The PS5 executable build still does not compile/link the renderer objects and RADV archive, and no PS5 build or runtime test has been performed.

## PS5 guest-memory import alignment

`gpu/vk/vk_memory.cpp` now rounds imported guest-pool ranges outward to 16 KiB when `RELINK_PS5_RADV` is enabled. This matches the pinned RADV PS5 winsys' `RADV_PS5_PAGE_BYTES` requirement for `radv_ps5_memory_grant_gpu()` and `buffer_from_ptr()`; the renderer's ordinary 4 KiB NVN bookkeeping boundaries are not sufficient for the kernel's GPU-access grant.

The PS5 branch also excludes the desktop `VirtualQuery` / `VirtualAlloc(MEM_COMMIT)` import-time path. This is intentionally not replaced with a guessed PS5 syscall. It establishes a required host contract: the eventual PS5 guest-memory backend must commit the complete guest page range when the game maps it, before Vulkan imports it. If it does not, RADV's `sceKernelMprotect()` grant can fail. The existing `host/hos_mem.c` is not yet a PS5 implementation, so this change does not make guest memory or the PS5 build complete.

This is a source-level integration change only; no compile or console test was run.

## Newly confirmed hard blocker: fixed guest address is not viable on the measured PS5

The memory-backend audit compared ReLink's address contract with the pinned SDK's recorded console probe:

- ReLink's native runtime folds a fixed host base of `0x100000000000` (16 TiB) into every lifted guest pointer.
- `host/hos_main.c` attempts to reserve a contiguous `HOS_GUEST_SPACE` of 512 GiB at that exact address.
- The pinned SDK probe records successful reservation of 16 GiB at 64 GiB, but no reservation at or above 1 TiB on the measured PS5 system software 12.09. It does not demonstrate that a 512 GiB range is available at 64 GiB either.

Consequently, implementing a direct-memory allocator at the existing 16 TiB base would be premature: the reservation itself is not supported by the available console evidence. The pointer-width correction to `uintptr_t` fixes native address arithmetic but does not make the fixed virtual address reservable.

### Required decision before allocator implementation

The next engineering task is an address-layout redesign, not a `hos_commit_ps5()` implementation. Candidate approaches are:

1. Select a lower fixed base and prove that the entire required guest range fits there; or
2. Reduce the guest window and audit every image, heap, address-region, generated-code, and GPU-import assumption; or
3. Introduce segmented guest mappings, which would require changing the constant-base pointer model and likely generated/runtime access paths.

No option is selected by this change. The original 16 TiB base and 512 GiB reservation must remain unchanged until a complete compatibility analysis establishes a replacement. In particular, do not assume that the 16 GiB reservation result proves a 512 GiB reservation will work.

The user has asked to skip console testing, so this work does not introduce or request another probe. The evidence above is source/documentation evidence from the pinned SDK's existing recorded probe, not a new test performed in this session. The PS5 target remains intentionally blocked; no allocator or title package is claimed to work.
