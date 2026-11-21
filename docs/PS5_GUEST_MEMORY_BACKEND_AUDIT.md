# PS5 guest-memory backend design audit

This follow-up checks ReLink's actual guest-memory contract against the pinned `PS5_PayloadSDK` platform layer at `b83202be73e930050e00de0f1b4d9ec46c0391bf`.

## ReLink's fixed guest window

- `third_party/a64-kit/rt/a64.h` fixes `A64_HOST_BASE` at `0x100000000000` (16 TiB). Lifted guest accesses use this constant directly; the base cannot be moved without changing the runtime/build contract.
- `host/hos.h` defines `HOS_GUEST_SPACE` as 512 GiB. `host/hos_main.c:reserve_guest()` reserves this whole range at the fixed base.
- The Windows path reserves the address range and commits memory in `hos_commit()`. The Linux path maps the entire 512 GiB window with `MAP_NORESERVE`; physical pages arrive on demand. The Linux Vulkan optimization depends on memfd plus `/dev/udmabuf`, which is not a PS5 interface.
- `host/hos_mem.c` aligns commits to 64 KiB in the Windows implementation, while address-region allocation and page accounting include 4 KiB granularity. The POSIX discard path uses `madvise`; the Windows region-gap release path uses `VirtualFree(MEM_DECOMMIT)`.

## What the pinned PS5 SDK actually provides

The pinned platform layer contains concrete kernel and memory interfaces; this is not a generic POSIX port:

- `ps5platform/kernel.h` declares `sceKernelReserveVirtualRange`, `sceKernelMunmap`, `sceKernelAllocateDirectMemory`, `sceKernelReleaseDirectMemory`, `sceKernelMapDirectMemory`, and `sceKernelMprotect`.
- The SDK documents 16 KiB kernel pages and 64 KiB direct-memory allocation alignment.
- `ps5platform/shm.h` / `platform/src/shm.c` expose `ps5_vrange_reserve()` and `ps5_shm_create/map/unmap/destroy()`. A shared-memory object allocates direct memory; views map it at a fixed address. `PS5_SHM_KEEP_RESERVED` unmaps a view and immediately reserves its virtual range again.
- `ps5platform/kernel.h` defines GPU read/write protection bits, but the current `ps5_shm_map()` implementation translates only CPU read/write/execute flags. Do not assume that its default mapping is already the correct GPU-visible mapping for ReLink's renderer.

The SDK placement helper attempts the requested hint first and rejects the documented RADV GPU window at `0x200000000–0x300000000`. ReLink's 16 TiB base is outside that window, but this source inspection does not prove that a 512 GiB fixed reservation succeeds on the user's console.

## Required PS5 implementation shape

A correct backend needs to preserve the fixed 16 TiB base and 512 GiB guest address space while backing only committed ranges with PS5 direct memory:

1. Reserve the exact 512 GiB range at `A64_HOST_BASE` with the kernel virtual-range API. Fail startup if the exact address or full size is refused; never accept a different base.
2. Implement `hos_commit(addr, size)` by rounding to the kernel's supported mapping granularity, allocating direct memory, and mapping it at the corresponding fixed guest address. The operation must be idempotent for already-backed pages and safe for overlapping or adjacent commits.
3. Track each direct-memory allocation and its guest virtual extent. On discard/decommit, unmap the corresponding view, restore the reservation, and release the physical allocation only when no CPU or GPU consumer can still reference it.
4. Preserve the existing zero-on-new-commit contract. Reused direct-memory pages must not expose stale guest data.
5. Reconcile 4 KiB guest bookkeeping with the SDK's 16 KiB mapping pages and 64 KiB direct-memory units. A sub-64 KiB free cannot release an allocation if another guest range shares that unit.
6. Integrate the renderer's imported-memory lifetime with the backend. Existing `rx_guest_imported()` prevents Linux page discard under live imports; the PS5 implementation needs an equivalent guarantee before releasing or remapping direct memory.

## Vulkan memory is a separate unresolved gate

The existing renderer's Linux fast path is a memfd/udmabuf DMA-BUF import and must not be compiled for PS5. The pinned PS5 RADV winsys does, however, implement a separate host-pointer import path: `radv_ps5_buffer_from_ptr()` accepts an aligned application-owned pointer, calls `radv_ps5_memory_grant_gpu()`, and retains only a wrapper because the application's memory remains its own. The grant function calls `sceKernelMprotect()` with CPU read/write and GPU read/write protections. The PS5 winsys requires the pointer and size to be aligned to its 16 KiB page size and the range to fit below its GPU address limit.

