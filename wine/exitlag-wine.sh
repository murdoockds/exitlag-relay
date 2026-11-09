#!/usr/bin/env bash
# Run ExitLag under Wine (GE-Proton via umu) with the nfapi.dll shim, so
# Linux games' traffic reaches it through exitlag-relayd.
#
#   exitlag-wine setup INSTALLER   create the prefix and install ExitLag
#   exitlag-wine [run]             start ExitLag (re-applies the shim first)
#   exitlag-wine stop              close ExitLag
#   exitlag-wine exec PROGRAM...   run something else in the prefix (only
#                                  while ExitLag is closed)
#
# Environment:
#   EXITLAG_WINE_HOME   state directory (default ~/.local/share/exitlag-wine)
#   EXITLAG_RELAY_HOST  exitlag-relayd's address (default 127.0.0.1:27999)
#   EXITLAG_WINE_SHIM   directory with nfapi.dll, idle.exe, nfextlag-wine.sys
#   PROTONPATH          Proton build for umu (default GE-Proton, the latest)
#   QT_SCREEN_SCALE_FACTORS  ExitLag's per-monitor scale ("model=1.75;..."),
#                       taken from niri's outputs when unset
#   PROTON_WAYLAND_MONITOR   Wine's primary output (default: niri's focused one)
#   EXITLAG_TRAY        tray helper (default: exitlag-tray from PATH; "" to use
#                       Wine's own tray item); EXITLAG_TRAY_PORT its UDP port
set -euo pipefail

base=${EXITLAG_WINE_HOME:-$HOME/.local/share/exitlag-wine}
host=${EXITLAG_RELAY_HOST:-127.0.0.1:27999}
here=$(dirname "$(readlink -f "$0")")
# Installed: PREFIX/bin/exitlag-wine next to PREFIX/share/exitlag-relay/wine.
# Source checkout: wine/exitlag-wine.sh next to wine/nfshim/build.
if [ -n "${EXITLAG_WINE_SHIM-}" ]; then
  shim=$EXITLAG_WINE_SHIM
elif [ -f "$here/../share/exitlag-relay/wine/nfapi.dll" ]; then
  shim=$here/../share/exitlag-relay/wine
else
  shim=$here/nfshim/build
fi

