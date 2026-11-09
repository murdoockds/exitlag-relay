// Command exitlag-relayd diverts a game's UDP traffic into a TUN device and
// forwards it to ExitLag running under Wine, where the nfapi.dll shim hands
// it to ExitLag as if it came from the game's Windows process.
//
// It is idle until a game profile is set over the control socket (by the
// GUI), and diverts only while a profile is set and the shim is checking
// in. Otherwise traffic goes out directly as usual.
package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"net/netip"
	"os"
	"os/signal"
	"os/user"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"exitlagrelay/internal/wire"
)

// fallbackPrefixes are Valve's relay /24s from the CS2 SDR config, used when
// the live config can't be fetched at startup.
var fallbackPrefixes = strings.Fields(`
39.156.182.0/24 45.121.184.0/24 58.254.163.0/24 61.152.104.0/24 89.222.108.0/24
89.222.117.0/24 101.206.160.0/24 103.10.124.0/24 103.10.125.0/24 103.28.54.0/24
111.206.90.0/24 112.45.45.0/24 114.113.213.0/24 117.185.19.0/24 118.123.224.0/24
120.233.106.0/24 140.206.227.0/24 146.66.152.0/24 146.66.155.0/24 152.233.52.0/24
155.133.224.0/24 155.133.225.0/24 155.133.226.0/24 155.133.227.0/24 155.133.230.0/24
155.133.238.0/24 155.133.244.0/24 155.133.246.0/24 155.133.248.0/24 155.133.249.0/24
155.133.252.0/24 155.133.255.0/24 162.254.192.0/24 162.254.193.0/24 162.254.194.0/24
162.254.195.0/24 162.254.196.0/24 162.254.197.0/24 162.254.198.0/24 162.254.199.0/24
180.163.126.0/24 183.60.122.0/24 185.25.180.0/24 185.25.182.0/24 185.25.183.0/24
205.196.6.0/24`)

const sdrConfigURL = "https://api.steampowered.com/ISteamApps/GetSDRConfig/v1/?appid=730"

type daemon struct {
	tun        *os.File
	link       *net.UDPConn
	allow      netip.Prefix
	div        diverter
	controlUID int

	peer     atomic.Pointer[netip.AddrPort]
	lastSeen atomic.Int64 // unix nanos of the last hello

	profile   atomic.Pointer[Profile]
	changed   chan struct{} // profile changed
	diverting atomic.Bool
	tx, rx    atomic.Uint64

	mu         sync.Mutex
	srcAddrs   map[uint16]netip.Addr // game source port -> source IP it used
	origCgroup map[int]string        // moved pid -> cgroup it came from
	statePath  string                // where origCgroup is persisted
	scopes     []string              // app scope cgroups matched by scope:
	scopePIDs  []int

	divMu  sync.Mutex // serializes changes to the diversion rules
	active *Profile   // profile currently installed (nil = off)

	// Wine prefixes whose processes are never diverted: ExitLag's own
	// prefix runs a stand-in named after the game's .exe.
	ignorePrefixes []string
}