This is the concrete renderer integration route for ReLink's guest pages, subject to confirming the exact pinned driver revision and runtime behavior. ReLink's fixed 16 TiB guest base is below the winsys' 2^47 address ceiling, and its 512 GiB window remains below that ceiling. The guest backend should map CPU-accessible direct memory at the fixed guest addresses, then let the RADV import path grant GPU access; the import alignment must remain at least 16 KiB. The SDK's shared-memory convenience API currently requests CPU protections only, which is consistent with this sequence, but its mapping/unmapping lifetime still needs to be coordinated with RADV imports.

Do not route PS5 through Linux udmabuf. Do not release or remap a guest extent while RADV still owns an imported buffer for it.

## Newly identified blocker: guest pointer width mismatch

A source-level inconsistency must be resolved before implementing the PS5 allocator:

- `host/hos.h` defines a 512 GiB guest window (`HOS_GUEST_SPACE = 1ULL << 39`) and places the heap at guest offset `0x1000000000` (64 GiB).
- `hos_main.c:reserve_guest()` reserves the full window at `A64_HOST_BASE`.
- However, `third_party/a64-kit/rt/a64.h` currently defines `A64_PTR(a)` as `A64_MEM + (uint32_t)(a)`. This truncates every guest offset to 32 bits.
- `host/hos.h` defines `GP(a)` through `A64_PTR(a)`; host-side heap, system, and address-region helpers therefore appear to wrap offsets above 4 GiB into the first 4 GiB of the window.

The runtime header's own description said it modeled a flat guest memory below 4 GiB, while the HOS layer describes and allocates a 39-bit guest address space. Inspection of `a64_rt.c` and the translator confirms that normal guest loads/stores and atomic helpers route through `A64_PTR`, and memory addresses are carried as `uint64_t`; the `uint32_t` cast was therefore inconsistent with the native 39-bit HOS map. The pointer conversion is now changed to `uintptr_t`: native 64-bit builds preserve the full guest offset, while wasm32 keeps its 32-bit pointer width. This is a source-level correction; it still needs compilation and runtime validation on native and wasm builds.

### Consequence

Do not implement or enable PS5 guest-memory mapping until this address-width contract is reconciled. The direct-memory plan remains viable only if the actual guest-address model and the 512 GiB HOS layout are made consistent first. No runtime behavior is inferred from this source inspection alone.

## First-touch commit path: additional platform work

The guest allocator is not just `hos_commit()`. The current demand-commit path spans three Windows-compatibility layers:

- `host/mkw_glue.c` implements explicit commits with `VirtualAlloc(MEM_COMMIT)`, rounded to 64 KiB.
- Its `commit_on_touch()` exception handler handles an access fault inside the reserved guest window, checks `VirtualQuery()`, and commits a 64 KiB unit on first touch. `hos_commit_init()` registers this handler.
- On Linux, `host/posix/win32_posix.c` translates SIGSEGV/SIGBUS into the compatibility exception record. Its `VirtualAlloc()` maps ordinary anonymous memory with `mmap()`; its `VirtualQuery()` treats the already-accessible guest window as committed. The kernel's `MAP_NORESERVE` demand paging supplies the physical pages, so the Linux handler does not need to allocate them itself.

PS5 cannot simply reuse the Linux behavior: its fixed guest range must first be reserved and a fault must cause a PS5 mapping to be installed. Nor can it blindly reuse the Windows behavior: PS5 has neither Windows `VirtualAlloc` nor Windows exception delivery.

### Required platform contract

A PS5 memory backend must provide all of the following as one coordinated implementation:

1. Reserve the exact `[A64_HOST_BASE, A64_HOST_BASE + HOS_GUEST_SPACE)` range as inaccessible virtual space.
2. Handle faults in that range and distinguish an uncommitted reservation from a mapped page without calling non-signal-safe allocators or taking a lock that the faulting thread might already hold.
3. Map a 64 KiB direct-memory unit at the faulting guest address (or use a separately validated PS5 anonymous-memory route), while preserving the 16 KiB kernel page and RADV host-import alignment requirements.
4. Make concurrent faults on the same unit idempotent; one thread must not replace or release a mapping another thread just installed.
5. Keep mapping metadata for decommit and coordinate release with `rx_guest_imported()` / `rx_guest_release()` so the GPU never retains a pointer to released pages.
6. Supply a PS5-aware memory query/state check or refactor the first-touch handler to use a platform-neutral page-state API. The Linux shim's `VirtualQuery()` implementation is not reusable as-is because it reports the entire guest window committed.

This confirms why adding only a `hos_commit_ps5()` function would be incomplete. The fault path, page-state tracking, and Vulkan import lifetime must be designed together.

## PS5 first-touch signal support: source evidence and remaining proof

