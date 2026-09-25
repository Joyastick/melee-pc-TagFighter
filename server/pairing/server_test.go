// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"crypto/ed25519"
	"net/netip"
	"testing"
	"time"
)

var (
	testKey = ed25519.NewKeyFromSeed(make([]byte, ed25519.SeedSize))
	t0      = time.Unix(1_800_000_000, 0)
	alice   = netip.MustParseAddrPort("203.0.113.5:40000")
	bob     = netip.MustParseAddrPort("198.51.100.7:50000")
	carol   = netip.MustParseAddrPort("192.0.2.9:60000")
)

func hello(n byte) []byte {
	b := header(typeHello, helloSize)
	b[hdrSize] = n
	return b
}

// cookieFor runs HELLO -> COOKIE and returns the cookie.
func cookieFor(t *testing.T, s *Server, from netip.AddrPort, now time.Time) (c [cookieSize]byte) {
	t.Helper()
	out := s.Handle(hello(1), from, now)
	if len(out) != 1 || len(out[0].Data) != cookiePacketSize || out[0].To != from {
		t.Fatalf("no cookie for %v: %v", from, out)
	}
	d := out[0].Data
	if !ed25519.Verify(testKey.Public().(ed25519.PublicKey), d[:len(d)-sigSize], d[len(d)-sigSize:]) {
		t.Fatal("cookie packet not signed")
	}
	if got := getEndpoint(d[hdrSize+nonceSize+cookieSize:]); got != from {
		t.Fatalf("observed endpoint %v, want %v", got, from)
	}
	copy(c[:], d[hdrSize+nonceSize:])
	return c
}

func join(c [cookieSize]byte, n byte, tp byte, lan, avoid netip.AddrPort) []byte {
	b := header(typeJoin, joinSize)
	p := b[hdrSize:]
	p[0] = n
	copy(p[nonceSize:], c[:])
	p[nonceSize+cookieSize] = tp
	putEndpoint(p[nonceSize+cookieSize+topicSize:], lan)
	p[nonceSize+cookieSize+topicSize+endpointSz] = 2
	putEndpoint(p[nonceSize+cookieSize+topicSize+endpointSz+1:], avoid)
	return b
}

var lan0 = netip.MustParseAddrPort("0.0.0.0:0")

func TestPairsTwoOnSameTopic(t *testing.T) {
	s := NewServer(testKey)
	ca, cb := cookieFor(t, s, alice, t0), cookieFor(t, s, bob, t0)
	lanA := netip.MustParseAddrPort("192.168.1.10:40000")
	out := s.Handle(join(ca, 1, 7, lanA, lan0), alice, t0)
	if len(out) != 1 || packetType(out[0].Data) != typeQueued {
		t.Fatalf("first join: %v", out)
	}
	out = s.Handle(join(cb, 2, 7, lan0, lan0), bob, t0)
	if len(out) != 2 {
		t.Fatalf("second join should match both: %v", out)
	}
	for _, o := range out {
		d := o.Data
		if packetType(d) != typeMatch || len(d) != matchSize {
			t.Fatalf("not a MATCH: %v", d)
		}
		if !ed25519.Verify(testKey.Public().(ed25519.PublicKey), d[:len(d)-sigSize], d[len(d)-sigSize:]) {
			t.Fatal("MATCH not signed")
		}
		peer := getEndpoint(d[hdrSize+nonceSize+topicSize:])
		plan := getEndpoint(d[hdrSize+nonceSize+topicSize+endpointSz:])
		switch o.To {
		case alice:
			if peer != bob || d[hdrSize] != 1 {
				t.Fatalf("alice told %v nonce %d", peer, d[hdrSize])
			}
		case bob:
			if peer != alice || plan != lanA || d[hdrSize] != 2 {
				t.Fatalf("bob told %v lan %v nonce %d", peer, plan, d[hdrSize])
			}
		default:
			t.Fatalf("MATCH to %v", o.To)
		}
	}
	if len(s.entries) != 0 || len(s.queues) != 0 || len(s.perIP) != 0 {
		t.Fatalf("queues not empty after match: %s", s.Stats())
	}
}

func TestDifferentTopicsDoNotPair(t *testing.T) {
	s := NewServer(testKey)
	s.Handle(join(cookieFor(t, s, alice, t0), 1, 7, lan0, lan0), alice, t0)
	out := s.Handle(join(cookieFor(t, s, bob, t0), 2, 8, lan0, lan0), bob, t0)
	if len(out) != 1 || packetType(out[0].Data) != typeQueued || len(s.entries) != 2 {
		t.Fatalf("different topics paired: %v", out)
	}
}

