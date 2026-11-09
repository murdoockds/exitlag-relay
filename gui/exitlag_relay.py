#!/usr/bin/env python3
"""ExitLag Relay: Linux control panel for playing games through ExitLag.

Pick a game, turn the relay on, and the game's UDP traffic goes through
ExitLag running under Wine (exitlag-wine), where the nfapi.dll shim hands it
to ExitLag as the game's Windows process. Closing the window keeps it running
in the tray; Exit (tray menu) turns the relay off, closes ExitLag and stops
the relay service, which this app starts when it opens.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from PySide6.QtCore import QObject, QPointF, Qt, QTimer, Signal
from PySide6.QtGui import QAction, QColor, QIcon, QImage, QPainter, QPen
from PySide6.QtNetwork import QLocalServer, QLocalSocket
from PySide6.QtWidgets import (
    QApplication, QComboBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout, QFrame, QGridLayout,
    QGroupBox, QHBoxLayout, QLabel, QLineEdit, QListWidget, QMenu, QMessageBox, QPushButton,
    QStyle, QSystemTrayIcon, QVBoxLayout, QWidget,
)

APP_ID = "exitlag-relay"
CONTROL = os.environ.get("EXITLAG_RELAY_CONTROL", "/run/exitlag-relay/control.sock")
CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "exitlag-relay"
GAMES_FILE = CONFIG_DIR / "games.json"
SERVICE = "exitlag-relay.service"
WINE_HOME = Path(os.environ.get("EXITLAG_WINE_HOME", Path.home() / ".local/share/exitlag-wine"))


DEFAULT_GAMES = [
    {"name": "Counter-Strike 2", "linux": ["cs2"], "windows": "cs2.exe"},
    {"name": "Dota 2", "linux": ["dota2"], "windows": "dota2.exe"},
    {"name": "Apex Legends (Proton)", "linux": ["r5apex.exe", "r5apex_dx12.exe"], "windows": "r5apex.exe"},
    # Vesktop on Linux; ExitLag only knows the official client, Discord.exe.
    {"name": "Discord (Vesktop)", "linux": ["scope:vesktop"], "windows": "Discord.exe"},
]


# ---------------------------------------------------------------- backends

def daemon(req):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(2)
        s.connect(CONTROL)
        s.sendall(json.dumps(req).encode() + b"\n")
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    resp = json.loads(buf)
    if not resp.get("ok"):
        raise RuntimeError(resp.get("error", "daemon error"))
    return resp.get("status") or {}


def service(verb):
    """Start/stop the relay daemon (polkit lets our user do this without a password)."""
    r = subprocess.run(["systemctl", "--no-ask-password", verb, SERVICE],
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(f"couldn't {verb} the relay service: {r.stderr.strip()}")


def ensure_service():
    try:
        return daemon({"cmd": "status"})
    except (OSError, ValueError, RuntimeError):
        pass
    service("start")
    for _ in range(50):
        try:
            return daemon({"cmd": "status"})
        except (OSError, ValueError, RuntimeError):
            time.sleep(0.2)
    raise RuntimeError("the relay service didn't come up (journalctl -u " + SERVICE + ")")


def wine_pids():
    """ExitLag.exe processes running in exitlag-wine's prefix."""
    prefix = str(WINE_HOME / "prefix")
    pids = []
    for d in Path("/proc").iterdir():
        if not d.name.isdigit():
            continue
        try:
            if (d / "comm").read_text().strip() != "ExitLag.exe":
                continue
            env = (d / "environ").read_bytes().split(b"\0")
        except OSError:
            continue
        for kv in env:
            if kv.startswith(b"WINEPREFIX=") and kv[11:].decode(errors="replace").startswith(prefix):
                pids.append(int(d.name))
    return pids


def exitlag_installed():
    return (WINE_HOME / "prefix/drive_c/Program Files/ExitLag/ExitLag.exe").exists()


def wine_state():
    if not exitlag_installed():
        return "not installed"
    return "running" if wine_pids() else "shut off"


def require_exitlag_wine():
    if not shutil.which("exitlag-wine"):
        raise RuntimeError("exitlag-wine isn't installed (see the README)")


def install_exitlag_wine(installer):
    """Create the Wine prefix and run ExitLag's installer in it (takes a while:
    umu downloads GE-Proton the first time)."""
    require_exitlag_wine()
    (WINE_HOME / "logs").mkdir(parents=True, exist_ok=True)
    r = subprocess.run(["exitlag-wine", "setup", installer], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True)
    if r.returncode != 0 or not exitlag_installed():
        raise RuntimeError("ExitLag setup failed: " + (r.stderr.strip().splitlines() or ["see "
                           + str(WINE_HOME / "logs/setup.log")])[-1])


def start_exitlag_wine():
    require_exitlag_wine()
    (WINE_HOME / "logs").mkdir(parents=True, exist_ok=True)
    log = open(WINE_HOME / "logs/exitlag-wine.out", "ab")
    subprocess.Popen(["exitlag-wine", "run"], start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=log, stderr=log)