The pinned SDK includes FreeBSD-compatible declarations for `sigaction()`, `sigaltstack()`, `SIGSEGV`, and `SIGBUS` in its FreeBSD headers. This is useful evidence that a POSIX-style signal API is part of the SDK surface, but header declarations alone do not establish that a PS5 title receives recoverable access-violation signals for reserved guest addresses, that its signal context can be edited to retry the faulting instruction, or that the handler can safely call the required kernel mapping APIs.

The SDK platform layer inspected so far does not provide a guest-memory first-touch handler. Its existing direct-memory and shared-memory helpers are ordinary thread-context APIs. The next implementation gate is therefore a minimal PS5 title-side probe that reserves a test range, deliberately reads/writes an uncommitted page, and records whether the signal handler runs with a usable machine context. Only after that behavior is proven should a handler be added to ReLink. If the signal path is unavailable or cannot safely recover, use explicit commits at every guest allocation boundary and remove reliance on implicit first-touch for PS5.

Also confirmed in the current code:
- `A64_WRITE_TRACK` is enabled by default in `nx2/build.py`, unless `RX_SOFT_WRITE_WATCH=0`. Its dirty bitmap is sized for the first 4 GiB, and `A64_MARK` intentionally indexes with 32-bit guest addresses. This is a separate 4 GiB write-watch design, not the 39-bit host pointer conversion. PS5 build integration must decide whether to disable this path or redesign it before GPU write tracking is enabled.
- `hos_mem.c`'s `pages_used` bitmap is also deliberately limited to the first 4 GiB; the allocation-pages path has a separate above-4-GiB branch that commits without using that bitmap. Do not expand this bitmap to 512 GiB without evaluating its memory cost and the existing zero-on-new-pages behavior.

## Console-probe evidence: virtual reservation and first-touch signals

The pinned SDK's `platform/docs/PROBE.md` records measured PS5 behavior from system software 12.09, inside a RetroArch title:

- `sceKernelReserveVirtualRange` successfully reserved 16 GiB at the hint `0x10_0000_0000` (64 GiB).
- The probe reports no reservation granted at or above `0x100_0000_0000` (1 TiB), including hints at 1 TiB, 4 TiB and 16 TiB. A hint that cannot fit the requested range is refused rather than relocated.
- ReLink's current `A64_HOST_BASE = 0x100000000000` is `0x1000_0000_0000` (16 TiB), outside the measured reservable range. The current fixed base therefore cannot be assumed usable on this PS5 environment.
- The probe does not establish that a contiguous 512 GiB reservation is available at any lower address. Its demonstrated 64 GiB reservation is 16 GiB, not 512 GiB.

This is a hard address-layout blocker. Do not implement the allocator around the current 16 TiB base. A lower base, smaller guest window, or segmented guest mapping requires a full compatibility audit of image layout, heap, address regions, generated code, pointer arithmetic, and GPU import address constraints.

The same probe establishes that a read from a reserved, unmapped page raises SIGSEGV with the correct fault address and that the signal handler can inspect the machine context, patch the faulting instruction through a writable alias, and return successfully. It completed 200 fault sites and reported approximately 57 microseconds per fault. This is stronger evidence than the SDK headers alone: recoverable signal delivery and a usable machine context were demonstrated in a title on the measured console.

However, this test patches the faulting instruction; it does not call `sceKernelAllocateDirectMemory` or `sceKernelMapDirectMemory` from the signal handler. A separate probe must test mapping a direct-memory page at the faulting address from the handler, plus concurrent faults on the same 64 KiB unit, before ReLink can use this as its first-touch implementation.

The same audit found two intentional 4 GiB limits that should not be confused with the native 39-bit pointer correction:
- `A64_WRITE_TRACK` is enabled by default unless `RX_SOFT_WRITE_WATCH=0`; its dirty bitmap and `A64_MARK` indices cover the first 4 GiB only. PS5 must disable this path or redesign it before enabling GPU write tracking.
- `hos_mem.c`'s `pages_used` bitmap covers the first 4 GiB only. The existing above-4-GiB `AllocateMemoryPages` branch deliberately avoids that bitmap and has separate zero-on-new-page behavior. Do not expand the bitmap to 512 GiB without analyzing memory cost and semantics.

## Next console probe: contiguous guest-window reservation

The SDK's virtual-address survey tested reservations only up to 16 GiB per call. It does not yet prove whether ReLink can reserve its whole guest window as one range or stitch together adjacent reservations. The address survey suggests testing from the 64 GiB base (`0x10_0000_0000)), below the 1 TiB refusal boundary.

A callable probe has been added at `tools/ps5/guest_window_probe.c`. It performs reservation-only tests (no direct-memory allocation and no guest-page writes):

1. Try one 512 GiB reservation at 64 GiB.
2. If that fails, try 32 adjacent 16 GiB reservations covering 64–576 GiB.
3. If that fails, try one 448 GiB reservation covering 64–512 GiB.
4. If that fails, try 28 adjacent 16 GiB reservations covering 64–512 GiB.

