# PS5 host startup and services audit

This audit follows the existing host implementation rather than assuming the PayloadSDK platform layer is a drop-in Win32 or Linux environment.

## Finding: the desktop host cannot yet be reused as a PS5 entry point

`host/hos_main.c` has two distinct startup paths:

- Windows builds compile `main()` around the main game routine and perform Windows-specific path validation, environment loading, log rotation, directory setup, image loading, memory reservation, display startup, and guest initialization.
- Non-Windows builds compile the shared routine as `static game_main()`, then `main()` initializes SDL on the process main thread, starts a 64 MiB game thread, and runs the SDL event loop.

A PS5 build cannot safely select the non-Windows path: it assumes SDL and desktop POSIX facilities. It also cannot simply define `_WIN32`: that selects Windows APIs, Windows entry/startup behavior, and Windows-specific display/input/audio code. The common guest boot sequence is currently embedded in this routine rather than exposed as a platform-neutral boot API.

## Host service inventory

| Area | Existing implementation dependency | PS5 port work |
|---|---|---|
| Process startup | `hos_main.c`: Windows path APIs or SDL/POSIX desktop main | Add a PS5 entry point and explicit title lifecycle; separate guest boot from desktop window/event-loop startup |
| Guest memory | `hos_main.c` and `hos_mem.c`: VirtualAlloc/VirtualFree on Windows; mmap/madvise/memfd and Linux udmabuf assumptions otherwise | Select PS5 virtual-memory allocation/mapping APIs; reserve the fixed guest address range and implement commit/discard semantics without assuming Linux memfd/udmabuf |
| Threads and synchronization | `hos_thread.c`: CreateThread, events, SRW locks, condition variables, thread descriptions, Windows wait/timer APIs | Map the full synchronization contract to PS5 platform primitives; preserve guest TLS, thread handles, virtual-core scheduling, and wake semantics |
| Filesystem | `nn_fs.c`: Win32 file handles, FindFirstFile, attributes, path separators, Windows directory creation | Implement title-relative RomFS and writable user/save/cache paths using supported PS5 filesystem APIs |
| Display | `nn_vi.c`, `display_sdl.c`, `display.c`: SDL or Win32 display/window functions | Use PS5 VideoOut/display ownership and the already-added Vulkan display WSI path; no SDL desktop window |
| Input | `nn_hid.c`, `pad_sdl.c`: SDL controller/motion/rumble and XInput/keyboard paths | Add DualSense/PS5 controller input and vibration mapping, or define a safe minimum input layer |
| Audio | `nn_audio.c`: Windows waveOut or SDL audio device | Implement PS5 audio output; audio renderer mixing logic can potentially remain shared if the sink is separated |
| Timing/profiling | Windows timing and desktop clock/thread assumptions in multiple host files | Map monotonic clock, sleep, timers, and profiling to SDK-supported interfaces |

## Memory is a first-order compatibility issue

The Linux implementation has an explicit fast path around shared memfd mappings and udmabuf/DMA-BUF imports. Those are Linux kernel interfaces, not portable POSIX features. The Windows path instead uses a reserved guest window and explicit commit/decommit behavior. Neither branch should be assumed correct on PS5.

The PS5 RADV integration currently targets a direct title-linked Vulkan driver. That does not by itself solve guest-memory placement, host-pointer import alignment/lifetime, or the synchronization requirements for renderer access to guest pages. A PS5 memory backend must be designed against the pinned SDK's actual VM APIs and the RADV external-memory capabilities before selecting the renderer import path.

## Safe implementation order

1. Keep the explicit `RX_TARGET=ps5` build guard. Do not let the current Linux source set compile as PS5.
2. Introduce a platform-neutral guest boot routine only after its platform dependencies are parameterized; keep Windows and SDL startup behavior unchanged.
3. Implement PS5 process/title startup and logging, then filesystem and guest-memory services.
4. Implement thread/synchronization primitives and controller input.
5. Separate audio mixing from the desktop output sink and add PS5 audio output.
6. Connect the PS5 display lifecycle to the direct RADV renderer and verify the title's shutdown/exit path.
7. Only then enable the PS5 source set and final static title link.

## Deliberately not changed

No speculative `#ifdef RELINK_PS5` shim was added to the desktop entry point. Doing so before resolving the memory, filesystem, thread, and display contracts would create a target that may compile while failing at boot or corrupting guest memory.

No PS5 build or console test is claimed by this audit.
