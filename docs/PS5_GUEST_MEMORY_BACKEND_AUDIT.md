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

## Decision

Do not add a speculative `hos_mem_ps5.c` yet. The SDK provides enough primitives to design a direct-memory-backed arena, but the exact 512 GiB reservation, overlapping-commit bookkeeping, zeroing/decommit rules, and GPU-visible mapping/import contract must be established first. The PS5 build guard remains correct until these are implemented together.

No PS5 compilation or console test is claimed.
