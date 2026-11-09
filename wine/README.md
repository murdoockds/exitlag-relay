# ExitLag under Wine

How the Wine side works: ExitLag runs under Wine (GE-Proton through umu),
and `exitlag-relayd` feeds it the game's UDP through a replacement for
ExitLag's packet-filter DLL. The main [README](../README.md) covers
installing and using it.

(The project started with ExitLag in a small Windows VM and a relay program
running under the game's `.exe` name. Some measurements below compare
against that setup.)

The approach follows [radmin-vpn-linux](https://github.com/baptisterajaut/radmin-vpn-linux),
which runs Radmin VPN under Wine by replacing the kernel driver Wine can't
load. ExitLag is easier: it reaches its driver through a vendor DLL with a
known API, so the DLL is what gets replaced and no driver code is needed.

```
game (Linux) ──► exitlag0 ──► exitlag-relayd ── UDP (internal/wire) ──┐
                                                                       ▼
            ExitLag.exe (Wine) ◄──► nfapi.dll shim: each game port is a "socket"
                 │                  ExitLag intercepted, owned by a stand-in
                 ▼                  process named <game>.exe
            ExitLag's tunnel (plain Winsock) ──► ExitLag servers ──► game servers
```

## Using it

```
exitlag-wine setup ~/Downloads/SetupExitLag-*.exe   # once (the app does this too)
exitlag-wine                                         # start ExitLag
```

Setup installs ExitLag into `~/.local/share/exitlag-wine/prefix` with its
own installer. The installer warns that it couldn't install the NDIS driver;
click OK, that path isn't used. ExitLag updates itself as on Windows.

The **ExitLag Relay** app starts ExitLag when the relay is turned on, and
the daemon tells the shim which game to stand in for. Sign in to ExitLag
once; boost the game in ExitLag as usual.

The first route calculation may end with *"could not discover network by
using ICMP because your ISP limits the number of requests, change the method
to TCP?"*: answer **Yes**. The cause is a Wine bug (see
[Limits](#not-done--limits)). With TCP, routes are calculated quickly.

`exitlag-wine stop` closes it. `exitlag-wine exec PROGRAM` runs something
else in the prefix, only while ExitLag is closed: every umu run gets a
private `/tmp`, so it can't reach a running wineserver and would start a
second one on the same prefix.

## What ExitLag needs, and what provides it

ExitLag 5.x diverts traffic with **NetFilter SDK** (`nfapi.dll` +
`nfextlag.sys`, its "WFP" divert driver) or **Windows Packet Filter**
(`ndisapi.dll` + `ndextlag.sys`, "NDIS"). Only the WFP path is provided.

| ExitLag checks / uses | Provided by |
|---|---|
| service `nfextlag` is running | `nfextlag-wine.sys`, a do-nothing driver Wine loads; the service's ImagePath points at it |
| version of `system32\drivers\nfextlag.sys` ("bad driver version" otherwise) | the real file, left in place (never loaded) |
| `nf_init`, `nf_setRulesEx`, `nf_udp*`, flow-control calls (18 imports) | `nfapi.dll` shim |
| endpoint's process is the game (`nf_getProcessNameFromKernel`) | `idle.exe` copied to `C:\Relay\games\<game>.exe` |
| WMI self-check (`Win32_LogicalDiskRootDirectory`, then a "repair WMI?" box) | `wmifix.c`: answers the query, answers the box No |

ExitLag's updater re-runs its installer, which restores the real `nfapi.dll`
and re-registers the driver; `exitlag-wine` re-applies both before every
start (registry fixed in a separate prefix boot, so Wine never tries the real
driver).

### The nfapi.dll shim (`nfshim/`)

Besides standing in for NetFilter, it carries the Wine fixes below
(`ipv4only.c`, `wmifix.c`, `vblank.c`, `dpifix.c`, `tray.c`) because it's
loaded into ExitLag before anything else runs.

Built with `zig cc -target x86_64-windows-gnu`. ExitLag imports the C++
names (`?nf_init@nfapi@@YA?AW4_NF_STATUS@@...`), mapped to C functions in
`nfapi.def`.

- **Event handler:** ExitLag's `NfEventHandler` vtable has the 16 slots of
  the public `NF_EventHandler` (threadStart … udpCanSend), called directly.
- **Undocumented functions** in ExitLag's NetFilter build, read from the real
  DLL: `nf_getExtensionsVersion` (returns `HKLM\SOFTWARE\ExitLag`
  `NfsdkExtVersion`, 8), `nf_udpEnableFiltering(id)`,
  `nf_udpAddRemoteAddrRule(id, rule)` (19-byte rule: 16-byte address, port,
  flag; logged only). `NF_RULE_EX` is 0x283 bytes (NF_RULE + MAX_PATH name +
  40 bytes).
- **Data path:** one endpoint per game source port. Host → `udpSend` (or out
  directly once ExitLag disabled filtering for it); `nf_udpPostSend` → the
  endpoint's own Winsock socket; that socket's replies → `udpReceive` (or back
  to the host); `nf_udpPostReceive` → back to the host, to the game.
  Endpoints close after 30 s without traffic from the host. An endpoint's
  socket is opened only when something is sent outside ExitLag's tunnel
  (`nf_udpPostSend`, or filtering disabled): while boosting, ExitLag tunnels
  everything itself, and CS2 opens ~90 game ports at start (Steam relay
  pings). A socket each made ~180 extra lines in `ss -lunp` (ExitLag.exe
  and wineserver), enough for Noctalia's portctl plugin to exceed its CPU
  budget, and the network thread only polled 60 of them.
- **Config:** `C:\Relay\nfshim.ini` (`host=`, written by `exitlag-wine`;
  `game=` as a fallback; `ipv6=1` to let ExitLag use IPv6, read at start;
  `profile=1`, `windiag=1` for diagnostics). The daemon names the game itself with a
  `Game` datagram after every Ack. Log: `C:\Relay\nfshim.log`.

### Fixes for Wine itself

- **IPv4 preferred** (`ipv4only.c`; `ipv6=1` turns it off): in the VM
  ExitLag only had IPv4 (libvirt's NAT network has none). On the host it
  tunnelled over IPv6 while measuring routes with IPv4 pings, which looked
  like the cause of a higher ping. Import hooks in ExitLag.exe,
  libexitlag.dll and Qt6Network.dll hide IPv6: `getaddrinfo` returns IPv4
  only, `GetAdaptersAddresses` and `GetIpForwardTable2` have no IPv6
  addresses or routes, `Icmp6CreateFile` fails. IPv6 sockets themselves are
  only logged: refusing them (`WSAEAFNOSUPPORT`) killed ExitLag with an
  unhandled C++ exception. ExitLag still opens IPv6 tunnels to addresses
  from its own server list, so this only partly applies; see
  [Latency](#latency) for what was measured afterwards.

- **CPU:** Qt and ExitLag's overlay pace frames with
  `IDXGIOutput::WaitForVBlank`, which DXVK implements as a busy-wait. Three
  threads kept ~1.5 cores busy. `vblank.c` replaces it with a sleep to the
  next 60 Hz tick (ExitLag: ~150% → ~15% CPU with its window open).
  (`QT_D3D_NO_VBLANK_THREAD` never reached Qt: see the `QT_*` note below.)
- **`profile.c`** (`profile=1`) is the sampler that found that: ptrace was off
  on the development machine and winedbg can't reach a running prefix, so it runs inside
  ExitLag and logs busy threads' RIPs and likely return addresses.
- **HiDPI** (laptop panel at 1.75x, external monitor at 1x): Proton's
  Wayland driver gives a window its output's real pixels (and rescales it
  when it moves to another output) but reports 96 DPI on every monitor, so
  ExitLag came out tiny on the 1.75x panel. Qt can scale per screen
  (`QT_SCREEN_SCALE_FACTORS`, keyed by the monitor's EDID name, which niri
  reports with each output's scale), but **Proton's Wine drops every `QT_*`
  variable** from Windows programs' environment. `exitlag-wine` passes them
  as `EXITLAG_QT_*` and the shim restores them in `DllMain`, before Qt reads
  them. Moves between outputs then work on their own. A new (or re-shown)
  window is sized by Qt for the primary screen but mapped by niri on the
  focused output; `exitlag-wine` makes the focused output Proton's primary
  (`PROTON_WAYLAND_MONITOR`), and `dpifix.c` corrects any remaining
  mismatch by resizing the window by the ratio of the two scales.
  A prefix-wide DPI (`LogPixels` 168) is not the fix: it's right on one
  monitor and crams the layout on the other.
- **Resizing:** a symptom of the same mismatch (Wine and Qt disagreeing on
  the window's size), not a separate bug. niri resizes the floating window
  normally once the scale is right. (For floating windows Wine applies the
  compositor's size only for interactive resizes, tiled, maximized or
  fullscreen states.)
- **Tray menu:** Proton's tray item for ExitLag only supports
  `ContextMenu(x, y)`, so ExitLag opened a Win32 menu at those coordinates,
  read as Windows screen coordinates. Noctalia passes coordinates relative to
  its tray widget, and Windows' monitor layout differs from niri's anyway, so
  a click on the laptop's bar opened the menu on the external monitor.
  `tray.c` keeps ExitLag's `Shell_NotifyIconW` away from Wine (IAT hook in
  `qwindows.dll`, installed via `LdrRegisterDllNotification`) and hands icon,
  tooltip and menu to **`exitlag-tray`** (Python, dbus-fast), a regular
  StatusNotifierItem with a DBusMenu that the tray draws itself, where it was
  clicked. The menu is ExitLag's own: the shim sends the WM_CONTEXTMENU a
  right-click would and answers Qt's `TrackPopupMenuEx` with the items (or
  the chosen command) instead of showing it. `exitlag-wine` starts the
  helper next to ExitLag; without it (`EXITLAG_TRAY=`), Wine's item is used.
- `windiag.c` (`windiag=1`) logs the main window's geometry, styles, DPI and
  monitor whenever they change.
- `winemenubuilder` is disabled (it would rewrite the host's file
  associations to open in this prefix; radmin-vpn-linux's lesson).

## Latency

Measured on 2026-10-22 with `tcpdump -i any udp` during a CS2 match (about
1,900 game packets each way in 30 s, Wi-Fi uplink). The daemon-to-shim
legs are matched by payload; the tunnel legs by the nearest packet with the
tunnel's 44-byte overhead.

| Leg | Median | p99 |
|---|---|---|
| exitlag0 → daemon → shim (lo) | 0.056 ms | 0.15 ms |
| shim → ExitLag → tunnel out | 0.30 ms | 0.85 ms |
| tunnel in → ExitLag → shim | 0.20 ms | 0.85 ms |
| daemon → exitlag0 | 0.038 ms | 0.11 ms |

So the relay, Wine included, adds about **0.6 ms** round trip (one 47 ms
outlier in 30 s). In-game ping was 117 ms against 110–112 ms in the VM; the
difference is ExitLag's choice of route. It sent every packet over two
tunnels: IPv6 to an entry server 6 ms away, and IPv4 to a server 126 ms
away (slower than the whole route, so useless as a second path). The game
server was 220 ms away directly. To improve on it, pick routes in ExitLag
or compare with `ipv6=1`.

## Daemon side

- `-ignore-wine-prefix DIR`: processes whose `WINEPREFIX` is inside DIR are
  never diverted. Needed for Proton games: their Linux matcher (e.g.
  `r5apex.exe`) also matches the stand-in, and moving it would take
  ExitLag's wineserver, and so ExitLag's own tunnel sockets, with it.
- `Game` message (`internal/wire`): the selected profile's Windows exe, sent
  after each Ack.
- The daemon listens on `127.0.0.1:27999` by default; `exitlag-wine` writes
  the same address into `nfshim.ini` (`host=`) on every start.

## Not done / limits

- **Wine reports the wrong router for traceroute probes.** `IcmpSendEcho2Ex`
  with a low TTL returns `IP_TTL_EXPIRED_TRANSIT` (11013) with the
  *destination* as the reply address instead of the router that answered
  (tested with GE-Proton 11: every hop to 1.1.1.1 came back as 1.1.1.1,
  where `tracepath` shows 192.168.1.1, 10.0.15.54, ...). ExitLag's ICMP
  network discovery can't see any hop, blames ISP rate limiting and offers
  TCP instead, which works. A fix would be the daemon probing natively
  (ping socket with `IP_RECVERR`) on the shim's behalf; not done, since TCP
  is enough.

- UDP only. ExitLag sets TCP rules too; the shim
  never reports TCP connections.
- The WMI self-check still fails for a reason not tracked down (every WMI
  call it makes succeeds); only its prompt is suppressed.
- The NDIS path and ExitLagPmService's FPS/GPU monitoring aren't supported.