func main() {
	listen := flag.String("listen", fmt.Sprintf("127.0.0.1:%d", wire.DefaultPort),
		"address the nfapi.dll shim inside ExitLag connects to")
	allow := flag.String("allow", "", "only accept the shim from this prefix (default: the /24 of -listen)")
	ifname := flag.String("tun", "exitlag0", "TUN interface name")
	priority := flag.Int("priority", 2712, "priority of the first of two ip rules (process, prefix)")
	table := flag.Int("table", 2713, "first of two routing tables (process, prefix)")
	cgroupDir := flag.String("cgroup", "/sys/fs/cgroup/exitlag-relay", "cgroup that game processes are moved into")
	cgroupRoot := flag.String("cgroup-root", "/sys/fs/cgroup", "cgroup2 mount point")
	control := flag.String("control", "/run/exitlag-relay/control.sock", "control socket for the GUI")
	state := flag.String("state", "", "file remembering where moved processes came from (survives restarts)")
	controlUser := flag.String("control-user", "", "only this user (and root) may use the control socket and have processes diverted")
	ignorePrefix := flag.String("ignore-wine-prefix", "",
		"comma-separated directories; Wine processes whose WINEPREFIX is inside one are never diverted (ExitLag's own prefix)")
	flag.Parse()

	laddr, err := netip.ParseAddrPort(*listen)
	if err != nil {
		log.Fatalf("bad -listen: %v", err)
	}
	allowed := netip.PrefixFrom(laddr.Addr(), 24).Masked()
	if *allow != "" {
		if allowed, err = netip.ParsePrefix(*allow); err != nil {
			log.Fatalf("bad -allow: %v", err)
		}
	}

	uid := -1
	if *controlUser != "" {
		u, err := user.Lookup(*controlUser)
		if err != nil {
			log.Fatalf("bad -control-user: %v", err)
		}
		uid, _ = strconv.Atoi(u.Uid)
	}

	d := &daemon{
		allow:          allowed,
		controlUID:     uid,
		changed:        make(chan struct{}, 1),
		srcAddrs:       map[uint16]netip.Addr{},
		origCgroup:     map[int]string{},
		statePath:      *state,
		ignorePrefixes: splitDirs(*ignorePrefix),
		div: diverter{
			ifname:      *ifname,
			mark:        "0x6d72",
			markPrio:    fmt.Sprint(*priority),
			markTable:   fmt.Sprint(*table),
			prefixPrio:  fmt.Sprint(*priority + 1),
			prefixTable: fmt.Sprint(*table + 1),
			cgroupDir:   *cgroupDir,
			cgroupRoot:  *cgroupRoot,
		},
	}

	if d.tun, err = openTun(*ifname); err != nil {
		log.Fatalf("create TUN %s: %v", *ifname, err)
	}
	must(ip("link", "set", *ifname, "mtu", "1400"))
	// Replies arrive on the TUN from addresses that are normally routed via
	// the real uplink; strict reverse-path filtering would drop them.
	must(os.WriteFile("/proc/sys/net/ipv4/conf/"+*ifname+"/rp_filter", []byte("0"), 0o644))
	// The relay is IPv4-only; no IPv6 link-local address for apps to find.
	must(os.WriteFile("/proc/sys/net/ipv6/conf/"+*ifname+"/disable_ipv6", []byte("1"), 0o644))
	must(d.div.setup())
	// Processes left in the cgroup by a previous run go back where they came from.
	d.loadOrig()
	d.release()

	// IP_FREEBIND lets us bind to an address that isn't up yet (e.g. a
	// bridge's, when we start before it at boot).
	lc := net.ListenConfig{Control: func(_, _ string, c syscall.RawConn) error {
		var serr error
		err := c.Control(func(fd uintptr) {
			serr = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_FREEBIND, 1)
		})
		if err != nil {
			return err
		}
		return serr
	}}
	pc, err := lc.ListenPacket(context.Background(), "udp4", laddr.String())
	if err != nil {
		d.div.cleanup()
		log.Fatalf("listen on %s: %v", laddr, err)
	}
	d.link = pc.(*net.UDPConn)
	if err := d.serveControl(*control); err != nil {
		d.div.cleanup()
		log.Fatalf("control socket: %v", err)
	}
	log.Printf("idle; shim link on %s (accepting %s), control on %s", laddr, d.allow, *control)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go d.fromTun()
	go d.fromPeer()
	go d.procWatch()
	go d.watch()
	<-sig
	d.release()
	d.div.cleanup()
	log.Printf("stopped")
}

func (d *daemon) currentProfile() *Profile { return d.profile.Load() }

func (d *daemon) setProfile(p *Profile) {
	d.profile.Store(p)
	d.release()
	if p != nil {
		log.Printf("game selected: %s (processes %v, ExitLag sees %s)", p.Name, p.Linux, p.Windows)
	} else {
		log.Printf("no game selected")
	}
	select {
	case d.changed <- struct{}{}:
	default:
	}
}

func (d *daemon) peerAlive() bool {
	return time.Since(time.Unix(0, d.lastSeen.Load())) < 5*time.Second
}

