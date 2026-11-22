#!/usr/bin/env bash
# Starts the game you built with build-game.sh.
#
#   scripts/linux/relink-play.sh [<game folder>] [game options]
#
# The folder defaults to ~/Games/ReLink. The game's own settings are in the folder (relink_options.ini; Insert or F1 opens
# the same menu in the game, F11 switches full screen). The first start is long, once: the game prepares its shaders and
# builds its graphics pipelines for your driver's cache.
set -euo pipefail

case "${1:-}" in -h|--help) sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;; esac

folder="$HOME/Games/ReLink"
if [ $# -gt 0 ] && [ -d "$1" ]; then folder="$1"; shift; fi
if resolved="$(cd "$folder" 2>/dev/null && pwd)"; then
    folder="$resolved"
else
    echo "relink-play: no game folder at $folder: the game is not built yet. Run scripts/linux/build-game.sh --nup \"<your game file>\" first and wait for it to finish." >&2
    exit 1
fi

exe="$folder/relink"
[ -x "$exe" ] || exe="$folder/bin/relink"
[ -x "$exe" ] || { echo "relink-play: no relink executable in $folder: the build did not finish, or this is not a Linux game folder" >&2; exit 1; }
[ -f "$folder/image.bin" ] || { echo "relink-play: $folder has no image.bin: the build did not finish" >&2; exit 1; }
[ -f "$(dirname "$exe")/relink_vk.so" ] || { echo "relink-play: relink_vk.so (the renderer) is missing beside $exe" >&2; exit 1; }

cd "$folder"
exec "$exe" "$@"
