# PS5 guest-memory diagnostic title

The intended deliverable is a PS5 title folder, not an ELF:

```
dist/FAKE00001/
├── eboot.bin
├── sce_sys/
│   └── param.json
└── sce_module/
    └── libc.prx
```

## Pinned build inputs

- PS5 Payload SDK revision: `b83202be73e930050e00de0f1b4d9ec46c0391bf`
- PS5_Vulkan native title toolchain revision: `7f492b4c6f67a4e8c8c67a4272cfeef22abea9cc`

The title build reuses the reference project's native startup object, PIE linker
script, native ELF-to-module converter, and reproducible libc runtime builder.
This avoids treating a generic SDK compiler output as a runnable PS5 title.

## Prepare and build

Clone/check out the pinned PS5_Vulkan revision and prepare its native
dependencies (the pinned SDK must be installed under its `.deps/native`
directory). Then:

```sh
export PS5_VULKAN_ROOT=/absolute/path/to/PS5_Vulkan
make -C tools/ps5
```

The script builds the host-side native title tool/runtime if absent, compiles
the reservation diagnostic with the pinned PS5 target wrapper, links the
application CRT with the SDK import libraries, converts it to a PS5 module,
signs the executable container, and assembles `dist/FAKE00001/`.

Intermediate ELF files are kept under `build/ps5-guest-probe/`; they are not
the deliverable. The title ID is a development placeholder and must be checked
against the console's installed title IDs before deployment.

The app only tests virtual-address reservation and release. It does not test
direct-memory allocation, GPU mapping, or the complete ReLink memory backend.
The main ReLink PS5 build guard remains enabled.