def open_exitlag_wine():
    """Bring ExitLag's window forward (niri), or start ExitLag."""
    pids = set(wine_pids())
    if not pids:
        start_exitlag_wine()
        return
    try:
        windows = json.loads(subprocess.run(["niri", "msg", "--json", "windows"], capture_output=True,
                                            text=True, timeout=5).stdout)
        for w in windows:
            if w.get("pid") in pids:
                subprocess.run(["niri", "msg", "action", "focus-window", "--id", str(w["id"])], timeout=5)
                return
    except (OSError, ValueError, subprocess.TimeoutExpired):
        pass


def stop_exitlag_wine():
    subprocess.run(["exitlag-wine", "stop"], capture_output=True, timeout=90)




# ---------------------------------------------------------------- detection

# Programs that use UDP but aren't games.
NOT_GAMES = {
    "steam", "steamwebhelper", "pipewire", "pipewire-pulse", "wireplumber", "avahi-daemon",
    "xwayland", "xwayland-satel", "chrome", "chromium", "firefox", "librewolf", "brave",
    "com.google.chrome", "org.mozilla.firefox", "librewolf.desktop",
    "spotify", "kdeconnectd", "syncthing", "telegram-desktop",
    "exitlag-relay", "wineserver", "services.exe", "winedevice.exe",
    "plugplay.exe", "explorer.exe", "rpcss.exe", "svchost.exe", "steam.exe", "steamwebhelper.",
}
STEAM_ROOTS = [Path.home() / ".local/share/Steam", Path.home() / ".steam/steam",
               Path.home() / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]


# Process names shared by many apps: match these by systemd scope instead.
GENERIC = {"electron", "chrome", "java", "node", "python3", "python", "mono", "dotnet"}
# Apps whose ExitLag name isn't simply "<name>.exe".
KNOWN_EXE = {"vesktop": "Discord.exe", "discord": "Discord.exe", "webcord": "Discord.exe",
             "legcord": "Discord.exe",
             # macOS client under Darling
             "leagueoflegends": "League of Legends.exe"}


def scope_app(cgroup_path):
    """'…/app-niri-vesktop-3809.scope' -> 'vesktop' (None if not an app scope)."""
    last = cgroup_path.rstrip("/").rsplit("/", 1)[-1]
    m = re.match(r"app-(?:niri-|flatpak-|gnome-|kde-)?(.+?)(?:-\d+)?\.scope$", last)
    return m.group(1) if m else None


def udp_inodes():
    inodes = set()
    for f in ("/proc/net/udp", "/proc/net/udp6"):
        try:
            for line in Path(f).read_text().splitlines()[1:]:
                parts = line.split()
                if len(parts) > 9:
                    inodes.add(parts[9])
        except OSError:
            pass
    return inodes


def steam_app_name(appid):
    for root in STEAM_ROOTS:
        libs = [root / "steamapps"]
        try:
            vdf = (root / "steamapps/libraryfolders.vdf").read_text()
            libs += [Path(l.split('"')[3]) / "steamapps" for l in vdf.splitlines() if '"path"' in l]
        except (OSError, IndexError):
            pass
        for lib in libs:
            try:
                for line in (lib / f"appmanifest_{appid}.acf").read_text().splitlines():
                    if '"name"' in line:
                        return line.split('"')[3]
            except (OSError, IndexError):
                continue
    return None