func TestCookieChecks(t *testing.T) {
	s := NewServer(testKey)
	c := cookieFor(t, s, alice, t0)
	if out := s.Handle(join(c, 1, 7, lan0, lan0), bob, t0); out != nil {
		t.Fatal("cookie accepted from another address")
	}
	other := netip.AddrPortFrom(alice.Addr(), alice.Port()+1)
	if out := s.Handle(join(c, 1, 7, lan0, lan0), other, t0); out != nil {
		t.Fatal("cookie accepted from another port")
	}
	if out := s.Handle(join(c, 1, 7, lan0, lan0), alice, t0.Add(29*time.Second)); len(out) != 1 {
		t.Fatal("fresh cookie refused")
	}
	if out := s.Handle(join(c, 2, 7, lan0, lan0), alice, t0.Add(90*time.Second)); out != nil {
		t.Fatal("expired cookie accepted")
	}
}

func TestKeepaliveRefreshesAndExpiry(t *testing.T) {
	s := NewServer(testKey)
	c := cookieFor(t, s, alice, t0)
	s.Handle(join(c, 1, 7, lan0, lan0), alice, t0)
	now := t0
	for i := 0; i < 6; i++ { // a minute of keepalives every 10 s
		now = now.Add(10 * time.Second)
		out := s.Handle(join(c, 1, 7, lan0, lan0), alice, now)
		if len(out) != 1 || packetType(out[0].Data) != typeQueued {
			t.Fatalf("keepalive %d refused", i)
		}
		copy(c[:], out[0].Data[hdrSize+nonceSize:]) // the fresh cookie
		s.Sweep(now)
	}
	if len(s.entries) != 1 {
		t.Fatal("kept-alive entry dropped")
	}
	s.Sweep(now.Add(entryLifetime + time.Second))
	if len(s.entries) != 0 || len(s.perIP) != 0 {
		t.Fatal("silent entry not dropped")
	}
}

func TestAvoidSkipsFailedPeer(t *testing.T) {
	s := NewServer(testKey)
	s.Handle(join(cookieFor(t, s, alice, t0), 1, 7, lan0, lan0), alice, t0)
	out := s.Handle(join(cookieFor(t, s, bob, t0), 2, 7, lan0, alice), bob, t0)
	if len(out) != 1 || packetType(out[0].Data) != typeQueued {
		t.Fatal("paired with the peer it asked to avoid")
	}
	out = s.Handle(join(cookieFor(t, s, carol, t0), 3, 7, lan0, lan0), carol, t0)
	if len(out) != 2 || !(out[1].To == alice) {
		t.Fatalf("carol should meet the longest waiter, alice: %v", out)
	}
}

func TestLeave(t *testing.T) {
	s := NewServer(testKey)
	c := cookieFor(t, s, alice, t0)
	s.Handle(join(c, 1, 7, lan0, lan0), alice, t0)
	b := header(typeLeave, leaveSize)
	copy(b[hdrSize+nonceSize:], c[:])
	s.Handle(b, alice, t0)
	if len(s.entries) != 0 {
		t.Fatal("LEAVE ignored")
	}
}

func TestNoAmplificationAndJunk(t *testing.T) {
	s := NewServer(testKey)
	if out := s.Handle(hello(1)[:helloSize-1], alice, t0); out != nil {
		t.Fatal("short HELLO answered")
	}
	if out := s.Handle([]byte("junk"), alice, t0); out != nil {
		t.Fatal("junk answered")
	}
	if out := s.Handle(header(typeStats, hdrSize), alice, t0); out != nil {
		t.Fatal("STATS answered off loopback")
	}
	c := cookieFor(t, s, alice, t0)
	for _, pkt := range [][]byte{hello(1), join(c, 1, 7, lan0, lan0)} {
		for _, o := range s.Handle(pkt, alice, t0) {
			if o.To == alice && len(o.Data) > len(pkt) {
				t.Fatalf("answer of %d bytes to a %d-byte request", len(o.Data), len(pkt))
			}
		}
	}
}

func TestRateLimitAndPerIPCap(t *testing.T) {
	s := NewServer(testKey)
	answered := 0
	for i := 0; i < 100; i++ {
		if s.Handle(hello(1), alice, t0) != nil {
			answered++
		}
	}
	if answered != rateBurst {
		t.Fatalf("answered %d of a burst, want %d", answered, rateBurst)
	}
	later := t0.Add(time.Minute)
	for p := uint16(1); p <= maxPerIP+2; p++ {
		ap := netip.AddrPortFrom(bob.Addr(), p)
		s.Handle(join(cookieFor(t, s, ap, later), 1, byte(p), lan0, lan0), ap, later)
	}
	if s.perIP[bob.Addr()] != maxPerIP {
		t.Fatalf("one IP holds %d entries, cap %d", s.perIP[bob.Addr()], maxPerIP)
	}
}

func TestWireSizes(t *testing.T) {
	if cookiePacketSize != 100 || matchSize != 110 || queuedSize != 50 || joinUsed > joinSize {
		t.Fatalf("sizes changed: cookie %d match %d queued %d join %d/%d",
			cookiePacketSize, matchSize, queuedSize, joinUsed, joinSize)
	}
}
