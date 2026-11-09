package main

import (
	"encoding/binary"
	"net/netip"
)

// udpPacket is an IPv4/UDP datagram read from or written to the TUN device.
type udpPacket struct {
	src, dst netip.AddrPort
	payload  []byte
}

// parseUDP extracts a UDP datagram from an IPv4 packet. Fragments and
// anything that isn't UDP are rejected; game packets are far below the MTU.
func parseUDP(b []byte) (udpPacket, bool) {
	if len(b) < 20 || b[0]>>4 != 4 {
		return udpPacket{}, false
	}
	ihl := int(b[0]&0x0f) * 4
	total := int(binary.BigEndian.Uint16(b[2:4]))
	if ihl < 20 || total > len(b) || total < ihl+8 || b[9] != 17 {
		return udpPacket{}, false
	}
	if binary.BigEndian.Uint16(b[6:8])&0x3fff != 0 { // MF flag or fragment offset
		return udpPacket{}, false
	}
	udp := b[ihl:total]
	ulen := int(binary.BigEndian.Uint16(udp[4:6]))
	if ulen < 8 || ulen > len(udp) {
		return udpPacket{}, false
	}
	return udpPacket{
		src:     netip.AddrPortFrom(netip.AddrFrom4([4]byte(b[12:16])), binary.BigEndian.Uint16(udp[0:2])),
		dst:     netip.AddrPortFrom(netip.AddrFrom4([4]byte(b[16:20])), binary.BigEndian.Uint16(udp[2:4])),
		payload: udp[8:ulen],
	}, true
}

// appendUDP builds an IPv4/UDP packet into buf.
func appendUDP(buf []byte, p udpPacket) []byte {
	ulen := 8 + len(p.payload)
	total := 20 + ulen
	src, dst := p.src.Addr().As4(), p.dst.Addr().As4()

	buf = append(buf,
		0x45, 0, byte(total>>8), byte(total), // version/IHL, TOS, total length
		0, 0, 0x40, 0, // ID, flags (DF), fragment offset
		64, 17, 0, 0, // TTL, protocol UDP, checksum
	)
	buf = append(buf, src[:]...)
	buf = append(buf, dst[:]...)
	ip := buf[len(buf)-20:]
	binary.BigEndian.PutUint16(ip[10:12], checksum(0, ip))

	buf = binary.BigEndian.AppendUint16(buf, p.src.Port())
	buf = binary.BigEndian.AppendUint16(buf, p.dst.Port())
	buf = binary.BigEndian.AppendUint16(buf, uint16(ulen))
	buf = append(buf, 0, 0)
	buf = append(buf, p.payload...)
	udp := buf[len(buf)-ulen:]

	// UDP checksum over the pseudo-header and the datagram.
	var pseudo [12]byte
	copy(pseudo[0:4], src[:])
	copy(pseudo[4:8], dst[:])
	pseudo[9] = 17
	binary.BigEndian.PutUint16(pseudo[10:12], uint16(ulen))
	sum := checksum(checksumAdd(0, pseudo[:]), udp)
	if sum == 0 {
		sum = 0xffff
	}
	binary.BigEndian.PutUint16(udp[6:8], sum)
	return buf
}

func checksumAdd(sum uint32, b []byte) uint32 {
	for len(b) >= 2 {
		sum += uint32(b[0])<<8 | uint32(b[1])
		b = b[2:]
	}
	if len(b) == 1 {
		sum += uint32(b[0]) << 8
	}
	return sum
}

func checksum(sum uint32, b []byte) uint16 {
	sum = checksumAdd(sum, b)
	for sum>>16 != 0 {
		sum = sum&0xffff + sum>>16
	}
	return ^uint16(sum)
}