Every successful reservation is released before the probe returns. The probe rejects a kernel-selected address that differs from the requested hint and cleans up any earlier segments if a later segment fails. This distinguishes a contiguous guest window from a set of disjoint regions and checks the final 64 GiB needed by the full 512 GiB layout.

The probe is source-only at this stage. It must be compiled into a PS5 title and run on the target console. Until its output is available, neither the 512 GiB nor the reduced 448 GiB layout is considered confirmed.

## Decision

Do not add a speculative `hos_mem_ps5.c` yet. The SDK provides enough primitives to design a direct-memory-backed arena, but the exact 512 GiB reservation, overlapping-commit bookkeeping, zeroing/decommit rules, and GPU-visible mapping/import contract must be established first. The PS5 build guard remains correct until these are implemented together.

No PS5 compilation or console test is claimed.


## Follow-up: exact SDK mapping semantics (2026-11-21)

The pinned SDK source was inspected directly at `platform/include/ps5platform/kernel.h`, `platform/include/ps5platform/shm.h`, and `platform/src/shm.c`. This narrows the allocator design but does not close the first-touch gate:

- `sceKernelReserveVirtualRange()` reserves virtual address space; `sceKernelMunmap()` releases mappings/reservations. The kernel header declares both with the signatures used by the SDK platform layer.
- `sceKernelAllocateDirectMemory()` allocates physical direct memory in 64 KiB-aligned units. `sceKernelMapDirectMemory()` maps a direct-memory offset at a requested virtual address; its documented page alignment is 16 KiB.
- `ps5_shm_create()` rounds backing allocations up to 64 KiB. `ps5_shm_map()` maps a view at 16 KiB granularity and can replace a reserved range with a fixed mapping.
- Crucially, `ps5_shm_map()` translates only `PS5_SHM_READ/WRITE/EXEC` to CPU protection bits. It does not grant `PS5_KERNEL_PROT_GPU_READ/WRITE`. ReLink must therefore explicitly grant GPU access with the pinned RADV host-pointer import path (or use a reviewed SDK extension); the shared-memory helper alone is not a complete Vulkan mapping API.
- The SDK helper's map/unmap path is ordinary thread-context code. It does not establish that allocation, mapping, or unmapping is safe from a SIGSEGV handler. The earlier signal probe established recoverable fault delivery and context access only, not kernel mapping from the handler.

### Allocator consequences

A PS5 page table cannot be represented as a simple 4 KiB bitmap. The implementation needs separate accounting for 16 KiB virtual mapping pages and 64 KiB physical direct-memory units. A 64 KiB physical unit may back four adjacent 16 KiB virtual pages, and it must not be released while any subpage remains mapped or any RADV import holds the corresponding guest range. Partial commits and frees therefore require a reference-counted or interval-based allocator, not one direct-memory allocation per arbitrary guest commit call.

The existing `ps5_shm` wrapper is useful as a source of fixed-map mechanics, but its global direct-memory allocation model and CPU-only protections should not be wired into `hos_commit()` unchanged. The next safe implementation step is to settle a bounded arena layout and ownership table, then validate whether the required kernel map/protect calls are callable from the first-touch handler. Until then, no speculative PS5 allocator is added and the PS5 build guard remains in place.

No compilation, first-touch mapping probe, or console test was performed for this follow-up.

## Address-layout calculation: what a lower-base candidate must cover

The guest address space is a 39-bit offset space, not a 512 GiB host-address base. The current map uses these guest offsets:

- System objects: 16 MiB through 112 MiB.
- Main image and SDK modules: beginning around 2 GiB, ending at the generated `hos_image_end`.
- Heap: begins at 64 GiB (`HOS_HEAP_BASE = 0x1000000000`).
- Address regions: allocated downward from the 512 GiB guest-space ceiling (`HOS_SPACE_END = 0x8000000000`).

For any candidate host base `B`, the complete reservation must therefore cover the half-open interval `[B, B + 512 GiB)`; guest offset `x` must continue to resolve to `B + x`. Moving the base does not change any guest offsets, image addresses, heap offsets, or generated guest addresses. It changes only the host virtual address at which the flat guest window is mapped.

The existing probe's 64 GiB success is evidence for a 16 GiB reservation at that hint, not for the full 512 GiB interval. A 64 GiB base would place the end of the required window at 576 GiB, below the observed 1 TiB refusal threshold, making it a reasonable *candidate to investigate* but not a validated choice. Do not set it as the production base until the complete 512 GiB range is proven available on the target environment.

### Consequence for the implementation

