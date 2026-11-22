# Project ReLink on Linux (Ubuntu / Debian)

The game is built on your own PC from your own copy of it, as on Windows. No game file is in this repository.
Tested on Ubuntu 24.04 (x86-64, clang 18, SDL2 2.30, FFmpeg 6.1).

## You need

- an x86-64 PC with **AVX2**, 16 GB of RAM or more (less: use `--jobs 1`), about 60 GB free while building
- a GPU and driver with **Vulkan 1.3 and mesh shaders** (Mesa RADV, or NVIDIA's driver)
- your own game dump, as a `.nup` file or as `romfs` + `exefs` folders (the version named in the readme)

## Steps

```bash
cd ProjectReLink                                   # the folder that holds scripts/ and nx2/ (run everything from here)
scripts/linux/install-deps.sh                      # apt packages, Python environment, Vulkan headers (asks for sudo once)
scripts/linux/build-game.sh --nup ~/Downloads/game.nup   # builds into ~/Games/ReLink (--out <folder> to change)
scripts/linux/relink-play.sh                       # starts it
```

- `build-game.sh --check --nup <file>` only checks that the file is the right game and version.
- `build-game.sh --update <game folder>` rebuilds an existing folder with the current tools.
- `RELINK_JOBS=<n>` limits parallel compiles. `RELINK_HOME` moves the tool files (default `~/.local/share/relink`).
- Other distributions: install the equivalents of the packages listed in `scripts/linux/install-deps.sh`, then run it with `--no-apt`.
  Vulkan-Headers 1.4.317 or newer is needed (Ubuntu 24.04's own 1.3.275 is too old); `setup-cpm.sh` fetches the pinned version.

## Checking the Linux build without a game

`scripts/linux/check.sh` builds the renderer (`relink_vk.so`), compiles the host runtime against `host/posix`, and runs the
tests that need no game. CI runs it (`.github/workflows/linux.yml`).

## What differs from Windows

- Window, input, audio: SDL2 (`host/posix/display_sdl.c`); Win32 calls are provided by `host/posix/win32_posix.c`.
- The Insert menu's GDI drawing is `host/posix/gdi_posix.c`.
- `GlobalMemoryStatusEx` (the renderer's low-memory guard for making pipelines ahead) is answered from `/proc/meminfo` (`host/posix/windows.h`).
- The first start is long: shaders and pipelines are prepared for your driver.

## Not verified

The full game build and play could not be run here because they need a game dump. The renderer builds and loads, the host runtime
compiles with no unresolved Win32 symbol, and the game-free tests pass. Please report anything that fails once you build with a real dump.
