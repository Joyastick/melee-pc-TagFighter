// SPDX-License-Identifier: GPL-3.0-or-later
package main

import (
	"crypto/ed25519"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"net/netip"
	"time"
)

const (
	cookieWindow  = 30 * time.Second // a cookie is good for this window and the one before
	entryLifetime = 30 * time.Second // a queued client that stops re-joining is dropped
	rateBurst     = 40               // packets per source IP ...
	ratePerSecond = 20               // ... refilled at this rate
	maxPerIP      = 4                // queued entries one IP may hold (several players behind one NAT)
	maxEntries    = 100000           // global cap, far beyond any real load
)

// Out is one datagram to send.
type Out struct {
	To   netip.AddrPort
	Data []byte
}

type entry struct {
	addr     netip.AddrPort
	nonce    nonce
	topic    topic
	lan      netip.AddrPort
	want     byte
	avoid    netip.AddrPort
	lastSeen time.Time
}

type bucket struct {
	tokens float64
	last   time.Time
}

// Server holds the queues. It is not safe for concurrent use: main.go runs
// it from one goroutine.
type Server struct {
	key     ed25519.PrivateKey
	secret  [32]byte
	entries map[netip.AddrPort]*entry
	queues  map[topic][]*entry
	perIP   map[netip.Addr]int
	buckets map[netip.Addr]*bucket
	matches uint64
}

func NewServer(key ed25519.PrivateKey) *Server {
	s := &Server{
		key:     key,
		entries: map[netip.AddrPort]*entry{},
		queues:  map[topic][]*entry{},
		perIP:   map[netip.Addr]int{},
		buckets: map[netip.Addr]*bucket{},
	}
	// Cookies only need to outlive a few round trips, so the secret is
	// fresh per process: a restart just makes clients fetch a new cookie.
	if _, err := rand.Read(s.secret[:]); err != nil {
		panic(err)
	}
	return s
}

func (s *Server) cookie(from netip.AddrPort, window int64) (c [cookieSize]byte) {
	m := hmac.New(sha256.New, s.secret[:])
	var b [8 + endpointSz]byte
	binary.BigEndian.PutUint64(b[:], uint64(window))
	putEndpoint(b[8:], from)
	m.Write(b[:])
	copy(c[:], m.Sum(nil))
	return c
}

func (s *Server) cookieOK(c [cookieSize]byte, from netip.AddrPort, now time.Time) bool {
	w := now.Unix() / int64(cookieWindow/time.Second)
	a, b := s.cookie(from, w), s.cookie(from, w-1)
	return hmac.Equal(c[:], a[:]) || hmac.Equal(c[:], b[:])
}

func (s *Server) currentCookie(from netip.AddrPort, now time.Time) [cookieSize]byte {
	return s.cookie(from, now.Unix()/int64(cookieWindow/time.Second))
}

func (s *Server) allow(ip netip.Addr, now time.Time) bool {
	b := s.buckets[ip]
	if b == nil {
		b = &bucket{tokens: rateBurst, last: now}
		s.buckets[ip] = b
	}
	if now.After(b.last) {
		b.tokens += now.Sub(b.last).Seconds() * ratePerSecond
		b.last = now
	}
	if b.tokens > rateBurst {
		b.tokens = rateBurst
	}
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// Handle processes one datagram and returns what to send in answer.
func (s *Server) Handle(pkt []byte, from netip.AddrPort, now time.Time) []Out {
	from = netip.AddrPortFrom(from.Addr().Unmap(), from.Port())
	if !from.Addr().Is4() {
		return nil
	}
	t := packetType(pkt)
	if t == typeStats && from.Addr().IsLoopback() {
		return []Out{{from, []byte(s.Stats())}}
	}
	if t == 0 || !s.allow(from.Addr(), now) {
		return nil
	}
	switch t {
	case typeHello:
		if len(pkt) != helloSize {
			return nil
		}
		var n nonce
		copy(n[:], pkt[hdrSize:])
		return []Out{{from, encodeCookie(s.key, n, s.currentCookie(from, now), from)}}
	case typeJoin:
		j, ok := parseJoin(pkt)
		if !ok || !s.cookieOK(j.cookie, from, now) {
			return nil
		}
		return s.join(j, from, now)
	case typeLeave:
		if len(pkt) != leaveSize {
			return nil
		}
		var c [cookieSize]byte
		copy(c[:], pkt[hdrSize+nonceSize:])
		if !s.cookieOK(c, from, now) {
			return nil
		}
		if e := s.entries[from]; e != nil {
			s.remove(e)
		}
	}
	return nil
}

func (s *Server) join(j joinRequest, from netip.AddrPort, now time.Time) []Out {
	if j.want != 2 {
		return nil // groups of four arrive with 3-4 player online
	}
	if e := s.entries[from]; e != nil {
		if e.topic == j.topic && e.nonce == j.nonce {
			// A keepalive: stay in line, and hand back a fresh cookie.
			e.lastSeen = now
			e.avoid = j.avoid
			return []Out{{from, encodeQueued(j.nonce, s.currentCookie(from, now), j.topic)}}
		}
		s.remove(e) // a new search from the same socket replaces the old one
	}
	me := &entry{addr: from, nonce: j.nonce, topic: j.topic, lan: j.lan, want: j.want,
		avoid: j.avoid, lastSeen: now}
	if peer := s.partner(me, now); peer != nil {
		s.remove(peer)
		s.matches++
		return []Out{
			{me.addr, encodeMatch(s.key, me.nonce, me.topic, peer.addr, peer.lan)},
			{peer.addr, encodeMatch(s.key, peer.nonce, peer.topic, me.addr, me.lan)},
		}
	}
	if s.perIP[from.Addr()] >= maxPerIP || len(s.entries) >= maxEntries {
		return nil
	}
	s.entries[from] = me
	s.queues[me.topic] = append(s.queues[me.topic], me)
	s.perIP[from.Addr()]++
	return []Out{{from, encodeQueued(j.nonce, s.currentCookie(from, now), j.topic)}}
}

// partner is the longest-waiting live entry on the same topic that neither
// side asked to avoid (the client names the peer whose attempt just failed).
func (s *Server) partner(me *entry, now time.Time) *entry {
	for _, e := range s.queues[me.topic] {
		if e.addr == me.addr || e.want != me.want || now.Sub(e.lastSeen) > entryLifetime {
			continue
		}
		if e.avoid == me.addr || me.avoid == e.addr {
			continue
		}
		return e
	}
	return nil
}

func (s *Server) remove(e *entry) {
	if s.entries[e.addr] != e {
		return
	}
	delete(s.entries, e.addr)
	q := s.queues[e.topic]
	for i, x := range q {
		if x == e {
			q = append(q[:i], q[i+1:]...)
			break
		}
	}
	if len(q) == 0 {
		delete(s.queues, e.topic)
	} else {
		s.queues[e.topic] = q
	}
	if s.perIP[e.addr.Addr()]--; s.perIP[e.addr.Addr()] <= 0 {
		delete(s.perIP, e.addr.Addr())
	}
}

// Sweep drops entries that stopped re-joining and idle rate buckets.
func (s *Server) Sweep(now time.Time) {
	for _, e := range s.entries {
		if now.Sub(e.lastSeen) > entryLifetime {
			s.remove(e)
		}
	}
	for ip, b := range s.buckets {
		if now.Sub(b.last) > time.Minute {
			delete(s.buckets, ip)
		}
	}
}

func (s *Server) Stats() string {
	return fmt.Sprintf("queued %d topics %d matches %d\n", len(s.entries), len(s.queues), s.matches)
}
