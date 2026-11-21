# PS5 Port Assessment (Option A: Preserve the Vulkan Renderer)

Status: source-level assessment only. No PS5 SDK build or console validation has been performed.

## Scope

Target: original PS5, firmware 7.61, Wobkot homebrew environment. The objective is to determine whether Project ReLink can retain its existing NVN replay and Vulkan renderer behind a PS5-compatible platform layer. This document deliberately does not claim that a Vulkan loader or Vulkan 1.3 implementation is available in the target environment.

## Confirmed source architecture

- `host/rx_backend.c` selects the Vulkan backend or the in-process WebGPU renderer. The Vulkan backend is loaded as a separate module; the WebGPU path is the default fallback.
- `gpu/vk/vk_api.h` defines the host/backend ABI. It passes guest-memory access, GPU-pool information, input, timing, and NVN texture-layout services to the renderer.
- `gpu/vk/vk_main.cpp` loads Vulkan dynamically and creates the Vulkan instance/device and presentation path.
- `gpu/vk/vk_memory.cpp` implements guest-memory exposure to the GPU, including a path based on `VK_EXT_external_memory_host` and a separate mirrored-buffer path.
- `nx2/build.py` has Windows and Linux build paths. The Linux path is still a desktop POSIX host path; it is not a PS5 target.

## Concrete blockers

1. **Runtime and loader** — `vk_main.cpp` checks for Vulkan 1.3 and sets the application API version to 1.3. Merely lowering this check would not make the renderer compatible; later API calls and feature structures must also be audited.
2. **Device feature requirements** — the renderer's comments and initialization path identify mesh shaders, host-memory import, and graphics pipeline libraries as relevant capabilities. The exact mandatory versus optional feature set must be extracted from device creation and pipeline setup before any fallback is designed.
3. **Memory model** — `vk_memory.cpp` is built around Vulkan buffers, device addresses, external host-memory import, and mirrored guest-memory pools. A PS5-native memory mapping cannot be substituted by changing a feature flag. It needs an explicit mapping/copy/coherency design.
4. **Presentation** — the current Vulkan startup uses desktop surface/swapchain integration. PS5 display output and frame presentation need a separate platform implementation if the PS5 SDK exposes an appropriate interface.
5. **Host portability** — several host and Vulkan files include Windows headers or call Win32 loader/window functions. The existing Linux compatibility layer is not proof of PS5 compatibility.
6. **Shader execution** — mesh/task shader use and the generated SPIR-V capabilities must be inventoried. A compute fallback is only viable if it preserves the shader semantics and all required pipeline behavior.

## Important distinction: WebGPU fallback

The repository includes an in-process WebGPU renderer under `gpu/render`, and the host can select it when the Vulkan module is unavailable. This is a useful alternate code path to study, but it does not by itself provide a PS5 implementation: its native WebGPU runtime, adapter backend, window/surface integration, and build dependencies would also need to run on the console.

## Recommended implementation order

1. Add a PS5 platform target boundary without changing PC defaults.
2. Identify the actual PS5 homebrew graphics and display APIs available to this build environment, including the exact SDK headers/libraries and supported memory/presentation calls.
3. Produce a Vulkan capability/API usage inventory from `gpu/vk`: required core version, extensions, feature bits, entry points, shader stages, external-memory paths, device-address use, synchronization, and swapchain calls.
4. Decide using that evidence whether Option A can be implemented as a Vulkan compatibility layer. If no compatible Vulkan runtime exists, stop trying to make the Vulkan loader start and evaluate a native PS5 renderer backend instead.
5. Only then implement memory, command submission, shader, and presentation adapters in small independently buildable changes.

## Out of scope until the platform boundary is proven

- Lowering the Vulkan 1.3 version check as a speculative workaround.
- Disabling extension checks without replacing the functionality they enable.
- Rewriting the full renderer before the PS5 graphics API and required feature set are known.
- Claiming a successful build or console test without the PS5 SDK/toolchain and a real target run.

## Next required technical input

The port needs the PS5 homebrew SDK/toolchain actually used by the Wobkot environment (compiler, headers, libraries, linker setup, and a minimal graphics sample/API reference). Without those artifacts, source inspection can map dependencies but cannot safely implement or validate a PS5 graphics backend.
