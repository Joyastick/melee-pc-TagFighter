// SPDX-License-Identifier: GPL-3.0-or-later

// Wire format of the MeleeVS pairing server. It must match
// src/pc/net_rendezvous.h byte for byte.
//
// Every packet starts with "MPS1", a version byte and a type byte. IPv4
// addresses travel as their four bytes in network order, ports big-endian.
// Signed packets end in a 64-byte Ed25519 signature over everything before
// it, made with the server's key (the client has the public half compiled
// in). The server never answers with more bytes than the packet that caused
// the answer, so it cannot be used to amplify traffic at someone else.
package main

import (
	"crypto/ed25519"
	"encoding/binary"
	"net/netip"
)

const (
	magic   = "MPS1"
	version = 1
	hdrSize = 6
	sigSize = 64

	typeHello  = 'H' // client -> server: {nonce}, padded to cookieSize
	typeCookie = 'C' // server -> client: {nonce, cookie, observed ip:port, sig}
	typeJoin   = 'J' // client -> server: {nonce, cookie, topic, lan, want, avoid}, padded to matchSize
	typeQueued = 'Q' // server -> client: {nonce, fresh cookie, topic}
	typeLeave  = 'L' // client -> server: {nonce, cookie, topic}
	typeMatch  = 'M' // server -> client: {nonce, topic, peer public, peer lan, sig}
	typeStats  = 'S' // loopback only: plain-text counters

	nonceSize  = 8
	cookieSize = 16
	topicSize  = 20
	endpointSz = 6

	helloSize        = cookiePacketSize
	cookiePacketSize = hdrSize + nonceSize + cookieSize + endpointSz + sigSize // 100
	joinSize         = matchSize
	joinUsed         = hdrSize + nonceSize + cookieSize + topicSize + endpointSz + 1 + endpointSz // 67
	queuedSize       = hdrSize + nonceSize + cookieSize + topicSize                               // 50
	leaveSize        = queuedSize
	matchSize        = hdrSize + nonceSize + topicSize + endpointSz + endpointSz + sigSize // 110
)

type topic [topicSize]byte
type nonce [nonceSize]byte

func header(t byte, size int) []byte {
	b := make([]byte, size)
	copy(b, magic)
	b[4] = version
	b[5] = t
	return b
}

// packetType returns the type byte of a well-formed header, or 0.
func packetType(b []byte) byte {
	if len(b) < hdrSize || string(b[:4]) != magic || b[4] != version {
		return 0
	}
	return b[5]
}

func putEndpoint(b []byte, ap netip.AddrPort) {
	a := ap.Addr().As4()
	copy(b, a[:])
	binary.BigEndian.PutUint16(b[4:], ap.Port())
}

func getEndpoint(b []byte) netip.AddrPort {
	return netip.AddrPortFrom(netip.AddrFrom4([4]byte(b[:4])), binary.BigEndian.Uint16(b[4:6]))
}

func sign(key ed25519.PrivateKey, b []byte) {
	copy(b[len(b)-sigSize:], ed25519.Sign(key, b[:len(b)-sigSize]))
}

type joinRequest struct {
	nonce  nonce
	cookie [cookieSize]byte
	topic  topic
	lan    netip.AddrPort
	want   byte
	avoid  netip.AddrPort
}

func parseJoin(b []byte) (j joinRequest, ok bool) {
	if len(b) != joinSize {
		return j, false
	}
	p := b[hdrSize:]
	copy(j.nonce[:], p)
	p = p[nonceSize:]
	copy(j.cookie[:], p)
	p = p[cookieSize:]
	copy(j.topic[:], p)
	p = p[topicSize:]
	j.lan = getEndpoint(p)
	p = p[endpointSz:]
	j.want = p[0]
	j.avoid = getEndpoint(p[1:])
	return j, true
}

func encodeCookie(key ed25519.PrivateKey, n nonce, cookie [cookieSize]byte, observed netip.AddrPort) []byte {
	b := header(typeCookie, cookiePacketSize)
	p := b[hdrSize:]
	copy(p, n[:])
	copy(p[nonceSize:], cookie[:])
	putEndpoint(p[nonceSize+cookieSize:], observed)
	sign(key, b)
	return b
}

func encodeQueued(n nonce, cookie [cookieSize]byte, t topic) []byte {
	b := header(typeQueued, queuedSize)
	p := b[hdrSize:]
	copy(p, n[:])
	copy(p[nonceSize:], cookie[:])
	copy(p[nonceSize+cookieSize:], t[:])
	return b
}

func encodeMatch(key ed25519.PrivateKey, n nonce, t topic, peer, peerLAN netip.AddrPort) []byte {
	b := header(typeMatch, matchSize)
	p := b[hdrSize:]
	copy(p, n[:])
	copy(p[nonceSize:], t[:])
	putEndpoint(p[nonceSize+topicSize:], peer)
	putEndpoint(p[nonceSize+topicSize+endpointSz:], peerLAN)
	sign(key, b)
	return b
}
