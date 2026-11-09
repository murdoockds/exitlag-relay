package main

import (
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// Profile is a game selected in the GUI.
type Profile struct {
	Name string `json:"name"`
	// Linux process names whose UDP traffic is diverted (comm, or the
	// basename of argv[0], e.g. "cs2" or, under Proton, "r5apex.exe").
	Linux []string `json:"linux"`
	// Windows executable the game is known as in ExitLag (the shim's
	// stand-in process gets this name).
	Windows string `json:"windows"`
	// Optional destination prefixes diverted for every process ("valve-sdr"
	// expands to Valve's relay network).
	Prefixes []string `json:"prefixes,omitempty"`
}

// diverter installs and removes the routing that sends a profile's traffic
// into the TUN device:
//
//   - by process: sockets created in our cgroup get an nftables mark, and
//     marked packets are routed via the TUN (table markTable);
//   - by destination: Prefixes are routed via the TUN for UDP (prefixTable).
type diverter struct {
	ifname      string
	mark        string
	markTable   string
	markPrio    string
	prefixTable string
	prefixPrio  string
	cgroupDir   string
	cgroupRoot  string
}

const nftTable = "exitlag_relay"

// setup prepares state that doesn't depend on a profile.
func (v *diverter) setup() error {
	v.disable()
	if err := os.MkdirAll(v.cgroupDir, 0o755); err != nil {
		return fmt.Errorf("create cgroup: %w", err)
	}
	return nil
}

// enable brings the TUN up and installs the profile's routing. The TUN has
// no address and is down while idle: apps that enumerate interfaces (WebRTC,
// e.g. Discord voice) would otherwise offer it as a path and fail over it.
func (v *diverter) enable(p *Profile, scopes []string) error {
	if err := ip("link", "set", v.ifname, "up"); err != nil {
		return err
	}
	// Replies arrive on the TUN, which has no address: any reverse-path
	// filtering drops them. systemd-udevd re-applies sysctl defaults
	// (rp_filter = 2) to new interfaces after we set it, so set it here.
	if err := os.WriteFile("/proc/sys/net/ipv4/conf/"+v.ifname+"/rp_filter", []byte("0"), 0o644); err != nil {
		return err
	}
	if len(p.Linux) > 0 {
		if err := ip("route", "replace", "default", "dev", v.ifname, "table", v.markTable); err != nil {
			return err
		}
		if err := nft(v.nftScript(scopes)); err != nil {
			return err
		}
		if err := ip("rule", "add", "priority", v.markPrio, "fwmark", v.mark, "lookup", v.markTable); err != nil {
			return err
		}
	}
	prefixes := expandPrefixes(p.Prefixes)
	for _, pre := range prefixes {
		if err := ip("route", "replace", pre, "dev", v.ifname, "table", v.prefixTable); err != nil {
			return err
		}
	}
	if len(prefixes) > 0 {
		if err := ip("rule", "add", "priority", v.prefixPrio, "ipproto", "udp", "lookup", v.prefixTable); err != nil {
			return err
		}
	}
	return nil
}

// disable removes everything enable installed. Safe to call repeatedly.
func (v *diverter) disable() {
	for ip("rule", "del", "priority", v.markPrio) == nil {
	}
	for ip("rule", "del", "priority", v.prefixPrio) == nil {
	}
	nft("delete table inet " + nftTable)
	ip("route", "flush", "table", v.prefixTable)
	ip("route", "flush", "table", v.markTable)
	ip("link", "set", v.ifname, "down")
}

func (v *diverter) cleanup() {
	v.disable()
}

func (v *diverter) relCgroup() string {
	rel, _ := filepath.Rel(v.cgroupRoot, v.cgroupDir)
	return rel
}

// nftScript marks UDP sent by sockets in our cgroup or in one of the given
// app scopes, except to local, private and multicast addresses (LAN games,
// DNS to the router, ...). It replaces the table atomically.
func (v *diverter) nftScript(scopes []string) string {
	var rules strings.Builder
	for _, cg := range append([]string{"/" + v.relCgroup()}, scopes...) {
		rel := strings.TrimPrefix(cg, "/")
		if _, err := os.Stat(filepath.Join(v.cgroupRoot, rel)); err != nil {
			continue // the app exited; nft can't resolve a missing cgroup
		}
		fmt.Fprintf(&rules, "\t\tmeta l4proto udp ip daddr != @private4 socket cgroupv2 level %d %q meta mark set %s\n",
			len(strings.Split(rel, "/")), rel, v.mark)
	}
	return fmt.Sprintf(`table inet %[1]s {}
delete table inet %[1]s
table inet %[1]s {
	set private4 {
		type ipv4_addr; flags interval
		elements = { 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10, 127.0.0.0/8,
		             169.254.0.0/16, 172.16.0.0/12, 192.168.0.0/16, 224.0.0.0/3 }
	}
	chain output {
		type route hook output priority mangle; policy accept;
%[2]s	}
}
`, nftTable, rules.String())
}

func expandPrefixes(in []string) []string {
	var out []string
	for _, p := range in {
		if p == "valve-sdr" {
			out = append(out, valvePrefixes()...)
		} else {
			out = append(out, p)
		}
	}
	return out
}

func ip(args ...string) error {
	out, err := exec.Command("ip", args...).CombinedOutput()
	if err != nil {
		return fmt.Errorf("ip %s: %v: %s", strings.Join(args, " "), err, strings.TrimSpace(string(out)))
	}
	return nil
}

func nft(script string) error {
	cmd := exec.Command("nft", "-f", "-")
	cmd.Stdin = strings.NewReader(script)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("nft: %v: %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}

func logErr(what string, err error) {
	if err != nil {
		log.Printf("%s: %v", what, err)
	}
}