The current source already makes the base overrideable with the `A64_HOST_BASE` compile-time definition in `third_party/a64-kit/rt/a64.h`. That is the correct mechanism for a PS5-specific address choice: the PS5 build should provide one consistent value to every translation unit that includes the runtime header, while desktop defaults remain unchanged. `reserve_guest()` must still reserve exactly that base and the entire 512 GiB window, and startup must fail rather than accept a relocated result.

The base change alone does not solve backing memory. The separate allocator/fault/decommit/GPU-import lifetime work remains required. No production base is selected in this audit, and no console probe or test was run.


## PS5 build-driver audit: source and flag boundaries (2026-11-21)

The PS5 compiler wrappers already exist in `third_party/a64-kit/tools/toolchain.py`:

- `ps5_sdk_root()` verifies the installed SDK against `deps/ps5-payload-sdk.commit` and checks for the pinned compiler, linker, headers, and static runtime libraries.
- `ps5_compiler(cxx=False)` selects `prospero-clang` or `prospero-clang++`; `ps5_linker()` selects `prospero-lld`.
- These helpers are currently unreachable from `nx2/build.py` for a PS5 build because the unconditional `toolchain.ps5()` guard exits before the compiler is selected.

The build driver needs more than a compiler switch. Its current source and flag graph is split into independent job groups:

1. Lifted chunks and the generated A64 runtime are compiled with `flags()` plus `giant_flags()`. They include `a64.h`, so one PS5 `A64_HOST_BASE` definition must reach every one of these jobs.
2. The standalone host source set is assembled by globbing `host/*.c`. It excludes desktop files only for Linux, and it adds `host/posix/*.c` for Linux. A PS5 target must instead use an explicit allowlist/platform source set; otherwise desktop entry points, Windows compatibility glue, SDL display code, or POSIX shims can leak into the title.
3. The WebGPU renderer and Vulkan renderer have separate C++ job groups and separate flags. The existing PS5 RADV path is a static title link, not the desktop Vulkan DLL path. Its objects need `RELINK_PS5_RADV`, the PS5 SDK headers, and the same guest-base definition where they include the guest model.
4. The final link currently has only Linux shared-object and Windows executable branches. PS5 needs its own ELF/PIE link recipe, the pinned linker script, static RADV archive, SDK C++ runtimes, platform library, and the required local-symbol/weak-symbol treatment. The title conversion/signing and `FAKE00001` folder packaging are separate final steps.

A safe build-driver change should introduce a dedicated PS5 job plan rather than redefine `LINUX` as false and fall into the Windows branch. The PS5 plan must explicitly define:
- compiler and linker selection;
- per-group target flags and include paths;
- an allowlisted set of lifted/runtime/host/RADV sources;
- generated-code and runtime dependencies;
- static RADV and SDK link inputs;
- title conversion, signing, and folder packaging.

### Memory mapping path narrowed by the pinned SDK

The pinned SDK's `ps5_shm_map()` implementation provides a useful, concrete fixed-map sequence: it maps a slice of a 64 KiB-rounded direct-memory object at a requested guest address with `sceKernelMapDirectMemory(..., PS5_KERNEL_MAP_FIXED, ...)`. `ps5_shm_unmap(..., PS5_SHM_KEEP_RESERVED)` then restores a reservation at the same address. This confirms the mechanics of replacing a reserved virtual extent with direct-memory backing and restoring the hole on unmap.

The pinned RADV driver independently allocates direct memory for its own GPU resources with `sceKernelAllocateDirectMemory()`, maps it with `sceKernelMapDirectMemory()`, and releases it with `sceKernelMunmap()` plus `sceKernelReleaseDirectMemory()`. Its allocation unit is 64 KiB; its GPU-visible mapping checks use the driver's own address-window rules.

This still does not justify using `ps5_shm_create()` as ReLink's guest allocator:
- `ps5_shm_create()` allocates one backing object for the requested size, rather than providing an on-demand allocator for arbitrary guest extents.
- `ps5_shm_map()` exposes CPU protections only; RADV host-pointer import must still grant GPU read/write protection.
- The SDK helper is not documented or demonstrated as safe from a SIGSEGV handler.
- ReLink needs per-extent ownership, overlap-safe commits, zero-on-new-commit, decommit, and import-lifetime tracking across the 512 GiB guest window.

### Implementation decision

Do not remove the PS5 build guard yet. The next code change should be a dedicated PS5 build-plan skeleton that fails closed until the guest-memory backend and RADV archive are available, rather than a compiler-only toggle. The next memory implementation must build on fixed direct-memory views and explicit ownership metadata; first-touch mapping from a signal handler remains unproven and must not be assumed.

No source build, console test, or title package was produced by this audit.

## Pinned SDK allocator limits and ReLink bookkeeping

A closer read of the pinned SDK implementation narrows what can safely be reused:

