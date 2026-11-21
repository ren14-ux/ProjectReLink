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

## Decision

Do not add a speculative `hos_mem_ps5.c` yet. The SDK provides enough primitives to design a direct-memory-backed arena, but the exact 512 GiB reservation, overlapping-commit bookkeeping, zeroing/decommit rules, and GPU-visible mapping/import contract must be established first. The PS5 build guard remains correct until these are implemented together.

No PS5 compilation or console test is claimed.
