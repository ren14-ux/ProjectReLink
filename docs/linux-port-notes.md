# Linux (Ubuntu) port: what was done

A port of [pixl27/ProjectReLink](https://github.com/pixl27/ProjectReLink) (GPL, see `LICENSE`) to Ubuntu, based on upstream
commit `34efe84`. Usage is in [linux.md](linux.md). This file records the work.

## Findings

- The project is a static-recompilation toolchain (Python + C/C++) that builds the game on the player's PC from their own dump.
  No game data is in the repository.
- Upstream already had the main Linux pieces: an SDL2 window/input/audio layer (`host/posix/display_sdl.c`), a Win32-on-POSIX
  shim (`host/posix/`), and a Linux path in `nx2/build.py` / `nx2/make_game.py` (ELF `relink` + `relink_vk.so`).
  What was missing was a way to set up and run it on Ubuntu, and a few gaps found by compiling against current Ubuntu.

## Changes

### Added
- `scripts/linux/install-deps.sh`: installs the apt packages (clang 18, lld, SDL2, ALSA, Vulkan, FFmpeg, dav1d, spirv-tools, ...),
  creates a Python venv with the build's modules (lz4, numpy, capstone, unicorn 2.1.x, zstandard, pillow), and runs `setup-cpm.sh`.
- `scripts/linux/setup-cpm.sh`: builds the header-package layout `nx2/build.py` reads (`RX_CPM`): Vulkan-Headers v1.4.317 from
  Khronos (Ubuntu's 1.3.275 is too old: the renderer uses `VK_EXT_shader_float8` and maximal reconvergence), plus Ubuntu's
  spirv-headers and Vulkan Memory Allocator.
- `scripts/linux/build-game.sh`: wrapper for `nx2/make_game.py` with Linux defaults (output `~/Games/ReLink`), checks for AVX2,
  RAM, free disk and a Vulkan device, and a clear message when a given path does not exist.
- `scripts/linux/relink-play.sh`: starts the built game, with clear errors if the build is missing or unfinished.
- `scripts/linux/check.sh`: game-free verification: builds `relink_vk.so`, checks it loads with `RTLD_NOW` and exports the entry
  points `host/rx_backend.c` looks up, compiles all 43 host files against the lift's generated headers and links them (fails on any
  Win32 symbol the POSIX layer lacks), then runs 26 tests that need no game.
- `scripts/linux/env.sh`: shared settings.
- `.github/workflows/linux.yml`: runs install + check on `ubuntu-24.04`.
- `docs/linux.md`, this file.

### Changed
- `host/posix/windows.h`: added `MEMORYSTATUSEX` and `GlobalMemoryStatusEx`, answered from `/proc/meminfo`. The renderer's
  `vk_draw.cpp` calls it to stop making pipelines ahead when commit is low; it did not compile on Linux without it.

### Investigated and not needed
- Earlier drafts also added region clipping to the GDI shim (`CreateRectRgn`, `SelectClipRgn`) and `-include cmath` in
  `nx2/build.py`; upstream fixed both itself in the meantime, so they were dropped when rebasing.

## Verification (Ubuntu 24.04, x86-64, clang 18.1.3)
- `check.sh` passes from a clean state: renderer builds (7.8 MB `relink_vk.so`), host runtime links with no unresolved Win32 call,
  26 game-free tests pass.
- `install-deps.sh` verified from an empty `RELINK_HOME`.
- Full game build and play: reported working by the owner on their own machine, with their own dump.

## Known issues
- `nx2/test_vk_linux_presentation.py` fails on unmodified upstream (it asserts source text of `fsr_start()` that has since moved);
  it is left out of `check.sh`.
- `test_pgo_rt_linux.py` needs LLVM 19+'s `llvm-profdata`; Ubuntu 24.04 ships 18.
- Tests needing the game, a Windows GPU or a Windows DLL build are not run by `check.sh`.