export WINEPREFIX=$base/prefix
export GAMEID=umu-exitlag
export PROTONPATH=${PROTONPATH:-GE-Proton}
export PROTON_ENABLE_WAYLAND=${PROTON_ENABLE_WAYLAND:-1}
export PROTON_LOG=1 PROTON_LOG_DIR=$base/logs
# Wine reports 96 DPI on every monitor and hands ExitLag (Qt, per-monitor
# DPI aware) the native resolution, so on a scaled output its UI came out
# tiny. Qt can scale per screen, and its screen names are the monitors' EDID
# model names, which niri reports along with each output's scale.
# Proton's Wine drops QT_* from Windows programs' environment; the nfapi.dll
# shim turns EXITLAG_QT_* back into QT_* inside ExitLag.
qt_scales=${QT_SCREEN_SCALE_FACTORS-}
if [ -z "$qt_scales" ] && command -v niri >/dev/null; then
  qt_scales=$(niri msg --json outputs 2>/dev/null | jq -r '
    [.[] | select(.logical != null and .model != null)
         | "\(.model | gsub("^\\s+|\\s+$"; ""))=\(.logical.scale)"] | join(";")' 2>/dev/null) || true
fi
[ -n "$qt_scales" ] && export EXITLAG_QT_SCREEN_SCALE_FACTORS=$qt_scales
# Qt sizes a new window for the primary screen, and niri opens it on the
# focused output; make those the same, or ExitLag starts at the wrong scale
# when they differ. (Moving it between outputs later is handled by Qt.)
if [ -z "${PROTON_WAYLAND_MONITOR-}" ] && command -v niri >/dev/null; then
  PROTON_WAYLAND_MONITOR=$(niri msg --json focused-output 2>/dev/null | jq -r '.name // empty' 2>/dev/null) || true
  [ -n "$PROTON_WAYLAND_MONITOR" ] && export PROTON_WAYLAND_MONITOR
fi
# winemenubuilder would rewrite the host's file associations to open in this
# prefix (radmin-vpn-linux learned this the hard way).
export WINEDLLOVERRIDES="winemenubuilder.exe=d${WINEDLLOVERRIDES:+;$WINEDLLOVERRIDES}"

drive=$WINEPREFIX/drive_c
app=$drive/"Program Files"/ExitLag
relay=$drive/Relay

umu() {
  # umu resolves a bare program name against the working directory first.
  cd "$base"
  if command -v umu-run >/dev/null; then
    umu-run "$@"
  elif command -v nix-shell >/dev/null; then
    nix-shell -p umu-launcher --run "$(printf '%q ' umu-run "$@")"
  else
    echo "exitlag-wine: umu-run not found; install umu-launcher (https://github.com/Open-Wine-Components/umu-launcher)" >&2
    exit 1
  fi
}

# Each umu-run gets its own container with a private /tmp, so it can't reach
# a running prefix's wineserver and would start a second one on the same
# prefix, which corrupts the registry. Anything but ExitLag itself therefore
# only runs while the prefix is idle.
# pids_in_prefix [NAME]: this prefix's processes named NAME (default
# wineserver), or with "" all of its Windows processes (Wine gives those a
# C:\... command line; this script and umu carry WINEPREFIX too).
pids_in_prefix() {
  local p pf
  for p in $(if [ -n "${1-wineserver}" ]; then pgrep -u "$(id -u)" -x "${1:-wineserver}"; else pgrep -u "$(id -u)"; fi); do
    pf=$(tr '\0' '\n' 2>/dev/null <"/proc/$p/environ" | sed -n 's/^WINEPREFIX=//p')
    case "${pf%/}" in "$WINEPREFIX" | "$WINEPREFIX"/*) ;; *) continue ;; esac
    if [ -z "${1-wineserver}" ]; then
      tr '\0' '\n' 2>/dev/null <"/proc/$p/cmdline" | head -1 | grep -qE '^[A-Za-z]:\\' || continue
    fi
    echo "$p"
  done
}

require_idle() {
  if [ -n "$(pids_in_prefix)" ]; then
    echo "exitlag-wine: the prefix is in use (ExitLag running?); run 'exitlag-wine stop' first" >&2
    exit 1
  fi
}

wait_idle() {
  local i
  for i in $(seq 1 60); do
    [ -z "$(pids_in_prefix)" ] && return 0
    sleep 1
  done
  return 1
}

# ExitLag only uses its WFP path while the "nfextlag" driver service runs.
# The service points at our stub (Wine can load it; it does nothing); the
# real nfextlag.sys stays in place because ExitLag checks its version.
# ExitLag's updater re-registers the real driver, so this is redone before
# every start, offline-checked first so the prefix isn't booted for nothing.
service_ok() {
  grep -a -A12 '^\[System\\\\ControlSet001\\\\Services\\\\nfextlag\]' "$WINEPREFIX/system.reg" 2>/dev/null |
    sed '/^$/q' | grep -q 'nfextlag-wine\.sys'
}

apply_shim() {
  [ -f "$app/ExitLag.exe" ] || {
    echo "exitlag-wine: ExitLag isn't installed; run 'exitlag-wine setup INSTALLER'" >&2
    exit 1
  }
  mkdir -p "$relay"
  # nfapi.dll: keep ExitLag's own copy as .orig (refreshed after updates),
  # never an older build of the shim.
  if ! cmp -s "$shim/nfapi.dll" "$app/nfapi.dll"; then
    grep -qa 'nfapi shim loaded' "$app/nfapi.dll" || cp -f "$app/nfapi.dll" "$app/nfapi.dll.orig"
    install -m 644 "$shim/nfapi.dll" "$app/nfapi.dll"
  fi
  install -m 644 "$shim/idle.exe" "$relay/idle.exe"
  install -m 644 "$shim/nfextlag-wine.sys" "$drive/windows/system32/drivers/nfextlag-wine.sys"
  # nfshim.ini: the daemon's address. The game comes from the daemon.
  local ini=$relay/nfshim.ini
  if [ -f "$ini" ]; then
    grep -v '^host=' "$ini" >"$ini.new" || true
  else
    : >"$ini.new"
  fi
  { echo "host=$host"; cat "$ini.new"; } >"$ini" && rm -f "$ini.new"

  if ! service_ok; then
    local k='HKLM\SYSTEM\CurrentControlSet\Services\nfextlag'
    printf '%s\r\n' \
      "reg add $k /v Type /t REG_DWORD /d 1 /f" \
      "reg add $k /v Start /t REG_DWORD /d 1 /f" \
      "reg add $k /v ErrorControl /t REG_DWORD /d 1 /f" \
      "reg add $k /v ImagePath /t REG_EXPAND_SZ /d system32\\drivers\\nfextlag-wine.sys /f" \
      "reg add $k /v DisplayName /t REG_SZ /d nfextlag /f" \
      >"$relay/service.bat"
    echo "exitlag-wine: pointing the nfextlag service at the stub driver"
    umu 'C:\windows\system32\cmd.exe' /c 'C:\Relay\service.bat' >"$base/logs/service.log" 2>&1
    wait_idle || true
    service_ok || {
      echo "exitlag-wine: couldn't register the nfextlag service (see $base/logs/service.log)" >&2
      exit 1
    }
  fi
}

cmd=${1:-run}
[ $# -gt 0 ] && shift
mkdir -p "$base/prefix" "$base/logs"

case $cmd in
setup)
  installer=${1:?usage: exitlag-wine setup SetupExitLag-*.exe}
  installer=$(readlink -f "$installer")
  require_idle
  # The installer can't install the NDIS driver under Wine and says so; the
  # WFP path is the one we use. It ends by starting ExitLag, which we close:
  # umu only returns once every process in the prefix has exited.
  echo "exitlag-wine: installing ExitLag (a warning about the NDIS driver is expected; click OK)"
  umu "$installer" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART '/LOG=C:\exitlag-setup.log' \
    >"$base/logs/setup.log" 2>&1 &
  umu_pid=$!
  while kill -0 "$umu_pid" 2>/dev/null; do
    if [ -n "$(pids_in_prefix ExitLag.exe)" ]; then
      sleep 2
      kill $(pids_in_prefix ExitLag.exe) 2>/dev/null || true
    fi
    sleep 1
  done
  wait_idle || true
  apply_shim
  echo "exitlag-wine: done; start it with 'exitlag-wine'"
  ;;
run)
  if [ -n "$(pids_in_prefix ExitLag.exe)" ]; then
    echo "exitlag-wine: ExitLag is already running"
    exit 0
  fi
  require_idle
  apply_shim
  # The tray icon goes through exitlag-tray (a native StatusNotifierItem
  # whose menu the desktop's tray draws); without it, Wine's tray item.
  tray=${EXITLAG_TRAY-$(command -v exitlag-tray || true)}
  if [ -n "$tray" ] && ! "$tray" --check 2>/dev/null; then
    echo "exitlag-wine: $tray can't start (python3 dbus-fast missing?); using Wine's tray item" >&2
    tray=
  fi
  if [ -n "$tray" ]; then
    export EXITLAG_TRAY_PORT=${EXITLAG_TRAY_PORT:-27997}
    "$tray" "$EXITLAG_TRAY_PORT" >"$base/logs/tray.log" 2>&1 &
    tray_pid=$!
    trap 'kill $tray_pid 2>/dev/null' EXIT
  fi
  umu "$app/ExitLag.exe"
  ;;
stop)
  pids=$(pids_in_prefix ExitLag.exe)
  [ -n "$pids" ] && kill $pids
  sleep 3
  # ExitLagPmService (a Windows service) can outlive the container that
  # started it, keeping the prefix busy.
  pids=$(pids_in_prefix "")
  [ -n "$pids" ] && kill $pids 2>/dev/null
  wait_idle || true
  ;;
exec)
  require_idle
  umu "$@"
  ;;
*)
  sed -n '2,15p' "$0" >&2
  exit 2
  ;;
esac