- `ps5_shm_create(bytes)` makes one `sceKernelAllocateDirectMemory()` allocation rounded to 64 KiB. The `ps5_shm` object owns that single physical extent; it is not a suballocator, page table, or commit-on-demand arena.
- `ps5_shm_map()` accepts 16 KiB-aligned offsets and lengths and can map a slice at a fixed virtual address. Its mapping protection is CPU-only: `kernel_protection()` translates only `PS5_SHM_READ/WRITE/EXEC`. GPU access is a later, separate `sceKernelMprotect()` operation performed by the pinned RADV host-pointer import path.
- `ps5_shm_unmap(..., PS5_SHM_KEEP_RESERVED)` first calls `sceKernelMunmap()`, then calls `sceKernelReserveVirtualRange()` to restore the hole. If the second call fails, the view is gone and the virtual hole is not restored. A ReLink backend must handle this partial-failure state; it cannot treat unmap-plus-reserve as an atomic decommit.
- `ps5_vrange_reserve()` rounds to 16 KiB and calls the SDK placement helper. That helper tries the hint and a list of 4 GiB-spaced candidates, calling `sceKernelReserveVirtualRange()` with flags 0. It is a placement search, not an exact-address reservation API. ReLink's 16 TiB fixed base therefore needs a direct fixed-range reservation path whose exact kernel semantics are confirmed, with a strict returned-address equality check.
- `ps5_shm_destroy()` releases its direct-memory allocation without tracking whether views remain. The caller must enforce the documented lifetime rule: every view must be unmapped before destroy.

The current ReLink memory bookkeeping adds two further incompatibilities that must be handled explicitly:

- `hos_mapped_pages.h` tracks mapped state in 4 KiB units over the 512 GiB guest window. The SDK maps in 16 KiB pages. The PS5 backend must update four 4 KiB accounting entries for each successfully mapped 16 KiB page and must not report a partial map as committed.
- `hos_mem.c` has a separate `pages_used` bitmap limited to the first 4 GiB, while `A64_WRITE_TRACK` in the A64 runtime also has first-4-GiB assumptions. These are not allocator ownership records and cannot safely be repurposed to track 64 KiB direct-memory extents across the whole guest window.

### Implementation decision

Do not wrap `ps5_shm_create/map/unmap` in a nominal `hos_commit()` and call that a complete backend. The next implementation must introduce explicit metadata for:
- each 64 KiB direct-memory allocation and its 16 KiB guest-page views;
- reservation, mapped, GPU-granted, imported, and decommitted states;
- per-page and per-extent ownership/refcounts for overlapping commits;
- partial failures while mapping multiple pages or restoring reservations;
- RADV import holds and GPU completion before physical release.

The SDK source confirms the primitive calls and their granularity, but it does not establish recoverable first-touch signal behavior or signal-handler safety for allocation/mapping calls. Until that is proven, PS5 must use explicit commit points and fail closed on uncommitted guest access rather than adding an unverified fault handler. No allocator implementation or runtime behavior is claimed by this audit.

## Renderer dirty-page tracking is another PS5 integration gate

The renderer's guest write-watch path is not currently safe to enable for the full PS5 guest window:

- `gpu/render/rx_buffer.cpp` declares and scans `a64_dirty` as `1 << 27` bytes: one byte for each 4 KiB page in the 512 GiB guest window.
- `third_party/a64-kit/rt/a64_rt.c` actually defines `a64_dirty` as only `1 << 20` bytes, and `third_party/a64-kit/rt/a64.h` indexes marks after truncating guest addresses to 32 bits. This is a first-4-GiB bitmap, not a 512-GiB bitmap.
- `host/hos_mem.c:hos_mark_written()` likewise limits its page walk to `A64_DIRTY_PAGES`, whose current definition is the first 4 GiB.
- The renderer uses these marks in `collect_written_pages()` to decide which guest pages need upload/synchronization. Its non-soft-write-watch fallback is platform write-watch behavior (Windows `GetWriteWatch`); that is not a PS5 implementation.

Consequently, do not simply carry the desktop `A64_WRITE_TRACK` define into a PS5 build. Doing so would either leave guest writes above 4 GiB untracked or require changing the bitmap to a 128 MiB allocation and auditing every writer/reader and synchronization path. The renderer also needs a PS5-specific way to collect CPU-written pages that preserves its GPU coherency guarantees.

A future implementation should choose and document one explicit mechanism before enabling the renderer:
1. a correctly sized 512-GiB page bitmap (128 MiB) with 64-bit guest page indexing and all lifted/runtime/host writers marking it; or
2. a PS5-supported page-protection/write-fault mechanism whose fault and retry semantics are demonstrated, with a safe fallback for concurrent CPU and GPU access.

