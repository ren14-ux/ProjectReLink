# PS5 guest-window reservation probe

This is a standalone diagnostic ELF. It is deliberately separate from the
ReLink PS5 application target, which remains guarded until its host services
and RADV title link are integrated.

## Build

Use the exact SDK revision pinned in `../../deps/ps5-payload-sdk.commit`:

```sh
export PS5_PAYLOAD_SDK=/absolute/path/to/installed/ps5-payload-sdk
make -C tools/ps5 clean all
```

The Makefile rejects a missing or mismatched SDK revision. The resulting
`relink-guest-window-probe.elf` can be transferred to the console using the
ELF loader appropriate to the installed jailbreak environment. Its output
reports each reservation attempt and cleanup result.

The diagnostic only reserves and releases virtual address ranges. It does not
allocate direct memory, map guest pages, or modify ReLink's runtime state.
A PASS establishes only that the tested reservation layout worked in this
process at that time; it does not yet prove that the full emulator memory
backend can use that layout.

No ReLink PS5 build guard is changed by this target.