def running_game_candidates():
    """User processes with UDP sockets open, most game-like first."""
    inodes, uid, out = udp_inodes(), os.getuid(), []
    for d in Path("/proc").iterdir():
        if not d.name.isdigit():
            continue
        try:
            if d.stat().st_uid != uid:
                continue
            comm = (d / "comm").read_text().strip()
            argv0 = (d / "cmdline").read_bytes().split(b"\0")[0].decode(errors="replace")
            socks = {os.readlink(f)[8:-1] for f in (d / "fd").iterdir()
                     if os.readlink(f).startswith("socket:[")}
        except OSError:
            continue
        if not socks & inodes:
            continue
        base = argv0.replace("\\", "/").rsplit("/", 1)[-1]
        if comm.lower() in NOT_GAMES or base.lower() in NOT_GAMES:
            continue
        try:
            cgroup = (d / "cgroup").read_text().strip().removeprefix("0::")
        except OSError:
            cgroup = ""
        app = scope_app(cgroup)
        if app and app.lower() in NOT_GAMES:
            continue
        env = {}
        try:
            for kv in (d / "environ").read_bytes().split(b"\0"):
                k, _, v = kv.partition(b"=")
                if k in (b"SteamAppId", b"STEAM_COMPAT_APP_ID", b"BOTTLE_NAME", b"WINEPREFIX"):
                    env[k.decode()] = v.decode(errors="replace")
        except OSError:
            pass
        is_exe = base.lower().endswith(".exe")
        # Darling runs every macOS program through its loader, so comm is
        # "mldr" for all of them; argv[0] is the program's macOS path, e.g.
        # /Applications/…/LeagueOfLegends.app/Contents/MacOS/LeagueofLegends.
        darling = comm == "mldr"
        appid = env.get("SteamAppId") or env.get("STEAM_COMPAT_APP_ID")
        if darling:
            source, name = "Darling", None
        elif appid and appid != "0":
            source, name = "Steam", steam_app_name(appid)
        elif is_exe or "WINEPREFIX" in env or "BOTTLE_NAME" in env:
            source, name = "Bottles" if "BOTTLE_NAME" in env else "Wine", None
        else:
            source, name = "native", None
        if is_exe:
            linux, windows, name = base, base, name or base[:-4]
        elif darling:
            # The daemon matches the basename of argv[0].
            linux = base
            windows = KNOWN_EXE.get(base.lower(), base + ".exe")
            name = windows[:-4] if base.lower() in KNOWN_EXE else base
        elif comm.lower() in GENERIC and app:
            linux, name = "scope:" + app, name or app
            windows = KNOWN_EXE.get(app.lower(), app + ".exe")
        else:
            linux, name = comm, name or comm
            windows = KNOWN_EXE.get(comm.lower(), comm + ".exe")
        out.append({"pid": int(d.name), "source": source, "name": name,
                    "linux": linux, "windows": windows})
    rank = {"Steam": 0, "Wine": 1, "Bottles": 1, "Darling": 1, "native": 2}
    # one entry per game: the oldest process (lowest PID) of each name
    seen, uniq = set(), []
    for c in sorted(out, key=lambda c: (rank[c["source"]], c["pid"])):
        if c["linux"] not in seen:
            seen.add(c["linux"])
            uniq.append(c)
    return uniq


class DetectDialog(QDialog):
    def __init__(self, parent):
        super().__init__(parent)
        self.setWindowTitle("Detect a running game")
        self.choice = None
        self.list = QListWidget()
        info = QLabel("Start the game (online, e.g. in a lobby), then pick it here. "
                      "Listed: your programs that currently have UDP sockets open.")
        info.setWordWrap(True)
        refresh = QPushButton("Refresh")
        refresh.clicked.connect(self.fill)
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        self.list.itemDoubleClicked.connect(lambda _: self.accept())
        layout = QVBoxLayout(self)
        layout.addWidget(info)
        layout.addWidget(self.list)
        row = QHBoxLayout()
        row.addWidget(refresh)
        row.addStretch(1)
        row.addWidget(buttons)
        layout.addLayout(row)
        layout.setContentsMargins(18, 16, 18, 14)
        layout.setSpacing(10)
        info.setObjectName("hint")
        self.setFixedSize(560, 400)
        self.fill()

    def fill(self):
        self.list.clear()
        self.cands = running_game_candidates()
        for c in self.cands:
            self.list.addItem(f"{c['name']}  —  {c['source']}, process {c['linux']} (pid {c['pid']})")
        if self.cands:
            self.list.setCurrentRow(0)
        else:
            self.list.addItem("No programs with UDP sockets found. Is the game running and online?")

    def accept(self):
        row = self.list.currentRow()
        if 0 <= row < len(self.cands):
            self.choice = self.cands[row]
        super().accept()


# ---------------------------------------------------------------- games

def load_games():
    try:
        data = json.loads(GAMES_FILE.read_text())
        return data.get("games") or DEFAULT_GAMES, data.get("selected")
    except (OSError, ValueError):
        return [dict(g) for g in DEFAULT_GAMES], None


def save_games(games, selected):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    GAMES_FILE.write_text(json.dumps({"selected": selected, "games": games}, indent=2) + "\n")


def split_list(text):
    return [t.strip() for t in text.split(",") if t.strip()]


