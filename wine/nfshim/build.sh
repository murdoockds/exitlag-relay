#!/usr/bin/env bash
# Build nfapi.dll, idle.exe and the stub nfextlag-wine.sys with zig's MinGW target.
set -euo pipefail
cd "$(dirname "$0")"
out=${1:-build}
mkdir -p "$out"
export ZIG_GLOBAL_CACHE_DIR=${ZIG_GLOBAL_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/zig}
cc="zig cc -target x86_64-windows-gnu -O2 -Wall -Wextra -Wno-unused-parameter"
build() {
  $cc -shared -o "$out/nfapi.dll" nfapi.c wmifix.c vblank.c dpifix.c tray.c profile.c windiag.c ipv4only.c nfapi.rc nfapi.def -lws2_32 -lole32 -loleaut32 -lwinmm -lgdi32 -liphlpapi
  $cc -Wl,--subsystem,windows -o "$out/idle.exe" idle.c
  $cc -shared -nostdlib -Wl,--subsystem,native -Wl,--entry,DriverEntry -o "$out/nfextlag-wine.sys" nfextlag.c
  rm -f "$out"/*.lib "$out"/*.pdb
}
if command -v zig >/dev/null; then build
elif command -v nix >/dev/null; then exec nix shell nixpkgs#zig -c "$0" "$@"
else
  echo "build.sh: zig is needed to cross-compile the shim (https://ziglang.org/download/)" >&2
  exit 1
fi
