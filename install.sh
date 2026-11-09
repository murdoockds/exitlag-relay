#!/usr/bin/env bash
# Install exitlag-relay on a systemd-based Linux distribution.
#
#   sudo ./install.sh [--user NAME] [--prefix DIR] [--port N] [--venv]
#   sudo ./install.sh --uninstall [--prefix DIR]
#
# Run it from a release tarball (prebuilt) or from a source checkout after
# `make`. It installs:
#   PREFIX/bin/            exitlag-relayd, exitlag-relay (the app),
#                          exitlag-run, exitlag-wine, exitlag-tray
#   PREFIX/share/exitlag-relay/wine/   the nfapi.dll shim and its helpers
#   PREFIX/share/applications/exitlag-relay.desktop
#   /etc/systemd/system/exitlag-relay.service    (not enabled: the app starts it)
#   /etc/polkit-1/rules.d/50-exitlag-relay.rules (lets NAME start/stop it)
#   /etc/sysctl.d/90-exitlag-relay.conf, /etc/modules-load.d/exitlag-relay.conf
#
# --user     the desktop user who runs the app (default: the user behind sudo)
# --prefix   default /usr/local
# --port     UDP port between the daemon and the shim (default 27999)
# --venv     put PySide6 and dbus-fast in a private Python environment under
#            PREFIX/lib/exitlag-relay instead of using the system's packages
set -euo pipefail

prefix=/usr/local
user=${SUDO_USER-}
port=27999
venv=0
uninstall=0
while [ $# -gt 0 ]; do
  case $1 in
  --user) user=$2; shift ;;
  --prefix) prefix=$2; shift ;;
  --port) port=$2; shift ;;
  --venv) venv=1 ;;
  --uninstall) uninstall=1 ;;
  -h | --help) sed -n '2,23p' "$0"; exit 0 ;;
  *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root: sudo $0 $*"
command -v systemctl >/dev/null || die "systemd is required"

share=$prefix/share/exitlag-relay
manifest=$share/installed-files

if [ "$uninstall" = 1 ]; then
  [ -f "$manifest" ] || die "nothing installed under $prefix ($manifest is missing)"
  say "Stopping the relay service"
  systemctl disable --now exitlag-relay.service 2>/dev/null || true
  say "Removing installed files"
  while IFS= read -r f; do rm -f "$f"; done <"$manifest"
  rm -rf "$prefix/lib/exitlag-relay"
  rm -f "$manifest"
  rmdir "$share/wine" "$share" 2>/dev/null || true
  systemctl daemon-reload
  say "Done. ExitLag's Wine prefix (~/.local/share/exitlag-wine) and your games list (~/.config/exitlag-relay) were kept."
  exit 0
fi

[ -n "$user" ] && [ "$user" != root ] || die "pass the desktop user: sudo $0 --user NAME"
home=$(getent passwd "$user" | cut -d: -f6) || die "no such user: $user"
wine_home=$home/.local/share/exitlag-wine

src=$(cd "$(dirname "$0")" && pwd)
for f in build/exitlag-relayd build/wine/nfapi.dll build/wine/idle.exe build/wine/nfextlag-wine.sys; do
  [ -f "$src/$f" ] || die "$f is missing: run 'make' first (needs Go and zig), or use a release tarball"
done

: >"$src/.manifest.tmp"
put() { # put MODE SRC DEST
  install -D -m "$1" "$2" "$3"
  echo "$3" >>"$src/.manifest.tmp"
}
fill() { # fill SRC DEST: copy a template, filling in @VARS@
  sed -e "s|@BINDIR@|$prefix/bin|g" -e "s|@USER@|$user|g" \
    -e "s|@LISTEN@|127.0.0.1:$port|g" -e "s|@WINE_HOME@|$wine_home|g" "$1" >"$src/.fill.tmp"
  put 644 "$src/.fill.tmp" "$2"
  rm -f "$src/.fill.tmp"
}

say "Installing to $prefix for user $user"
put 755 "$src/build/exitlag-relayd" "$prefix/bin/exitlag-relayd"
put 755 "$src/gui/exitlag_relay.py" "$prefix/bin/exitlag-relay"
put 755 "$src/gui/exitlag_run.py" "$prefix/bin/exitlag-run"
put 755 "$src/wine/exitlag-tray.py" "$prefix/bin/exitlag-tray"
for f in nfapi.dll idle.exe nfextlag-wine.sys; do
  put 644 "$src/build/wine/$f" "$share/wine/$f"