class GameDialog(QDialog):
    def __init__(self, parent, game=None):
        super().__init__(parent)
        self.setWindowTitle("Edit game" if game else "Add game")
        game = game or {}
        self.name = QLineEdit(game.get("name", ""))
        self.linux = QLineEdit(", ".join(game.get("linux", [])))
        self.linux.setPlaceholderText("cs2  ·  r5apex.exe (Proton/Wine)  ·  scope:vesktop  ·  cmd:text")
        self.linux.setToolTip("Process name, or the .exe name for Proton/Wine/Bottles games.\n"
                              "scope:NAME matches every process of an app started as app-NAME-….scope\n"
                              "(use it for Electron apps such as Vesktop, whose processes are all 'electron').\n"
                              "cmd:TEXT matches text anywhere in the command line.")
        self.windows = QLineEdit(game.get("windows", ""))
        self.windows.setPlaceholderText("cs2.exe   (the name ExitLag knows the game by)")
        self.prefixes = QLineEdit(", ".join(game.get("prefixes", [])))
        self.prefixes.setPlaceholderText("optional: extra destination prefixes")
        self.setFixedWidth(560)
        detect = QPushButton("Detect from running game…")
        detect.setCursor(Qt.PointingHandCursor)
        detect.clicked.connect(self.detect)
        hint = label("The Linux side picks which of your processes are relayed; the Windows "
                     ".exe is the name ExitLag sees (e.g. Vesktop → Discord.exe).", "hint")
        hint.setWordWrap(True)
        form = QFormLayout()
        form.setSpacing(10)
        form.setLabelAlignment(Qt.AlignRight | Qt.AlignVCenter)
        form.addRow("Name", self.name)
        form.addRow("Linux processes", self.linux)
        form.addRow("Windows .exe", self.windows)
        form.addRow("Extra prefixes", self.prefixes)
        buttons = QDialogButtonBox(QDialogButtonBox.Save | QDialogButtonBox.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(20, 18, 20, 16)
        layout.setSpacing(12)
        layout.addWidget(detect)
        layout.addLayout(form)
        layout.addWidget(hint)
        layout.addWidget(buttons)

    def detect(self):
        d = DetectDialog(self)
        if d.exec() and d.choice:
            c = d.choice
            if not self.name.text().strip():
                self.name.setText(c["name"])
            self.linux.setText(c["linux"])
            self.windows.setText(c["windows"])

    def game(self):
        g = {"name": self.name.text().strip(), "linux": split_list(self.linux.text()),
             "windows": self.windows.text().strip()}
        if split_list(self.prefixes.text()):
            g["prefixes"] = split_list(self.prefixes.text())
        return g

    def accept(self):
        g = self.game()
        if not g["name"] or not g["windows"].lower().endswith(".exe") or not (g["linux"] or g.get("prefixes")):
            QMessageBox.warning(self, "Incomplete", "Give a name, a Windows .exe and at least one Linux process name.")
            return
        super().accept()


# ---------------------------------------------------------------- look

ACCENT = "#e5383b"


def chevron_icon():
    """A down-chevron PNG for the game picker (stylesheets need an image file;
    drawn with QPainter so it doesn't depend on Qt's SVG plugin)."""
    path = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / APP_ID / "chevron.png"
    img = QImage(48, 48, QImage.Format_ARGB32)  # 4x the 12 px it's shown at: sharp on HiDPI
    img.fill(Qt.transparent)
    p = QPainter(img)
    p.setRenderHint(QPainter.Antialiasing)
    p.setPen(QPen(QColor("#8a9099"), 6, Qt.SolidLine, Qt.RoundCap, Qt.RoundJoin))
    p.drawPolyline([QPointF(10, 18), QPointF(24, 32), QPointF(38, 18)])
    p.end()
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        img.save(str(path))
    except OSError:
        pass
    return path.as_posix()


CHEVRON = chevron_icon()
STYLE = f"""
* {{ font-size: 10pt; }}
QWidget {{ background: #121418; color: #e7e9ec; }}
QDialog {{ background: #16191e; }}
QLabel {{ background: transparent; }}
QLabel#title {{ font-size: 16pt; font-weight: 700; }}
QLabel#subtitle, QLabel#hint, QLabel#cardTitle, QLabel#section {{ color: #8a9099; }}
QLabel#section, QLabel#cardTitle {{ font-size: 8pt; font-weight: 700; letter-spacing: 1px; }}
QLabel#cardValue {{ font-size: 11pt; font-weight: 600; }}
QLabel#cardSub {{ color: #8a9099; font-size: 8.5pt; }}
QLabel#pill {{ border-radius: 8px; padding: 4px 11px; font-weight: 700; font-size: 8.5pt; }}
QLabel#pill[state="on"] {{ background: #173826; color: #4ade80; }}
QLabel#pill[state="wait"] {{ background: #3a3015; color: #facc15; }}
QLabel#pill[state="off"] {{ background: #23262c; color: #8a9099; }}
QLabel#dot {{ border-radius: 4px; min-width: 8px; max-width: 8px; min-height: 8px; max-height: 8px; }}
QLabel#dot[state="on"] {{ background: #4ade80; }}
QLabel#dot[state="wait"] {{ background: #facc15; }}
QLabel#dot[state="off"] {{ background: #4b5059; }}
QLabel#dot[state="bad"] {{ background: #f87171; }}
QLabel#message {{ color: #aab0b8; font-size: 9pt; }}
QLabel#message[error="true"] {{ color: #f87171; }}
QFrame#card {{ background: #1a1d22; border: 1px solid #262a31; border-radius: 12px; }}
QComboBox {{ background: #1f2329; border: 1px solid #2d323a; border-radius: 10px;
            padding: 9px 12px; font-size: 11.5pt; font-weight: 600; }}
QComboBox:hover {{ border-color: #3a404a; }}
QComboBox::drop-down {{ border: none; width: 34px; }}
QComboBox::down-arrow {{ image: url({CHEVRON}); width: 12px; height: 12px; }}
QComboBox QAbstractItemView {{ background: #1f2329; border: 1px solid #2d323a; padding: 4px;
                               selection-background-color: {ACCENT}; outline: none; }}
QPushButton {{ background: #23272e; border: 1px solid #30353e; border-radius: 8px; padding: 6px 12px; }}
QPushButton:hover {{ background: #2b3038; }}
QPushButton:pressed {{ background: #1d2026; }}
QPushButton:disabled {{ color: #5b616b; border-color: #262a31; }}
QDialog QPushButton:default {{ background: {ACCENT}; border: none; color: white; font-weight: 600; }}
QDialog QPushButton:default:hover {{ background: #f04b4e; }}
QPushButton#ghost {{ background: transparent; border: none; color: #8a9099; padding: 4px 6px; }}
QPushButton#ghost:hover {{ color: #e7e9ec; }}
QPushButton#small {{ padding: 4px 10px; font-size: 9pt; }}
QPushButton#primary {{ background: {ACCENT}; border: none; border-radius: 10px; color: white;
                       font-size: 12pt; font-weight: 700; padding: 12px; }}
QPushButton#primary:hover {{ background: #f04b4e; }}
QPushButton#primary:disabled {{ background: #5a2a2b; color: #c9a3a4; }}
QPushButton#primary[on="true"] {{ background: #1f2329; border: 1px solid {ACCENT}; color: #ff7a7c; }}
QPushButton#primary[on="true"]:hover {{ background: #2a1f21; }}
QLineEdit {{ background: #1f2329; border: 1px solid #2d323a; border-radius: 8px; padding: 7px 9px; }}
QLineEdit:focus {{ border-color: {ACCENT}; }}
QListWidget {{ background: #1a1d22; border: 1px solid #262a31; border-radius: 10px; padding: 4px; outline: none; }}
QListWidget::item {{ padding: 8px; border-radius: 6px; }}
QListWidget::item:selected {{ background: {ACCENT}; color: white; }}
QMenu {{ background: #1a1d22; border: 1px solid #2d323a; padding: 4px; }}
QMenu::item {{ padding: 6px 18px; border-radius: 6px; }}
QMenu::item:selected {{ background: {ACCENT}; }}
QMenu::separator {{ height: 1px; background: #2d323a; margin: 4px 8px; }}
QToolTip {{ background: #1f2329; color: #e7e9ec; border: 1px solid #2d323a; padding: 6px; }}
"""


def set_prop(widget, name, value):
    """Set a dynamic property used by the stylesheet and re-apply the style."""
    if widget.property(name) != value:
        widget.setProperty(name, value)
        widget.style().unpolish(widget)
        widget.style().polish(widget)


def label(text="", name=None):
    lbl = QLabel(text)
    if name:
        lbl.setObjectName(name)
    return lbl


class Card(QFrame):
    """A rounded status tile: title, dot + value, small detail line, optional button."""

    def __init__(self, title, button=None):
        super().__init__()
        self.setObjectName("card")
        self.dot = label(name="dot")
        set_prop(self.dot, "state", "off")
        self.value = label("…", "cardValue")
        self.sub = label("", "cardSub")
        head = QHBoxLayout()
        head.setSpacing(8)
        head.addWidget(self.dot, 0, Qt.AlignVCenter)
        head.addWidget(self.value, 1)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(14, 12, 14, 12)
        layout.setSpacing(4)
        layout.addWidget(label(title.upper(), "cardTitle"))
        layout.addLayout(head)
        layout.addWidget(self.sub)
        if button:
            button.setObjectName("small")
            layout.addSpacing(4)
            layout.addWidget(button, 0, Qt.AlignLeft)
        layout.addStretch(1)

    def set(self, state, value, sub=""):
        set_prop(self.dot, "state", state)
        width = max(self.value.width(), 120)
        self.value.setText(self.value.fontMetrics().elidedText(value, Qt.ElideRight, width))
        self.value.setToolTip(value if len(value) > 18 else "")
        self.sub.setText(self.sub.fontMetrics().elidedText(sub, Qt.ElideRight, max(self.sub.width(), 120)))


# ---------------------------------------------------------------- app

class Bridge(QObject):
    status = Signal(dict)
    busy = Signal(str)
    done = Signal(str)  # error message, or "" on success


class Panel(QWidget):
    WIDTH, HEIGHT = 480, 580
    HINT = "Turn the relay on before starting the game, and enable the same game in ExitLag."

    def __init__(self, app):
        super().__init__()
        self.app = app
        self.setWindowTitle("ExitLag Relay")
        self.setFixedSize(self.WIDTH, self.HEIGHT)
        self.games, selected = load_games()
        self.relay_on = False
        self.last = {}
        self.rate = None  # (time, tx, rx) of the previous status, for packets/s
        self.working = False
        self.quitting = False
        self.bridge = Bridge()
        self.bridge.status.connect(self.show_status)
        self.bridge.busy.connect(self.set_busy)
        self.bridge.done.connect(self.task_done)

        # header
        self.pill = label("OFF", "pill")
        self.pill.setAlignment(Qt.AlignCenter)
        self.pill.setFixedWidth(92)
        set_prop(self.pill, "state", "off")
        titles = QVBoxLayout()
        titles.setSpacing(0)
        titles.addWidget(label("ExitLag Relay", "title"))
        titles.addWidget(label("Linux games through ExitLag under Wine", "subtitle"))
        header = QHBoxLayout()
        header.addLayout(titles, 1)
        header.addWidget(self.pill, 0, Qt.AlignTop)

        # game picker
        self.combo = QComboBox()
        self.combo.setMinimumHeight(44)
        self.reload_combo(selected)
        self.combo.currentIndexChanged.connect(self.game_changed)
        game_tools = QHBoxLayout()
        game_tools.setSpacing(2)
        game_tools.addWidget(label("GAME", "section"))
        game_tools.addStretch(1)
        for text, slot in (("Add", self.add_game), ("Edit", self.edit_game), ("Remove", self.remove_game)):
            b = QPushButton(text)
            b.setObjectName("ghost")
            b.setCursor(Qt.PointingHandCursor)
            b.clicked.connect(slot)
            game_tools.addWidget(b)

        self.toggle = QPushButton("Start relay")
        self.toggle.setObjectName("primary")
        self.toggle.setCursor(Qt.PointingHandCursor)
        self.toggle.clicked.connect(lambda: self.set_relay(not self.relay_on))

        # status cards
        self.wine_btn = QPushButton("Start")
        self.wine_btn.clicked.connect(self.toggle_exitlag)
        self.exitlag_btn = QPushButton("Open ExitLag")
        self.exitlag_btn.clicked.connect(self.open_exitlag)
        self.cards = {
            "exitlag": Card("ExitLag (Wine)", self.wine_btn),
            "link": Card("ExitLag link", self.exitlag_btn),
            "divert": Card("Diversion"),
            "procs": Card("Game processes"),
        }
        grid = QGridLayout()
        grid.setSpacing(10)
        for i, key in enumerate(("exitlag", "link", "divert", "procs")):
            grid.addWidget(self.cards[key], i // 2, i % 2)
        self.traffic = Card("Traffic")

        self.msg = label(self.HINT, "message")
        self.msg.setWordWrap(True)
        self.msg.setFixedHeight(36)
        self.msg.setAlignment(Qt.AlignLeft | Qt.AlignTop)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(22, 20, 22, 16)
        layout.setSpacing(10)
        layout.addLayout(header)
        layout.addSpacing(8)
        layout.addLayout(game_tools)
        layout.addWidget(self.combo)
        layout.addSpacing(2)
        layout.addWidget(self.toggle)
        layout.addSpacing(6)
        layout.addLayout(grid)
        layout.addWidget(self.traffic)
        layout.addStretch(1)
        layout.addWidget(self.msg)

        self.make_tray()
        threading.Thread(target=self.poll, daemon=True).start()
        self.run("Starting the relay service…", ensure_service)

    # ---- tray

    def make_tray(self):
        self.icon_on = QIcon.fromTheme("network-transmit-receive", self.style().standardIcon(QStyle.SP_MediaPlay))
        self.icon_off = QIcon.fromTheme("network-offline", self.style().standardIcon(QStyle.SP_MediaStop))
        self.setWindowIcon(self.icon_on)
        self.tray = QSystemTrayIcon(self.icon_off, self)
        menu = QMenu()
        menu.addAction("Show", self.show_panel)
        self.tray_relay = QAction("Relay on", menu, checkable=True)
        self.tray_relay.triggered.connect(self.set_relay)
        menu.addAction(self.tray_relay)
        menu.addAction("Open ExitLag", self.open_exitlag)
        self.tray_wine = menu.addAction("Start ExitLag", self.toggle_exitlag)
        menu.addSeparator()
        menu.addAction("Exit (closes ExitLag)", self.exit_all)
        self.tray.setContextMenu(menu)
        self.tray.activated.connect(lambda r: self.show_panel() if r == QSystemTrayIcon.Trigger else None)
        self.tray.show()
        self.told_tray = False

    def show_panel(self):
        self.show()
        self.raise_()
        self.activateWindow()

    def closeEvent(self, ev):
        if self.quitting or not QSystemTrayIcon.isSystemTrayAvailable():
            return super().closeEvent(ev)
        ev.ignore()
        self.hide()
        if not self.told_tray:
            self.tray.showMessage("ExitLag Relay", "Still running in the tray. Right-click it to exit.",
                                  self.icon_on, 4000)
            self.told_tray = True

    # ---- games

    def current_game(self):
        i = self.combo.currentIndex()
        return self.games[i] if 0 <= i < len(self.games) else None

    def reload_combo(self, select=None):
        self.combo.blockSignals(True)
        self.combo.clear()
        for g in self.games:
            self.combo.addItem(g["name"])
        names = [g["name"] for g in self.games]
        self.combo.setCurrentIndex(names.index(select) if select in names else 0)
        self.combo.blockSignals(False)

    def save(self):
        g = self.current_game()
        save_games(self.games, g["name"] if g else None)

    def add_game(self):
        d = GameDialog(self)
        if d.exec():
            self.games.append(d.game())
            self.reload_combo(d.game()["name"])
            self.game_changed()

    def edit_game(self):
        g = self.current_game()
        if not g:
            return
        d = GameDialog(self, g)
        if d.exec():
            self.games[self.combo.currentIndex()] = d.game()
            self.reload_combo(d.game()["name"])
            self.game_changed()

    def remove_game(self):
        g = self.current_game()
        if g and QMessageBox.question(self, "Remove", f"Remove {g['name']}?") == QMessageBox.Yes:
            self.games.remove(g)
            self.reload_combo()
            self.game_changed()

    def game_changed(self):
        self.save()
        if self.relay_on:
            self.set_relay(True)  # switch the relay to the new game

    # ---- actions (run off the UI thread)

    def run(self, label, fn):
        if self.working:
            return
        def task():
            self.bridge.busy.emit(label)
            try:
                fn()
                self.bridge.done.emit("")
            except Exception as e:  # shown to the user
                self.bridge.done.emit(str(e))
        threading.Thread(target=task, daemon=True).start()

    def set_busy(self, text):
        self.working = True
        self.show_message(text)
        for w in (self.toggle, self.wine_btn, self.combo):
            w.setEnabled(False)

    def task_done(self, err):
        self.working = False
        for w in (self.toggle, self.wine_btn, self.combo):
            w.setEnabled(True)
        if err:
            self.show_message(err, error=True)
            self.tray.showMessage("ExitLag Relay", err, QSystemTrayIcon.Warning, 6000)
        else:
            self.show_message(self.HINT)
        if self.quitting:
            self.app.quit()

    def show_message(self, text, error=False):
        set_prop(self.msg, "error", "true" if error else "false")
        self.msg.setText(text)

    def set_relay(self, on):
        game = self.current_game()
        if on and not game:
            return
        if on and not exitlag_installed():
            self.tray_relay.setChecked(False)
            self.install_exitlag()
            return
        self.relay_on = on
        self.tray_relay.setChecked(on)
        if on:
            self.run(f"Starting the relay for {game['name']}…", lambda: self.relay_up(game))
        else:
            self.run("Stopping the relay…", self.relay_down)

    def relay_up(self, game):
        ensure_service()
        # The daemon tells the shim in ExitLag which game to stand in for.
        daemon({"cmd": "set", "profile": game})
        if not wine_pids():
            self.bridge.busy.emit("Starting ExitLag…")
            start_exitlag_wine()
        for _ in range(120):
            if daemon({"cmd": "status"}).get("exitlag_up"):
                return
            time.sleep(1)
        raise RuntimeError("ExitLag didn't connect; is it open and signed in?")

    def relay_down(self):
        try:
            daemon({"cmd": "clear"})
        except (OSError, ValueError):
            pass  # service not running: nothing is diverted anyway

    def install_exitlag(self):
        """Ask for ExitLag's Windows installer and install it under Wine."""
        if self.working:
            return
        QMessageBox.information(
            self, "Install ExitLag",
            "ExitLag isn't installed in Wine yet.\n\nDownload the Windows installer from "
            "exitlag.com, then pick it in the next window. Setup takes a few minutes the first "
            "time (it downloads GE-Proton). ExitLag's installer will say it couldn't install "
            "the NDIS driver: click OK, it isn't needed.")
        downloads = Path.home() / "Downloads"
        path, _ = QFileDialog.getOpenFileName(self, "ExitLag installer",
                                              str(downloads if downloads.is_dir() else Path.home()),
                                              "Windows installer (*.exe)")
        if path:
            self.run("Installing ExitLag under Wine (this takes a few minutes)…",
                     lambda: install_exitlag_wine(path))

    def open_exitlag(self):
        if not exitlag_installed():
            self.install_exitlag()
        else:
            self.run("Opening ExitLag…", open_exitlag_wine)

    def toggle_exitlag(self):
        if not exitlag_installed():
            self.install_exitlag()
        elif self.last.get("exitlag") == "running":
            self.run("Closing ExitLag…", self.close_exitlag)
        else:
            self.run("Starting ExitLag…", start_exitlag_wine)

    def close_exitlag(self):
        if self.relay_on:
            self.relay_on = False
            try:
                self.relay_down()
            except Exception:
                pass
        stop_exitlag_wine()

    def exit_all(self):
        self.quitting = True
        self.tray.showMessage("ExitLag Relay", "Turning the relay off and closing ExitLag…", self.icon_off, 4000)
        def shutdown():
            try:
                self.close_exitlag()
            finally:
                service("stop")
        self.run("Turning the relay off, closing ExitLag and stopping the relay service…", shutdown)

    # ---- status

    def poll(self):
        while True:
            st = {"exitlag": wine_state()}
            try:
                st.update(daemon({"cmd": "status"}))
                st["daemon"] = True
            except (OSError, ValueError, RuntimeError):
                st["daemon"] = False
            self.bridge.status.emit(st)
            time.sleep(1)

    def show_status(self, st):
        self.last = st
        state = st["exitlag"]
        running = state == "running"
        if state == "not installed":
            self.cards["exitlag"].set("bad", "Not installed", "click Install")
            self.wine_btn.setText("Install")
        else:
            self.cards["exitlag"].set("on" if running else "off", "Running" if running else "Stopped",
                                      "exitlag-wine")
            self.wine_btn.setText("Close" if running else "Start")
        self.tray_wine.setText({"Install": "Install ExitLag…", "Close": "Close ExitLag"}.get(
            self.wine_btn.text(), "Start ExitLag"))

        if not st["daemon"]:
            self.cards["link"].set("off", "Service stopped", "starts with the relay")
            self.cards["divert"].set("off", "Off")
            self.cards["procs"].set("off", "None")
            self.traffic.set("off", "Idle")
            self.rate = None
            self.set_pill("off")
            if not self.working:
                self.set_toggle(False)
            return

        prof = st.get("profile")
        if prof and not self.relay_on and not self.working:  # state from before a GUI restart
            self.relay_on = True
        if st.get("exitlag_up"):
            self.cards["link"].set("on", "Connected", st.get("peer_addr", "").split(":")[0])
        else:
            self.cards["link"].set("wait" if prof else "off", "Not connected",
                                   "waits for ExitLag's sign-in" if prof else "")

        diverting = bool(st.get("diverting"))
        if diverting:
            self.cards["divert"].set("on", "On", prof["name"])
        elif prof:
            self.cards["divert"].set("wait", "Waiting", "for ExitLag")
        else:
            self.cards["divert"].set("off", "Off", "pick a game and start the relay")

        pids = st.get("pids") or []
        if pids:
            self.cards["procs"].set("on", f"{len(pids)} attached", ", ".join(map(str, pids[:6])))
        elif prof:
            self.cards["procs"].set("wait", "None yet", "start the game now")
        else:
            self.cards["procs"].set("off", "None")

        tx, rx, now = st.get("tx_packets", 0), st.get("rx_packets", 0), time.monotonic()
        if self.rate:
            t0, tx0, rx0 = self.rate
            dt = max(now - t0, 0.001)
            up, down = (tx - tx0) / dt, (rx - rx0) / dt
            active = up + down > 0.5
            self.traffic.set("on" if active else ("wait" if diverting else "off"),
                             f"↑ {up:.0f}   ↓ {down:.0f}  packets/s", f"total {tx:,} out · {rx:,} in")
        elif not diverting:
            self.traffic.set("off", "Idle")
        self.rate = (now, tx, rx)

        self.set_pill("on" if diverting else ("wait" if prof else "off"))
        self.tray.setIcon(self.icon_on if diverting else self.icon_off)
        self.tray.setToolTip("ExitLag Relay: " + (f"on ({prof['name']})" if diverting else "off"))
        if not self.working:
            self.set_toggle(self.relay_on)
            self.tray_relay.setChecked(self.relay_on)

    def set_pill(self, state):
        self.pill.setText({"on": "RELAY ON", "wait": "WAITING", "off": "OFF"}[state])
        set_prop(self.pill, "state", state)

    def set_toggle(self, on):
        self.toggle.setText("Stop relay" if on else "Start relay")
        set_prop(self.toggle, "on", "true" if on else "false")


def main():
    QApplication.setDesktopFileName(APP_ID)
    app = QApplication(sys.argv)
    app.setApplicationName("ExitLag Relay")
    app.setStyle("Fusion")
    app.setStyleSheet(STYLE)
    app.setQuitOnLastWindowClosed(False)

    # Single instance: a second launch just shows the first one's window.
    probe = QLocalSocket()
    probe.connectToServer(APP_ID)
    if probe.waitForConnected(300):
        probe.write(b"show")
        probe.waitForBytesWritten(300)
        return 0
    QLocalServer.removeServer(APP_ID)
    server = QLocalServer()
    server.listen(APP_ID)

    panel = Panel(app)
    server.newConnection.connect(lambda: (server.nextPendingConnection(), panel.show_panel()))
    if "--hidden" not in sys.argv:
        panel.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
