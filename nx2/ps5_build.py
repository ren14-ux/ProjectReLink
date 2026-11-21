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

    title_tool = os.path.join(vulkan_root, "build", "runtime-shim", "ps5-native-tool")
    if not os.path.isfile(title_tool):
        errors.append("missing PS5 title conversion/signing tool: " + title_tool)

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
