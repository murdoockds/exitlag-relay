package main

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"log"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
	"time"
)

// procWatch applies the active profile's process matchers every 250 ms:
//
//   - name/cmd matches are moved into the relay cgroup. Sockets are tagged
//     with the cgroup of the process that creates them, so a game has to be
//     moved before it opens its sockets (hence the frequent polling, and the
//     "attach" command for launchers that can do it up front).
//   - For Wine/Proton/Bottles processes the prefix's wineserver is moved too:
//     Wine creates sockets in wineserver and hands them to the game, so they
//     carry wineserver's cgroup, not the game's.
//   - scope: matches aren't moved. Their own cgroup (the app's systemd scope)
//     is added to the nftables rule instead, which leaves systemd's view of
//     the app intact (moving every process out empties the scope and systemd
//     deletes it).
func (d *daemon) procWatch() {
	for range time.Tick(250 * time.Millisecond) {
		p := d.currentProfile()
		if p == nil || len(p.Linux) == 0 {
			d.setScopes(nil, nil)
			continue
		}
		names, scopes := splitMatchers(p.Linux)
		ours := d.div.relCgroup()
		scopePaths := map[string]bool{}
		var scopePIDs []int
		prefixes := map[string]bool{}

		for _, pid := range userPIDs(d) {
			cg := cgroupOf(pid)
			if cg == "" {
				continue
			}
			if len(scopes) > 0 && cg != "/"+ours && containsAny(strings.ToLower(cg), scopes) {
				scopePaths[cg] = true
				scopePIDs = append(scopePIDs, pid)
				continue
			}
			if len(names) == 0 || !matchesName(pid, names) || d.ignoredPrefix(winePrefix(pid)) {
				continue
			}
			if cg != "/"+ours {
				if err := d.attach(pid); err != nil {
					log.Printf("move pid %d to cgroup: %v", pid, err)
				}
			}
			if pfx := winePrefix(pid); pfx != "" {
				prefixes[pfx] = true
			}
		}
		if len(prefixes) > 0 {
			for _, pid := range userPIDs(d) {
				if comm(pid) != "wineserver" || !prefixes[winePrefix(pid)] || cgroupOf(pid) == "/"+ours {
					continue
				}
				if err := d.attach(pid); err != nil {
					log.Printf("move wineserver %d to cgroup: %v", pid, err)
				}
			}
		}

		paths := make([]string, 0, len(scopePaths))
		for path := range scopePaths {
			paths = append(paths, path)
		}
		sort.Strings(paths)
		d.setScopes(paths, scopePIDs)
	}
}

// setScopes records the scope cgroups to divert and, if diversion is on,
// reloads the nftables rule when they changed.
func (d *daemon) setScopes(paths []string, pids []int) {
	d.mu.Lock()
	changed := !slices.Equal(paths, d.scopes)
	d.scopes = paths
	d.scopePIDs = pids
	d.mu.Unlock()
	if changed {
		if len(paths) > 0 {
			log.Printf("diverting app scopes: %s", strings.Join(paths, ", "))
		}
		d.reloadNft()
	}
}

func (d *daemon) scopeSnapshot() []string {
	d.mu.Lock()
	defer d.mu.Unlock()
	return slices.Clone(d.scopes)
}

// attach moves pid (and its future children) into the relay cgroup,
// remembering where it was so release can put it back.
func (d *daemon) attach(pid int) error {
	orig := cgroupOf(pid)
	if err := os.WriteFile(filepath.Join(d.div.cgroupDir, "cgroup.procs"), []byte(strconv.Itoa(pid)), 0o644); err != nil {
		return err
	}
	if orig != "" && orig != "/"+d.div.relCgroup() {
		d.mu.Lock()
		d.origCgroup[pid] = orig
		d.mu.Unlock()
		d.saveOrig()
	}
	log.Printf("process %d (%s) attached", pid, comm(pid))
	return nil
}