// valvePrefixes returns the /24s of every relay in Valve's live SDR config,
// or the built-in list if it can't be fetched.
func valvePrefixes() []string {
	client := http.Client{Timeout: 10 * time.Second}
	var cfg struct {
		Pops map[string]struct {
			Relays []struct {
				IPv4 string `json:"ipv4"`
			} `json:"relays"`
		} `json:"pops"`
	}
	resp, err := client.Get(sdrConfigURL)
	if err == nil {
		err = json.NewDecoder(resp.Body).Decode(&cfg)
		resp.Body.Close()
	}
	set := map[string]bool{}
	for _, pop := range cfg.Pops {
		for _, r := range pop.Relays {
			if a, err := netip.ParseAddr(r.IPv4); err == nil && a.Is4() {
				set[netip.PrefixFrom(a, 24).Masked().String()] = true
			}
		}
	}
	if len(set) == 0 {
		log.Printf("couldn't fetch Valve's relay list (%v), using the built-in one", err)
		return fallbackPrefixes
	}
	out := make([]string, 0, len(set))
	for p := range set {
		out = append(out, p)
	}
	sort.Strings(out)
	return out
}

// fromTun forwards diverted game packets to the shim.
func (d *daemon) fromTun() {
	buf := make([]byte, 65535)
	out := make([]byte, 0, 65535)
	for {
		n, err := d.tun.Read(buf)
		if err != nil {
			log.Fatalf("read TUN: %v", err)
		}
		p, ok := parseUDP(buf[:n])
		peer := d.peer.Load()
		if !ok || peer == nil {
			continue
		}
		d.mu.Lock()
		d.srcAddrs[p.src.Port()] = p.src.Addr()
		d.mu.Unlock()

		out = wire.AppendData(out[:0], wire.Data{Addr: p.dst, HostPort: p.src.Port(), Payload: p.payload})
		d.link.WriteToUDPAddrPort(out, *peer)
		d.tx.Add(1)
	}
}

// fromPeer handles the shim's hellos and injects replies back into the TUN.
func (d *daemon) fromPeer() {
	buf := make([]byte, wire.MaxDatagram)
	out := make([]byte, 0, 65535)
	ack := wire.Control(wire.TypeAck)
	for {
		n, from, err := d.link.ReadFromUDPAddrPort(buf)
		if err != nil {
			continue
		}
		from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
		if !d.allow.Contains(from.Addr()) {
			continue
		}
		b := buf[:n]
		if wire.IsControl(b, wire.TypeHello) {
			if old := d.peer.Load(); old == nil || *old != from {
				log.Printf("shim at %s", from)
			}
			d.peer.Store(&from)
			d.lastSeen.Store(time.Now().UnixNano())
			d.link.WriteToUDPAddrPort(ack, from)
			exe := ""
			if p := d.profile.Load(); p != nil {
				exe = p.Windows
			}
			d.link.WriteToUDPAddrPort(wire.Game(exe), from)
			continue
		}
		data, err := wire.ParseData(b)
		if err != nil {
			continue
		}
		d.mu.Lock()
		dst, ok := d.srcAddrs[data.HostPort]
		d.mu.Unlock()
		if !ok {
			continue
		}
		out = appendUDP(out[:0], udpPacket{
			src:     data.Addr,
			dst:     netip.AddrPortFrom(dst, data.HostPort),
			payload: data.Payload,
		})
		d.tun.Write(out)
		d.rx.Add(1)
	}
}

// watch keeps the diversion in place while a game is selected and the
// shim is alive, and removes it otherwise so the game goes direct.
func (d *daemon) watch() {
	tick := time.NewTicker(time.Second)
	for {
		select {
		case <-tick.C:
		case <-d.changed:
		}
		want := d.currentProfile()
		if !d.peerAlive() {
			want = nil
		}
		d.divMu.Lock()
		if want != d.active {
			if d.active != nil {
				d.div.disable()
				d.diverting.Store(false)
				log.Printf("diversion off: %s traffic goes direct", d.active.Name)
				d.active = nil
			}
			if want != nil {
				if err := d.div.enable(want, d.scopeSnapshot()); err != nil {
					logErr("enable diversion", err)
					d.div.disable()
				} else {
					d.active = want
					d.diverting.Store(true)
					log.Printf("diversion on: %s traffic goes through ExitLag", want.Name)
				}
			}
		}
		d.divMu.Unlock()
	}
}

// reloadNft refreshes the marking rule (e.g. new app scopes) while diverting.
func (d *daemon) reloadNft() {
	d.divMu.Lock()
	defer d.divMu.Unlock()
	if d.active != nil && len(d.active.Linux) > 0 {
		logErr("update diversion", nft(d.div.nftScript(d.scopeSnapshot())))
	}
}

func must(err error) {
	if err != nil {
		log.Fatal(err)
	}
}