done
# exitlag-wine finds the shim in ../share/exitlag-relay/wine; the port is
# baked in so it matches the service.
sed -e "s|^host=\${EXITLAG_RELAY_HOST:-127.0.0.1:27999}|host=\${EXITLAG_RELAY_HOST:-127.0.0.1:$port}|" \
  "$src/wine/exitlag-wine.sh" >"$src/.fill.tmp"
put 755 "$src/.fill.tmp" "$prefix/bin/exitlag-wine"
rm -f "$src/.fill.tmp"
put 644 "$src/dist/exitlag-relay.desktop" "$prefix/share/applications/exitlag-relay.desktop"

say "Installing the system service, polkit rule, sysctl and module settings"
fill "$src/dist/exitlag-relay.service.in" /etc/systemd/system/exitlag-relay.service
fill "$src/dist/exitlag-relay.rules.in" /etc/polkit-1/rules.d/50-exitlag-relay.rules
put 644 "$src/dist/exitlag-relay-sysctl.conf" /etc/sysctl.d/90-exitlag-relay.conf
put 644 "$src/dist/exitlag-relay-modules.conf" /etc/modules-load.d/exitlag-relay.conf
mkdir -p "$share"
mv "$src/.manifest.tmp" "$manifest"

if [ "$venv" = 1 ]; then
  say "Creating a Python environment with PySide6 and dbus-fast (about 200 MB)"
  py=$prefix/lib/exitlag-relay/venv
  python3 -m venv "$py"
  "$py/bin/pip" install --quiet --upgrade pip
  "$py/bin/pip" install --quiet PySide6 dbus-fast
  for f in exitlag-relay exitlag-run exitlag-tray; do
    sed -i "1s|.*|#!$py/bin/python3|" "$prefix/bin/$f"
  done
fi

systemctl daemon-reload
modprobe tun 2>/dev/null || true
modprobe nft_socket 2>/dev/null || true
# If the relay was already running, pick up the new binary and settings.
systemctl try-restart exitlag-relay.service || true

# Report what's still missing; nothing below is fatal.
missing=()
for c in ip nft jq pgrep; do
  command -v "$c" >/dev/null || missing+=("$c")
done
command -v umu-run >/dev/null || missing+=(umu-run)
python=$(head -1 "$prefix/bin/exitlag-relay" | sed 's/^#!//; s/^\/usr\/bin\/env //')
$python -c 'import PySide6.QtWidgets' 2>/dev/null || missing+=(PySide6)
$python -c 'import dbus_fast' 2>/dev/null || missing+=(dbus-fast)
[ "$(stat -fc %T /sys/fs/cgroup 2>/dev/null)" = cgroup2fs ] ||
  warn "/sys/fs/cgroup isn't cgroup v2; exitlag-relayd needs the unified hierarchy"

if [ ${#missing[@]} -gt 0 ]; then
  warn "still missing: ${missing[*]}"
  . /etc/os-release 2>/dev/null || true
  case " ${ID-} ${ID_LIKE-} " in
  *" arch "*) echo "  sudo pacman -S --needed iproute2 nftables jq procps-ng python-pyside6 python-dbus-fast umu-launcher" ;;
  *" fedora "*) echo "  sudo dnf install iproute nftables jq procps-ng python3-pyside6 python3-dbus-fast umu-launcher" ;;
  *" debian "* | *" ubuntu "*) echo "  sudo apt install iproute2 nftables jq procps python3-dbus-fast python3-pyside6.qtwidgets python3-pyside6.qtnetwork" ;;
  *" suse "* | *" opensuse "*) echo "  sudo zypper install iproute2 nftables jq procps python3-pyside6 python3-dbus-fast" ;;
  esac
  echo "  Package names differ between releases. If PySide6 or dbus-fast aren't packaged,"
  echo "  rerun with --venv. umu-launcher: https://github.com/Open-Wine-Components/umu-launcher/releases"
fi

say "Installed. Open \"ExitLag Relay\" from your app menu; it asks for ExitLag's Windows installer the first time."