// release moves every process in the relay cgroup back to the cgroup it
// came from (children go back to their parent's). A cgroup that has since
// disappeared is recreated, so processes never stay behind.
func (d *daemon) release() {
	d.mu.Lock()
	orig := d.origCgroup
	d.origCgroup = map[int]string{}
	d.mu.Unlock()
	d.saveOrig()

	moved, lost := 0, 0
	for _, pid := range d.movedPIDs() {
		dest, ok := orig[pid]
		if !ok {
			dest = orig[ppid(pid)]
		}
		if dest == "" {
			// Unknown origin (e.g. moved by an older daemon): use the cgroup
			// of the nearest ancestor that isn't in the relay cgroup.
			dest = ancestorCgroup(pid, "/"+d.div.relCgroup())
		}
		if dest == "" {
			lost++
			continue
		}
		dir := filepath.Join(d.div.cgroupRoot, dest)
		err := os.WriteFile(filepath.Join(dir, "cgroup.procs"), []byte(strconv.Itoa(pid)), 0o644)
		if errors.Is(err, fs.ErrNotExist) {
			if err = os.MkdirAll(dir, 0o755); err == nil {
				err = os.WriteFile(filepath.Join(dir, "cgroup.procs"), []byte(strconv.Itoa(pid)), 0o644)
			}
		}
		if err != nil {
			log.Printf("return pid %d to %s: %v", pid, dest, err)
			lost++
			continue
		}
		moved++
	}
	if moved > 0 || lost > 0 {
		log.Printf("released %d processes (%d without a known origin stay in the relay cgroup)", moved, lost)
	}
}

// saveOrig persists the origin map so a daemon restart can still release.
func (d *daemon) saveOrig() {
	if d.statePath == "" {
		return
	}
	d.mu.Lock()
	b, _ := json.Marshal(d.origCgroup)
	d.mu.Unlock()
	logErr("save state", os.WriteFile(d.statePath, b, 0o600))
}

func (d *daemon) loadOrig() {
	if d.statePath == "" {
		return
	}
	b, err := os.ReadFile(d.statePath)
	if err != nil {
		return
	}
	var m map[int]string
	if json.Unmarshal(b, &m) == nil {
		d.mu.Lock()
		for k, v := range m {
			d.origCgroup[k] = v
		}
		d.mu.Unlock()
	}
}

// movedPIDs lists the processes in the relay cgroup.
func (d *daemon) movedPIDs() []int {
	data, err := os.ReadFile(filepath.Join(d.div.cgroupDir, "cgroup.procs"))
	if err != nil {
		return nil
	}
	var pids []int
	for _, f := range strings.Fields(string(data)) {
		if pid, err := strconv.Atoi(f); err == nil {
			pids = append(pids, pid)
		}
	}
	return pids
}

// attachedPIDs lists every diverted process: moved ones and scope matches.
func (d *daemon) attachedPIDs() []int {
	pids := d.movedPIDs()
	d.mu.Lock()
	pids = append(pids, d.scopePIDs...)
	d.mu.Unlock()
	sort.Ints(pids)
	return slices.Compact(pids)
}

// ownedByUser reports whether pid belongs to the control user; with no
// control user configured, any non-root process qualifies.
func (d *daemon) ownedByUser(pid int) bool {
	uid, ok := procUID(pid)
	if !ok {
		return false
	}
	if d.controlUID >= 0 {
		return uid == d.controlUID
	}
	return uid != 0
}

func userPIDs(d *daemon) []int {
	entries, err := os.ReadDir("/proc")
	if err != nil {
		return nil
	}
	var pids []int
	for _, e := range entries {
		pid, err := strconv.Atoi(e.Name())
		if err != nil || pid == os.Getpid() || !d.ownedByUser(pid) {
			continue
		}
		pids = append(pids, pid)
	}
	return pids
}

func procUID(pid int) (int, bool) {
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/status", pid))
	if err != nil {
		return 0, false
	}
	for _, line := range strings.Split(string(b), "\n") {
		if rest, ok := strings.CutPrefix(line, "Uid:"); ok {
			if fields := strings.Fields(rest); len(fields) > 0 {
				uid, err := strconv.Atoi(fields[0])
				return uid, err == nil
			}
		}
	}
	return 0, false
}

func comm(pid int) string {
	b, _ := os.ReadFile(fmt.Sprintf("/proc/%d/comm", pid))
	return strings.TrimSpace(string(b))
}

// winePrefix returns the process's WINEPREFIX (without trailing slash).
func winePrefix(pid int) string {
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/environ", pid))
	if err != nil {
		return ""
	}
	for _, kv := range bytes.Split(b, []byte{0}) {
		if v, ok := bytes.CutPrefix(kv, []byte("WINEPREFIX=")); ok {
			return strings.TrimRight(string(v), "/")
		}
	}
	return ""
}

