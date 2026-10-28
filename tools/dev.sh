#!/bin/sh
# Works on vellum with each saved change on screen in a few seconds, where you were:
#   tools/dev.sh [FILE.md] [vellum's options]
# zig build --watch rebuilds a dev build (-Ddev) into zig-out/dev at every saved source file, and the
# running vellum starts again as each new build, with its file, scroll, zoom and editor as they were.
# Sizes and colours change without a rebuild through a style file (--style FILE, or vellum.style
# beside the program), read again when it changes.
set -eu
cd "$(dirname "$0")/.."
zig build -Ddev -p zig-out/dev
zig build -Ddev -p zig-out/dev --watch --debounce 100 > /dev/null &
watcher=$!
trap 'kill $watcher 2> /dev/null' EXIT INT TERM
"$PWD/zig-out/dev/bin/vellum" "$@"
# (each build that replaces it is started by the one before, so this waits on the first only)
while pgrep -f "^$PWD/zig-out/dev/bin/vellum" > /dev/null; do sleep 1; done
