"""Fail-closed prerequisites for the future in-process PS5 title build.

This module intentionally does not compile anything. The desktop build graph is
not a PS5 build graph; until the PS5 host services and static RADV link exist,
RX_TARGET=ps5 must stop before desktop target selection.
"""
import os


def preflight(root, toolchain):
    """Report the concrete missing pieces without falling through to Linux/Windows."""
    root = os.path.abspath(root)
    errors = []

    try:
        sdk = toolchain.ps5_sdk_root()
    except SystemExit as exc:
        errors.append(str(exc))
        sdk = ""

    host = os.path.join(root, "host", "ps5")
    required_host = (
        "entry.c",       # title lifecycle and common game boot handoff
        "memory.c",      # exact guest reservation, commit/decommit, first-touch policy
        "memory_state.c", # 4 KiB page / 16 KiB view / 64 KiB extent lifetime bookkeeping
        "memory_state.h",
        "memory_backend.c", # serialized mapping, import, and release coordinator
        "memory_backend.h",
        "sdk_extent_pool.c", # direct-memory allocation and fixed guest-view mapping
        "sdk_extent_pool.h",
        "guest_window.c",  # exact virtual-address reservation and ownership
        "guest_window.h",
        "thread.c",      # PS5 thread, TLS, wait and synchronization services
        "filesystem.c",  # title RomFS and writable save/cache paths
        "input.c",       # DualSense input and vibration
        "audio.c",       # PS5 audio sink
    )
    for name in required_host:
        path = os.path.join(host, name)
        if not os.path.isfile(path):
            errors.append("missing PS5 host service: " + os.path.relpath(path, root))

    vulkan_root = os.path.abspath(os.environ.get(
        "PS5_VULKAN_ROOT", os.path.join(root, "..", "PS5_Vulkan")))
    radv = os.environ.get(
        "RELINK_PS5_RADV_ARCHIVE",
        os.path.join(vulkan_root, ".deps", "native", "radv-release", "lib",
                     "libvulkan_radeon.ps5.a"))
    if not os.path.isfile(radv):
        errors.append("missing pinned static RADV archive: " + radv)

    linker_script = os.path.join(vulkan_root, "tooling", "psbc", "ps5-pie-unwind.ld")
    if not os.path.isfile(linker_script):
        errors.append("missing PS5 PIE linker script: " + linker_script)

    package_script = os.path.join(root, "tools", "ps5", "build-title.sh")
    title_params = os.path.join(root, "tools", "ps5", "param.json")
    native_wrapper = os.path.join(vulkan_root, "tooling", "prospero-clang18")
    native_crt = os.path.join(vulkan_root, "tooling", "native", "app_crt.cpp")
    native_symbols = os.path.join(vulkan_root, "tooling", "native", "app-symbols.map")
    for path, label in (
        (package_script, "PS5 title packaging script"),
        (title_params, "PS5 title param.json"),
        (native_wrapper, "PS5 native compiler wrapper"),
        (native_crt, "PS5 title startup object source"),
        (native_symbols, "PS5 title export map"),
    ):
        if not os.path.isfile(path):
            errors.append("missing " + label + ": " + path)

    # build-title.sh rebuilds the native converter/runtime when their checked
    # outputs are absent, so the converter executable itself is not a preflight
    # requirement. Its inputs and the package script are.

    if errors:
        raise SystemExit(
            "RX_TARGET=ps5 is not buildable; the dedicated title build is incomplete:\n  - "
            + "\n  - ".join(errors)
            + "\nNo desktop source set or linker path was selected."
        )

    # These prerequisites are necessary, not sufficient. The normal build
    # graph still needs its PS5-specific compile groups and final title link.
    raise SystemExit(
        "RX_TARGET=ps5 prerequisites are present, but nx2/build.py has not yet "
        "integrated the PS5 compile groups, in-process RADV objects, SDK link "
        "inputs, and FAKE title packaging. No build was attempted."
    )
