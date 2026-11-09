package main

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

const (
	iffTun    = 0x0001
	iffNoPI   = 0x1000
	tunSetIff = 0x400454ca
)

// openTun creates a TUN device that exists for as long as the returned file
// stays open.
func openTun(name string) (*os.File, error) {
	fd, err := syscall.Open("/dev/net/tun", syscall.O_RDWR|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, err
	}
	var ifr struct {
		name  [16]byte
		flags uint16
		_     [22]byte
	}
	if len(name) >= len(ifr.name) {
		syscall.Close(fd)
		return nil, fmt.Errorf("interface name %q too long", name)
	}
	copy(ifr.name[:], name)
	ifr.flags = iffTun | iffNoPI
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), tunSetIff, uintptr(unsafe.Pointer(&ifr))); errno != 0 {
		syscall.Close(fd)
		return nil, fmt.Errorf("TUNSETIFF: %w", errno)
	}
	return os.NewFile(uintptr(fd), "/dev/net/tun"), nil
}
