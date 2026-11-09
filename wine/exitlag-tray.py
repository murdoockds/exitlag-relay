#!/usr/bin/env python3
"""exitlag-tray: ExitLag's tray icon as a native StatusNotifierItem.

Wine's tray item for ExitLag only supports ContextMenu(x, y), which makes
ExitLag pop up a Win32 menu at those coordinates in Windows' screen layout:
on a multi-monitor niri desktop it opened on the wrong monitor. Here the icon
is a regular StatusNotifierItem with a DBusMenu, so the desktop's tray draws
the menu itself, where it was clicked.

The nfapi.dll shim inside ExitLag (tray.c) keeps ExitLag's icon away from
Wine and talks to us over UDP on 127.0.0.1:PORT; see tray.c for the protocol.
exitlag-wine starts us next to ExitLag (EXITLAG_TRAY_PORT) and stops us after.

    exitlag-tray PORT
"""
import asyncio
import os
import socket
import sys

from dbus_fast import BusType, Variant
from dbus_fast.aio import MessageBus
from dbus_fast.service import PropertyAccess, ServiceInterface, dbus_property, method, signal

WATCHER = "org.kde.StatusNotifierWatcher"


class Link(asyncio.DatagramProtocol):
    """The UDP conversation with the shim inside ExitLag."""

    def __init__(self, app):
        self.app = app
        self.transport = None
        self.menu_reply = None

    def connection_made(self, transport):
        self.transport = transport

    def send(self, text):
        if self.transport:
            self.transport.sendto(text.encode())

    def datagram_received(self, data, addr):
        head, _, rest = data.partition(b"\n")
        word, *args = head.decode(errors="replace").split(" ")
        if word == "STATE":
            self.app.set_state(args[0] == "1", rest.decode(errors="replace"))
        elif word == "ICON" and len(args) == 2:
            self.app.set_icon(int(args[0]), int(args[1]), rest)
        elif word == "ITEMS":
            if self.menu_reply and not self.menu_reply.done():
                self.menu_reply.set_result(rest.decode(errors="replace"))

    async def menu(self):
        self.menu_reply = asyncio.get_running_loop().create_future()
        self.send("MENU")
        try:
            return await asyncio.wait_for(self.menu_reply, 2)
        except asyncio.TimeoutError:
            return None


def win32_label(text):
    """Win32 mnemonics (&File, && for &) to DBusMenu's (_File, __ for _)."""
    out, i = [], 0
    while i < len(text):
        c = text[i]
        if c == "&" and i + 1 < len(text) and text[i + 1] == "&":
            out.append("&")
            i += 2
            continue
        out.append("_" if c == "&" else ("__" if c == "_" else c))
        i += 1
    return "".join(out).split("\t")[0]  # drop accelerator text


class Item(ServiceInterface):
    def __init__(self, app):
        super().__init__("org.kde.StatusNotifierItem")
        self.app = app

    @dbus_property(access=PropertyAccess.READ)
    def Category(self) -> "s":
        return "ApplicationStatus"

    @dbus_property(access=PropertyAccess.READ)
    def Id(self) -> "s":
        return "ExitLag"

    @dbus_property(access=PropertyAccess.READ)
    def Title(self) -> "s":
        return "ExitLag"

    @dbus_property(access=PropertyAccess.READ)
    def Status(self) -> "s":
        return "Active"

    @dbus_property(access=PropertyAccess.READ)
    def WindowId(self) -> "i":
        return 0

    @dbus_property(access=PropertyAccess.READ)
    def IconName(self) -> "s":
        return "" if self.app.icon else "network-transmit-receive"

    @dbus_property(access=PropertyAccess.READ)
    def IconPixmap(self) -> "a(iiay)":
        return [self.app.icon] if self.app.icon else []

    @dbus_property(access=PropertyAccess.READ)
    def ToolTip(self) -> "(sa(iiay)ss)":
        return ["", [], "ExitLag", self.app.tip]

    @dbus_property(access=PropertyAccess.READ)
    def ItemIsMenu(self) -> "b":
        return False

    @dbus_property(access=PropertyAccess.READ)
    def Menu(self) -> "o":
        return "/MenuBar"

    @method()
    def Activate(self, x: "i", y: "i"):
        self.app.link.send("ACTIVATE")

    @method()
    def SecondaryActivate(self, x: "i", y: "i"):
        self.app.link.send("SECONDARY")

    @method()
    def ContextMenu(self, x: "i", y: "i"):
        pass  # hosts use Menu

    @method()
    def Scroll(self, delta: "i", orientation: "s"):
        pass

    @signal()
    def NewIcon(self):
        pass

    @signal()
    def NewToolTip(self):
        pass