The first option has a material fixed-memory cost and requires checking every store/CAS/helper path. The second depends on platform behavior not established by the pinned SDK header declarations. Until one is implemented, the PS5 renderer cannot claim reliable incremental guest-memory uploads. This is separate from direct-memory allocation and Vulkan import lifetime.

## Vulkan import lifetime: what the current renderer guarantees, and what PS5 must preserve

The current Vulkan backend has a GPU-completion-aware lifetime model that a PS5 allocator must integrate with rather than replace with a simple CPU-side reference count:

- `gpu/vk/vk_memory.cpp` tracks live imported guest ranges in the `imported` pool map. `memory_guest_imported(lo, hi)` checks overlap while holding `pools_mu`, and also checks retired pools whose import nodes are retained.
- Pool/import retirement is tied to the renderer's `mem_completed` timeline value. Retired imports are destroyed only after their recorded value is complete; the release path then calls `release_uncovered(lo, hi)`.
- Guest address-region frees are reported through `memory_guest_release(lo, hi)`. That function queues ranges under `released_mu`, coalescing adjacent ranges where possible. `released_drain()` later processes the queue while the pool lock is held.
- `release_uncovered()` computes the portions not covered by live or GPU-pending imports before asking the host to give those guest pages back. This avoids unmapping memory still referenced by a submitted GPU workload.
- The host memory layer has a separate distinction between a freed guest page and a page that can actually be discarded: `FreeMemoryPages` and `hos_region_free` preserve/zero ranges reported as imported, while non-imported ranges can be discarded or decommitted.

Important platform limitation: the current `release_uncovered()` host decommit callback is guarded by `_WIN32`; `hos_region_release_gaps()` is also implemented only for the Windows backend. The Linux path retains its own discard behavior and does not provide a reusable PS5 direct-memory decommit implementation. The current renderer's pool/timeline bookkeeping is therefore a design reference, not a ready-made PS5 allocator bridge.

PS5 backend requirements derived from this lifecycle:

1. Keep each direct-memory extent alive while any mapped 16 KiB view or Vulkan import references it.
2. A host import reference must remain held through the GPU timeline value that retires the corresponding Vulkan allocation, not merely until the CPU finishes creating or destroying the Vulkan object.
3. When a guest range is freed, mark it logically free first; defer physical release for any extent with a live import or incomplete GPU timeline value.
4. On retirement, release only the now-uncovered 16 KiB views. A 64 KiB direct-memory allocation may be released only when all four constituent views are unmapped and no import holds any part of the extent.
5. Treat zeroing and decommit as distinct operations. If an import still references the extent, preserve the backing and zero only when the renderer's synchronization rules make that safe; never punch/unmap the backing out from under the GPU.
6. Keep the PS5 state manager's transitions transactional: if any kernel map/unmap/protection call fails, retain enough state to retry or roll back without claiming the extent was released.

This establishes the next safe implementation boundary: a standalone metadata/lifetime layer can track extent and view state, but the production adapter must receive explicit GPU-retirement notifications from the renderer before it can call `sceKernelReleaseDirectMemory`. No direct-memory release callback is wired to the existing renderer today.

## Standalone extent-state bookkeeping added

`host/ps5/memory_state.h` and `host/ps5/memory_state.c` now provide a platform-independent metadata layer for the physical/view lifetime model. It deliberately makes no SDK calls and is not yet connected to `hos_commit`, `hos_region_free`, or Vulkan.

The caller supplies a fixed metadata array; no heap allocation, mutex, or hidden global state is introduced. Each record represents one 64 KiB direct-memory extent and tracks its opaque backing handle, four 16 KiB CPU mapping bits, four GPU-grant bits, per-view import reference counts, and a pending-release flag.

The API enforces these transitions:

- A view can be marked mapped only once and only while its extent is not retiring.
- GPU permission can be recorded only for a mapped view. It must be revoked before unmapping.
- An import range must be nonempty and 16 KiB aligned. The entire range is validated before any reference count changes; every covered view must already be mapped and GPU-granted, and retiring extents reject new imports.
- Import release validates the entire range before decrementing any count, so an invalid or duplicate release cannot partially drop a hold.
- An extent can be forgotten only after it is marked for retirement and all four views are unmapped, all GPU grants are revoked, and all import holds are zero. The caller receives the backing handle only at that point.

This is intentionally a bookkeeping primitive, not a memory allocator or kernel adapter. In particular:

- The caller must invoke each transition only after its corresponding SDK operation succeeds; this module cannot roll back a failed kernel operation.
- The renderer must retain import holds until the GPU timeline confirms completion, then release those holds.
- The caller remains responsible for serializing concurrent access to the state object.
- No direct-memory allocation, fixed guest reservation, page mapping, GPU permission change, zeroing, decommit, or physical release is performed here.
- The fixed metadata capacity must be chosen by the future backend and exhaustion must fail closed.

