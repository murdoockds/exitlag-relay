// Package wire defines the UDP protocol between exitlag-relayd and the
// nfapi.dll shim inside ExitLag (wine/nfshim), the "peer" below.
//
// Every datagram starts with a one-byte type:
//
//	Data  (host -> peer): type | dst IPv4 (4) | dst port (2) | host src port (2) | payload
//	Data  (peer -> host): type | remote IPv4 (4) | remote port (2) | host src port (2) | payload
//	Hello (peer -> host): type | Magic
//	Ack   (host -> peer): type | Magic
//	Game  (host -> peer): type | Magic | Windows exe name of the selected game
//	                    (sent with every Ack)
//
// Multi-byte fields are big-endian.
package wire

import (
	"encoding/binary"
	"errors"
	"net/netip"
)

const (
	TypeData  = 1
	TypeHello = 2
	TypeAck   = 3
	TypeGame  = 4

	DefaultPort = 27999
	HeaderLen   = 9
	// MaxDatagram bounds a relayed packet. The host TUN MTU is chosen so
	// that payload plus header always fits in one datagram on the link to the peer.
	MaxDatagram = 65535
)

var Magic = []byte("CS2R")

var errShort = errors.New("short datagram")

// Data is a relayed UDP payload together with its addressing.
type Data struct {
	Addr     netip.AddrPort // dst for host->peer, remote src for peer->host
	HostPort uint16         // UDP source port of the game on the host
	Payload  []byte
}

// AppendData encodes d into buf and returns the extended slice.
func AppendData(buf []byte, d Data) []byte {
	ip := d.Addr.Addr().As4()
	buf = append(buf, TypeData)
	buf = append(buf, ip[:]...)
	buf = binary.BigEndian.AppendUint16(buf, d.Addr.Port())
	buf = binary.BigEndian.AppendUint16(buf, d.HostPort)
	return append(buf, d.Payload...)
}

// ParseData decodes a TypeData datagram. The payload aliases b.
func ParseData(b []byte) (Data, error) {
	if len(b) < HeaderLen || b[0] != TypeData {
		return Data{}, errShort
	}
	ip := netip.AddrFrom4([4]byte(b[1:5]))
	return Data{
		Addr:     netip.AddrPortFrom(ip, binary.BigEndian.Uint16(b[5:7])),
		HostPort: binary.BigEndian.Uint16(b[7:9]),
		Payload:  b[HeaderLen:],
	}, nil
}

// Control builds a hello or ack datagram.
func Control(typ byte) []byte {
	return append([]byte{typ}, Magic...)
}

// IsControl reports whether b is a control datagram of the given type.
func IsControl(b []byte, typ byte) bool {
	return len(b) == 1+len(Magic) && b[0] == typ && string(b[1:]) == string(Magic)
}

// Game builds a Game datagram naming the selected game's Windows exe ("" for
// none).
func Game(exe string) []byte {
	return append(append([]byte{TypeGame}, Magic...), exe...)
}
