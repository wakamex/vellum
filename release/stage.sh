#!/usr/bin/env bash
# Builds one release target and packs it with the license texts of everything compiled into it.
#   release/stage.sh linux|windows OUT_DIR
# Writes OUT_DIR/vellum-vVERSION-x86_64-linux.tar.gz or OUT_DIR/vellum-vVERSION-x86_64-windows.zip,
# VERSION being build.zig.zon's .version. Run from a clean checkout; zig 0.17 fetches the
# dependencies into zig-pkg/, where their license texts are read from.
set -euo pipefail
cd "$(dirname "$0")/.."

target=${1:?linux or windows}
out=$(mkdir -p "${2:?output directory}" && cd "$2" && pwd)
case $target in
    linux) zig_target=x86_64-linux-gnu.2.27 exe=vellum ext=tar.gz ;;
    windows) zig_target=x86_64-windows-gnu exe=vellum.exe ext=zip ;;
    *) echo "unknown target: $target" >&2; exit 1 ;;
esac
version=$(sed -n 's/^ *\.version = "\(.*\)",$/\1/p' build.zig.zon)
test -n "$version"
name=vellum-v$version-x86_64-$target

# A dependency's package folder, from the .hash that a build.zig.zon gives it.
package() {
    local hash
    hash=$(grep -A3 "^ *\.$2 = \.{" "$1" | sed -n 's/^ *\.hash = "\(.*\)",$/\1/p')
    test -n "$hash" && test -d "zig-pkg/$hash" || { echo "no package for $2 in $1" >&2; exit 1; }
    echo "zig-pkg/$hash"
}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
zig build -Doptimize=ReleaseSmall -Dtarget="$zig_target" -p "$work/prefix"

stage=$work/$name
mkdir -p "$stage/licenses"
cp "$work/prefix/bin/$exe" README.md LICENSE "$stage/"
[ "$target" = windows ] && cp tools/register-windows.ps1 "$stage/"
# What is compiled in: md4c, gesso with kb_text_shape and stb, and SDL (with its Wayland protocol code on Linux).
gesso=$(package build.zig.zon gesso)
sdl=$(package "$gesso/build.zig.zon" sdl)
cp vendor/md4c/LICENSE.md "$stage/licenses/md4c-LICENSE.md"
cp "$gesso/LICENSE" "$stage/licenses/gesso-LICENSE"
cp "$gesso/vendor/kb/LICENSE" "$stage/licenses/kb_text_shape-LICENSE"
cp "$gesso/vendor/stb/LICENSE" "$stage/licenses/stb-LICENSE"
mkdir "$stage/licenses/SDL"
cp -r "$sdl/LICENSE.txt" "$sdl/REUSE.toml" "$sdl/LICENSES" "$stage/licenses/SDL/"
if [ "$target" = linux ]; then
    linux_deps=$(package "$sdl/build.zig.zon" sdl_linux_deps)
    mkdir "$stage/licenses/SDL_linux_deps"
    cp -r "$linux_deps/LICENSE.txt" "$linux_deps/REUSE.toml" "$linux_deps/LICENSES" "$stage/licenses/SDL_linux_deps/"
fi

cd "$work"
rm -f "$out/$name.$ext"
if [ "$ext" = zip ]; then zip -qrX "$out/$name.$ext" "$name"; else tar -czf "$out/$name.$ext" "$name"; fi
echo "$out/$name.$ext"