The source is not yet included in a PS5 build source group and no production caller uses it. This creates a testable boundary for the next integration step without implying that PS5 guest memory is operational.

### Host-side state-transition tests

`host/ps5/memory_state_test.c` exercises the metadata API without a PS5 runtime. It covers map/grant/import/retire/release ordering, blocking unmap or GPU revocation while imports remain, atomic validation of multi-view import ranges, overflow/alignment rejection, duplicate extents, and metadata-capacity exhaustion. Run it on a development host with:

```sh
sh host/ps5/run-memory-state-tests.sh
```

The runner compiles the C implementation with C11, pedantic warnings, and warnings-as-errors in a temporary directory. It is a host-side unit test only; it does not validate SDK calls, GPU timeline integration, PS5 mapping semantics, or title startup. The PS5 preflight now requires both `memory_state.c` and `memory_state.h` as part of the eventual backend source set, while the actual `memory.c` adapter remains a separate missing prerequisite.

## Pinned PayloadSDK shared-memory API contract

The pinned PayloadSDK revision `b83202be73e930050e00de0f1b4d9ec46c0391bf` was inspected directly at:

- `platform/include/ps5platform/shm.h`
- `platform/src/shm.c`
- `platform/include/ps5platform/kernel.h`

The public adapter signatures are:

- `ps5_shm_create(size_t bytes, struct ps5_shm *shm)`
- `ps5_shm_destroy(struct ps5_shm *shm)`
- `ps5_shm_map(const struct ps5_shm *, size_t offset, size_t bytes, void *address, int protection, unsigned flags, void **view)`
- `ps5_shm_unmap(void *view, size_t bytes, unsigned flags)`
- `ps5_vrange_reserve(size_t bytes, void *hint, size_t alignment, void **base)`
- `ps5_vrange_release(void *base, size_t bytes)`

The platform kernel header declares `sceKernelAllocateDirectMemory`, `sceKernelReleaseDirectMemory`, `sceKernelMapDirectMemory`, `sceKernelReserveVirtualRange`, `sceKernelMunmap`, and `sceKernelMprotect`. Its declared direct-memory granularity is 64 KiB and page granularity is 16 KiB. GPU read/write protection bits are separate from the CPU mapping flags.

### Adapter implications

1. Prefer the SDK `ps5_shm` wrapper for physical extent allocation and view mapping instead of duplicating its direct-memory arithmetic.
2. The `ps5_shm` object records the direct-memory start and rounded allocation size, but does not track individual views or prevent premature destruction. ReLink's extent-state manager must remain the ownership authority.
3. `ps5_shm_map()` maps CPU protections and handles execute permission separately. GPU permission still requires an explicit protection operation and a successful result before ReLink records the GPU-granted state.
4. `ps5_shm_unmap(..., PS5_SHM_KEEP_RESERVED)` first unmaps the view and then attempts to reserve the hole again. If the reserve step fails, the original mapping is already gone and the hole may remain unreserved. The adapter must report this as a partial transition and preserve enough metadata for recovery; it must not pretend the operation was atomic.
5. `ps5_vrange_reserve()` delegates to placement search. Its interface accepts a hint and alignment, not an exact-address guarantee. It is therefore not sufficient evidence for reserving ReLink's fixed 512 GiB window at `A64_HOST_BASE`.
6. `ps5_shm_destroy()` calls `sceKernelReleaseDirectMemory()` without checking whether views remain. ReLink must call it only after all four 16 KiB views are unmapped, GPU permissions are revoked, and all Vulkan import references have retired after GPU completion.
7. The pinned `platform/src/shm.c` implementation also ignores the return value from `sceKernelReleaseDirectMemory()` and clears the `ps5_shm` object unconditionally. That makes `ps5_shm_destroy()` unsuitable for a fail-closed allocator: a kernel release failure becomes unobservable and the only local copy of the allocation handle is erased. The adapter may use `ps5_shm_create()` and `ps5_shm_map()`, but its release path should call the declared `sceKernelReleaseDirectMemory(shm.direct_start, shm.bytes)` directly, check the returned status, and clear/forget the object only after success. If release fails, retain the direct start, byte count, and extent metadata for retry. This is source-level reasoning from the pinned implementation, not a claim about console runtime behavior.
7. These declarations establish the pinned SDK's source-level interface, not successful runtime behavior on the user's console. No console probe, build, or runtime test was performed for this audit.

### Remaining adapter blockers

- A proven exact-address reservation path for the complete guest window.
- Recovery logic for the SDK's unmap-then-reserve partial failure.
- A production adapter that connects the SDK object and view handles to `Ps5MemState`.
- Renderer callbacks that release import references only after the GPU timeline reports completion.

Until these are implemented, the SDK calls must not be wired into guest allocation/free paths.
