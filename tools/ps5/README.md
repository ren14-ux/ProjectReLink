# PS5 guest-memory diagnostic title

The deliverable is a PS5 title directory, **not an ELF file**:

```
dist/PPSA00000/
├── eboot.bin
├── sce_sys/
│   └── param.json
└── sce_module/
    └── libc.prx
```

## Build requirements

- `PS5_PAYLOAD_SDK`: installed SDK at the exact revision in `deps/ps5-payload-sdk.commit`
- `PS5_NATIVE_TOOL`: executable `ps5-native-tool` from the PS5_Vulkan native title toolchain
- `PS5_LIBC_PRX`: compatible `libc.prx` runtime module for this title

Build:

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export PS5_NATIVE_TOOL=/path/to/ps5-native-tool
export PS5_LIBC_PRX=/path/to/libc.prx
make -C tools/ps5
```

Intermediate ELF files stay under `build/ps5-guest-probe/`; the installable
folder is `dist/PPSA00000/`. The title ID is a development placeholder and
must be checked against the user's installed title IDs before deployment.

The app only tests virtual-address reservation and release. It does not yet
test direct-memory allocation, GPU mapping, or the complete ReLink memory
backend. The main ReLink PS5 build guard remains enabled.