class Menu(ServiceInterface):
    """com.canonical.dbusmenu, mirroring ExitLag's tray menu."""

    def __init__(self, app):
        super().__init__("com.canonical.dbusmenu")
        self.app = app
        self.revision = 1
        self.items = {}  # id -> (parent, flags, label)
        self.raw = None

    def load(self, raw):
        if raw is None or raw == self.raw:
            return False
        self.raw = raw
        self.items = {}
        for line in raw.splitlines():
            parts = line.split("\t", 3)
            if len(parts) == 4:
                self.items[int(parts[0])] = (int(parts[1]), int(parts[2]), parts[3])
        self.revision += 1
        return True

    def props(self, id_):
        if id_ == 0:
            return {"children-display": Variant("s", "submenu")}
        parent, flags, label = self.items[id_]
        if flags & 1:
            return {"type": Variant("s", "separator")}
        p = {"label": Variant("s", win32_label(label)), "enabled": Variant("b", not flags & 2)}
        if flags & 16:
            p["children-display"] = Variant("s", "submenu")
        if flags & 4 or flags & 8:
            p["toggle-type"] = Variant("s", "radio" if flags & 8 else "checkmark")
            p["toggle-state"] = Variant("i", 1 if flags & 4 else 0)
        return p

    def node(self, id_, depth):
        children = []
        if depth != 0:
            children = [Variant("(ia{sv}av)", self.node(c, depth - 1))
                        for c, (parent, _, _) in self.items.items() if parent == id_]
        return [id_, self.props(id_), children]

    @dbus_property(access=PropertyAccess.READ)
    def Version(self) -> "u":
        return 3

    @dbus_property(access=PropertyAccess.READ)
    def TextDirection(self) -> "s":
        return "ltr"

    @dbus_property(access=PropertyAccess.READ)
    def Status(self) -> "s":
        return "normal"

    @dbus_property(access=PropertyAccess.READ)
    def IconThemePath(self) -> "as":
        return []

    @method()
    async def GetLayout(self, parent: "i", depth: "i", names: "as") -> "u(ia{sv}av)":
        if not self.items:
            self.load(await self.app.link.menu())
        if parent != 0 and parent not in self.items:
            parent = 0
        return [self.revision, self.node(parent, depth)]

    @method()
    def GetGroupProperties(self, ids: "ai", names: "as") -> "a(ia{sv})":
        return [[i, self.props(i)] for i in (ids or [0, *self.items]) if i == 0 or i in self.items]

    @method()
    def GetProperty(self, id_: "i", name: "s") -> "v":
        return self.props(id_).get(name, Variant("s", ""))

    def event(self, id_, event):
        if event == "clicked" and id_ in self.items:
            self.app.link.send(f"CLICK {id_}")

    @method()
    def Event(self, id_: "i", event: "s", data: "v", timestamp: "u"):
        self.event(id_, event)

    @method()
    def EventGroup(self, events: "a(isvu)") -> "ai":
        for id_, event, _, _ in events:
            self.event(id_, event)
        return []

    @method()
    async def AboutToShow(self, id_: "i") -> "b":
        # The menu can change (e.g. Restore/Hide); fetch it fresh on every open.
        changed = self.load(await self.app.link.menu())
        if changed:
            self.LayoutUpdated(self.revision, 0)
        return changed

    @method()
    async def AboutToShowGroup(self, ids: "ai") -> "aiai":
        changed = self.load(await self.app.link.menu())
        if changed:
            self.LayoutUpdated(self.revision, 0)
        return [[0] if changed else [], []]

    @signal()
    def LayoutUpdated(self, revision, parent) -> "ui":
        return [revision, parent]



class App:
    def __init__(self, port):
        self.port = port
        self.present = False
        self.tip = ""
        self.icon = None
        self.bus = None
        self.name = f"org.kde.StatusNotifierItem-{os.getpid()}-1"
        self.registered = False
        self.link = Link(self)
        self.item = Item(self)
        self.menu = Menu(self)

    def set_state(self, present, tip):
        self.tip = tip
        if self.registered:
            self.item.NewToolTip()
        if present != self.present:
            self.present = present
            asyncio.ensure_future(self.sync())

    def set_icon(self, w, h, argb):
        if len(argb) == w * h * 4:
            self.icon = [w, h, bytes(argb)]
            if self.registered:
                self.item.NewIcon()

    async def sync(self):
        """Register with the tray while ExitLag has an icon, else go away."""
        if self.present and not self.registered:
            await self.bus.request_name(self.name)
            self.registered = True
            await self.register()
        elif not self.present and self.registered:
            await self.bus.release_name(self.name)
            self.registered = False

    async def register(self):
        try:
            intro = await self.bus.introspect(WATCHER, "/StatusNotifierWatcher")
            watcher = self.bus.get_proxy_object(WATCHER, "/StatusNotifierWatcher", intro)
            await watcher.get_interface(WATCHER).call_register_status_notifier_item(self.name)
        except Exception as e:  # no tray yet; NameOwnerChanged brings us back
            print(f"exitlag-tray: no StatusNotifierWatcher ({e})", file=sys.stderr)

    def on_name_owner_changed(self, msg):
        # The tray (re)started: register again.
        if msg.member == "NameOwnerChanged" and msg.body and msg.body[0] == WATCHER and msg.body[2]:
            if self.registered:
                asyncio.ensure_future(self.register())

    async def main(self):
        self.bus = await MessageBus(bus_type=BusType.SESSION).connect()
        self.bus.export("/StatusNotifierItem", self.item)
        self.bus.export("/MenuBar", self.menu)
        introspection = await self.bus.introspect("org.freedesktop.DBus", "/org/freedesktop/DBus")
        dbus = self.bus.get_proxy_object("org.freedesktop.DBus", "/org/freedesktop/DBus", introspection)
        await dbus.get_interface("org.freedesktop.DBus").call_add_match(
            "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='%s'" % WATCHER)
        self.bus.add_message_handler(self.on_name_owner_changed)

        loop = asyncio.get_running_loop()
        await loop.create_datagram_endpoint(lambda: self.link, remote_addr=("127.0.0.1", self.port),
                                            family=socket.AF_INET)
        while True:
            self.link.send("HELLO")
            await asyncio.sleep(1)


if __name__ == "__main__":
    if sys.argv[1:] == ["--check"]:  # the imports above worked
        sys.exit(0)
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().splitlines()[-1].strip())
    asyncio.run(App(int(sys.argv[1])).main())
