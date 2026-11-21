# Vulkan Feature Inventory for the PS5 Port

Source reviewed: `gpu/vk/vk_main.cpp`, `gpu/vk/vk_memory.cpp`, `gpu/vk/vk_api.h`. This is a source inventory, not a claim that any of these APIs are available on PS5.

## Confirmed startup gates

The device initialization in `gpu/vk/vk_main.cpp` rejects a device unless all of the following are available:

- Vulkan 1.3 at instance initialization.
- Dynamic rendering (`VkPhysicalDeviceVulkan13Features::dynamicRendering`).
- Synchronization2 (`VkPhysicalDeviceVulkan13Features::synchronization2`).
- Timeline semaphores (`VkPhysicalDeviceVulkan12Features::timelineSemaphore`).

These are hard gates in the current implementation. Removing the checks without replacing the operations is not a valid port.

## Mesh/task shader path

The renderer enables native mesh/task shaders only when both `meshShader` and `taskShader` are supported. The source contains a fallback that runs task and mesh programs as compute shaders and draws generated vertices through ordinary graphics pipelines. That fallback requires buffer device address support.

Therefore, lack of native mesh shaders alone is not necessarily fatal, but lack of the compute path's buffer-device-address requirement is another blocker. The fallback also needs shader and pipeline behavior validated; it is not proof of PS5 compatibility.

## Memory and guest address handling

`gpu/vk/vk_memory.cpp` describes an external-host-memory import path using `VK_EXT_external_memory_host`. The code also contains a mirror-buffer route for guest pools. The renderer uses Vulkan buffer device addresses when mapping mirrors. A native backend would need to preserve guest GPU address translation, guest CPU/GPU visibility, write tracking, cache/coherency rules, and lifetime synchronization.

The presence of a mirror path is useful, but it still depends on Vulkan resource creation, copies, barriers, and buffer device addresses. It is not a drop-in PS5 memory implementation.

## Other optional/conditional features

The inspected initialization code also probes or conditionally uses:

- Graphics pipeline library fast linking.
- Subgroup size control and 32-lane subgroup behavior.
- Push descriptors and multiple descriptor-related features.
- Robust buffer access / robust buffer access 2.
- Sparse binding and sparse image residency.
- Cooperative matrix support.
- Extended dynamic state 3.
- Conditional rendering, vertex attribute divisor, custom border color, depth clip control, and other optional features.

These should be classified by actual call sites as required, optional, or performance-only before designing a reduced feature profile. The above list is not an assertion that all are mandatory.

## Engineering consequence

The first viable PS5 milestone is not "make Vulkan 1.3 check pass." It is a platform feasibility spike that answers:

1. Which graphics API and driver entry points can a Wobkot homebrew process call on firmware 7.61?
2. Is there an actual Vulkan loader/driver for that environment? If yes, what Vulkan version, extensions, and feature bits does it expose?
3. If not, is there an established supported translation layer from Vulkan to the PS5 native graphics API, or must the renderer be ported to a native backend?
4. Can the PS5 host allocate/map GPU-visible memory and present frames with the required synchronization semantics?

Until those answers are evidenced by the target SDK headers/libraries or a minimal sample, a Vulkan compatibility shim would be speculative. The implementation should not weaken the existing feature gates or claim a successful PS5 build.