// ignoredPrefix reports whether a Wine prefix lies inside one of
// -ignore-wine-prefix's directories.
func (d *daemon) ignoredPrefix(prefix string) bool {
	if prefix == "" {
		return false
	}
	for _, dir := range d.ignorePrefixes {
		if prefix == dir || strings.HasPrefix(prefix, dir+"/") {
			return true
		}
	}
	return false
}

func splitDirs(s string) []string {
	var dirs []string
	for _, dir := range strings.Split(s, ",") {
		if dir = strings.TrimRight(strings.TrimSpace(dir), "/"); dir != "" {
			dirs = append(dirs, filepath.Clean(dir))
		}
	}
	return dirs
}

// splitMatchers separates scope:text entries (lowercased text) from the rest.
//
//	name        comm (truncated by the kernel to 15 characters) or the
//	            basename of argv[0], which under Wine/Proton is the Windows
//	            path of the game's .exe
//	cmd:text    text anywhere in the command line
//	scope:text  the process's cgroup path (systemd scope) contains text, e.g.
//	            "scope:vesktop" for an app niri started as app-vesktop-….scope
//	            (Electron apps' processes are all called "electron")
func splitMatchers(in []string) (names, scopes []string) {
	for _, n := range in {
		n = strings.ToLower(strings.TrimSpace(n))
		if text, ok := strings.CutPrefix(n, "scope:"); ok {
			if text != "" {
				scopes = append(scopes, text)
			}
		} else if n != "" {
			names = append(names, n)
		}
	}
	return names, scopes
}

func containsAny(s string, subs []string) bool {
	for _, sub := range subs {
		if strings.Contains(s, sub) {
			return true
		}
	}
	return false
}

// matchesName reports whether the process matches a name or cmd: entry.
func matchesName(pid int, names []string) bool {
	c := strings.ToLower(comm(pid))
	if c == "" {
		return false
	}
	var argv0, cmdline string
	if b, err := os.ReadFile(fmt.Sprintf("/proc/%d/cmdline", pid)); err == nil {
		first, _, _ := bytes.Cut(b, []byte{0})
		argv0 = strings.ToLower(string(first))
		if i := strings.LastIndexAny(argv0, `/\`); i >= 0 {
			argv0 = argv0[i+1:]
		}
		cmdline = strings.ToLower(string(bytes.ReplaceAll(b, []byte{0}, []byte{' '})))
	}
	for _, n := range names {
		if text, ok := strings.CutPrefix(n, "cmd:"); ok {
			if text != "" && strings.Contains(cmdline, text) {
				return true
			}
			continue
		}
		short := n
		if len(short) > 15 {
			short = short[:15]
		}
		if c == short || argv0 == n {
			return true
		}
	}
	return false
}

// cgroupOf returns pid's cgroup v2 path relative to the cgroup root.
func cgroupOf(pid int) string {
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/cgroup", pid))
	if err != nil {
		return ""
	}
	path, ok := strings.CutPrefix(strings.TrimSpace(string(b)), "0::")
	if !ok {
		return ""
	}
	return path
}

// ancestorCgroup walks up pid's parents to the first one outside skip. A
// systemd manager's init.scope is reserved for the manager itself; an app
// whose nearest ancestor is the user manager (e.g. one niri started) goes to
// the manager's app.slice instead, in exitlag-released.scope.
func ancestorCgroup(pid int, skip string) string {
	for i := 0; i < 64 && pid > 1; i++ {
		pid = ppid(pid)
		cg := cgroupOf(pid)
		if cg == "" || cg == skip {
			continue
		}
		if manager, ok := strings.CutSuffix(cg, "/init.scope"); ok {
			if strings.Contains(manager, "/user@") {
				return manager + "/app.slice/exitlag-released.scope"
			}
			continue // the system manager: keep looking (and likely give up)
		}
		return cg
	}
	return ""
}

func ppid(pid int) int {
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		return 0
	}
	// pid (comm) state ppid ...: comm may contain spaces, so split after ')'.
	s := string(b)
	fields := strings.Fields(s[strings.LastIndexByte(s, ')')+1:])
	if len(fields) < 2 {
		return 0
	}
	n, _ := strconv.Atoi(fields[1])
	return n
}
