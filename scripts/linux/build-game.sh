#!/usr/bin/env bash
# Builds the game for Linux, on this PC, from your own copy of it (nx2/make_game.py with the Linux settings).
#
#   scripts/linux/build-game.sh --nup <game.nup> [--out <folder>] [--language en-US] [--jobs <n>]
#   scripts/linux/build-game.sh --romfs <dir> --exefs <dir> [--out <folder>]
#   scripts/linux/build-game.sh --check --nup <game.nup>       only: is this the right game and version?
#   scripts/linux/build-game.sh --update <game folder>         again, with these tools, from the files the folder keeps
#
# --out defaults to ~/Games/ReLink. Every other option is make_game.py's (--work, --log, --language, ...).
# It takes 10 to 45 minutes or much longer on a small PC, needs about 60 GB free while it runs and a lot of memory
# (RELINK_JOBS=<n> or --jobs <n> limits the parallel compiles; on a PC with less than 16 GB of RAM use 1 or 2).
# No game file is in this repository: the version it needs is named in the readme.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"

say() { printf 'build-game: %s\n' "$*"; }
die() { printf 'build-game: %s\n' "$*" >&2; exit 1; }

case "${1:-}" in -h|--help|"") sed -n '2,13p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;; esac

PY="$(relink_python)" || exit 1
[ -d "$RELINK_CPM/vulkanheaders" ] || die "the Vulkan header packages are missing: run scripts/linux/install-deps.sh"
command -v clang >/dev/null && command -v ld.lld >/dev/null || die "clang and lld are missing: run scripts/linux/install-deps.sh"

# (a path that is not there: say so here, in words, instead of letting make_game.py fail on it)
prev=""
for arg in "$@"; do
    case "$prev" in
        --nup|--romfs|--exefs|--update)
            [ -e "$arg" ] || die "no such file or folder: $arg
  --nup wants the path of YOUR game file (for example ~/Downloads/game.nup); '/path/to/game.nup' in the docs is only a placeholder." ;;
    esac
    prev="$arg"
done

check_only=0 have_out=0 have_update=0
for arg in "$@"; do
    case "$arg" in
        --check) check_only=1 ;;
        --out|--out=*) have_out=1 ;;
        --update) have_update=1 ;;
    esac
done

if [ "$check_only" = 0 ]; then
    # (the same CPU the build targets: nx2/host_cpu.py. Without AVX2 the renderer's ASTC decoder and the game's vector code stop at the start)
    grep -qw avx2 /proc/cpuinfo || die "this processor has no AVX2: ReLink cannot run on it"
    mem_gb=$(awk '/^MemTotal:/ { printf "%d", $2 / 1048576 }' /proc/meminfo)
    [ "$mem_gb" -ge 15 ] || say "warning: $mem_gb GB of RAM: the build wants 16 GB or more. If a compile is killed, try --jobs 1"
    target="$HOME/Games/ReLink"
    prev=""
    for arg in "$@"; do
        if [ "$prev" = --out ]; then target="$arg"; fi
        case "$arg" in --out=*) target="${arg#--out=}" ;; esac
        prev="$arg"
    done
    probe="$target"; while [ ! -d "$probe" ] && [ "$probe" != / ]; do probe="$(dirname "$probe")"; done
    free_gb=$(df -Pk "$probe" | awk 'NR==2 { printf "%d", $4 / 1048576 }')
    [ "$have_update" = 1 ] || [ "$free_gb" -ge 60 ] || say "warning: only $free_gb GB free where the game goes: the build wants about 60 GB while it runs"
    if command -v vulkaninfo >/dev/null; then
        info="$(vulkaninfo --summary 2>/dev/null || true)"
        if [ -z "$info" ]; then
            say "warning: vulkaninfo found no Vulkan device; the game needs a GPU with Vulkan 1.3 and mesh shaders (docs/linux.md)"
        elif ! printf '%s\n' "$info" | grep -Eq 'apiVersion *= *1\.[34]'; then
            say "warning: no Vulkan 1.3 device was listed by vulkaninfo; the game needs Vulkan 1.3 with mesh shaders"
        fi
    fi
fi

args=("$@")
[ "$check_only" = 1 ] || [ "$have_out" = 1 ] || [ "$have_update" = 1 ] || args+=(--out "$HOME/Games/ReLink")
# (RELINK_JOBS is read by make_game.py itself)

say "$("$PY" -I "$RELINK_ROOT/nx2/version.py" 2>/dev/null || echo ReLink): building with $(clang --version | head -1)"
exec "$PY" -I -u "$RELINK_ROOT/nx2/make_game.py" "${args[@]}"
