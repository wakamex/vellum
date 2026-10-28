#!/bin/sh
# Puts a dev build of vellum on a Windows machine in place of the installed one, which every dev
# build there then starts again as, each window with its file, scroll, zoom and editor as they were:
#   tools/windows-dev.sh [--watch] [SSH_HOST]   (default host: windows)
# The build goes to %LOCALAPPDATA%\Programs\vellum, where tools/register-windows.ps1 installs vellum
# and opens .md files with it. A running vellum.exe cannot be overwritten but can be renamed, so the
# new file takes its name and the running one moves aside, to be deleted at the next push. Release
# builds do not watch their program file: the first dev build starts when a file is next opened.
# With --watch it stays, rebuilding at every saved source file and putting each build in place.
set -eu
cd "$(dirname "$0")/.."
watch=false
if [ "${1:-}" = --watch ]; then watch=true; shift; fi
host=${1:-windows}
out=zig-out/windows-dev
exe=$out/bin/vellum.exe
folder=AppData/Local/Programs/vellum

push() {
    ssh -o BatchMode=yes "$host" "New-Item -ItemType Directory -Force $folder > \$null"
    scp -q "$exe" "$host:$folder/vellum.exe.new"
    ssh -o BatchMode=yes "$host" 'powershell -NoProfile -Command -' <<PS | tr -d '\0'
\$d = "\$env:LOCALAPPDATA\Programs\vellum"
Get-ChildItem "\$d\*.old" | Remove-Item -ErrorAction SilentlyContinue
if (Test-Path "\$d\vellum.exe") { Rename-Item "\$d\vellum.exe" "vellum-\$([DateTime]::Now.Ticks).old" }
Rename-Item "\$d\vellum.exe.new" vellum.exe
"vellum: dev build in place in \$d, windows running: \$(@(Get-Process vellum -ErrorAction SilentlyContinue).Count)"
PS
}

build() { zig build -Ddev -Dtarget=x86_64-windows-gnu -p $out "$@"; }
build
push
$watch || exit 0
build --watch --debounce 100 > /dev/null &
watcher=$!
trap 'kill $watcher 2> /dev/null' EXIT INT TERM
last=$(stat -c %Y.%s "$exe")
while sleep 1; do
    now=$(stat -c %Y.%s "$exe" 2> /dev/null) || continue
    if [ "$now" != "$last" ]; then last=$now; push; fi
done
