package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"log"
	"net"
	"os"
	"syscall"
)

// Control protocol: one JSON object per line in each direction.
//
//	{"cmd":"status"}
//	{"cmd":"set","profile":{...}}   activate a game
//	{"cmd":"clear"}                 deactivate
//	{"cmd":"attach","pid":1234}     move a process (e.g. a launcher) into the cgroup
type request struct {
	Cmd     string   `json:"cmd"`
	Profile *Profile `json:"profile,omitempty"`
	PID     int      `json:"pid,omitempty"`
}

type status struct {
	Profile   *Profile `json:"profile"`
	ExitLagUp bool     `json:"exitlag_up"`
	PeerAddr  string   `json:"peer_addr,omitempty"`
	Diverting bool     `json:"diverting"`
	PIDs      []int    `json:"pids"`
	TxPackets uint64   `json:"tx_packets"`
	RxPackets uint64   `json:"rx_packets"`
}

type response struct {
	OK     bool    `json:"ok"`
	Error  string  `json:"error,omitempty"`
	Status *status `json:"status,omitempty"`
}

func (d *daemon) serveControl(path string) error {
	os.Remove(path)
	l, err := net.Listen("unix", path)
	if err != nil {
		return err
	}
	if d.controlUID >= 0 {
		if err := os.Chown(path, d.controlUID, -1); err != nil {
			return err
		}
	}
	if err := os.Chmod(path, 0o600); err != nil {
		return err
	}
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				log.Printf("control: %v", err)
				return
			}
			go d.handleControl(c.(*net.UnixConn))
		}
	}()
	return nil
}

func (d *daemon) handleControl(c *net.UnixConn) {
	defer c.Close()
	if !d.peerAllowed(c) {
		return
	}
	sc := bufio.NewScanner(c)
	enc := json.NewEncoder(c)
	for sc.Scan() {
		var req request
		var resp response
		if err := json.Unmarshal(sc.Bytes(), &req); err != nil {
			resp.Error = "bad request: " + err.Error()
		} else if err := d.do(req); err != nil {
			resp.Error = err.Error()
		} else {
			resp.OK = true
		}
		resp.Status = d.status()
		if enc.Encode(resp) != nil {
			return
		}
	}
}

// peerAllowed accepts root and the control user.
func (d *daemon) peerAllowed(c *net.UnixConn) bool {
	raw, err := c.SyscallConn()
	if err != nil {
		return false
	}
	var cred *syscall.Ucred
	raw.Control(func(fd uintptr) {
		cred, err = syscall.GetsockoptUcred(int(fd), syscall.SOL_SOCKET, syscall.SO_PEERCRED)
	})
	if err != nil || cred == nil {
		return false
	}
	uid := int(cred.Uid)
	return uid == 0 || d.controlUID < 0 || uid == d.controlUID
}

func (d *daemon) do(req request) error {
	switch req.Cmd {
	case "status":
		return nil
	case "set":
		if req.Profile == nil || (len(req.Profile.Linux) == 0 && len(req.Profile.Prefixes) == 0) {
			return errors.New("profile needs process names or prefixes")
		}
		d.setProfile(req.Profile)
		return nil
	case "clear":
		d.setProfile(nil)
		return nil
	case "attach":
		if req.PID <= 0 || !d.ownedByUser(req.PID) {
			return errors.New("can only attach your own processes")
		}
		return d.attach(req.PID)
	default:
		return errors.New("unknown command " + req.Cmd)
	}
}

func (d *daemon) status() *status {
	s := &status{
		Profile:   d.currentProfile(),
		ExitLagUp: d.peerAlive(),
		Diverting: d.diverting.Load(),
		PIDs:      d.attachedPIDs(),
		TxPackets: d.tx.Load(),
		RxPackets: d.rx.Load(),
	}
	if peer := d.peer.Load(); peer != nil {
		s.PeerAddr = peer.String()
	}
	return s
}
