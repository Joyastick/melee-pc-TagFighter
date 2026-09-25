// SPDX-License-Identifier: GPL-3.0-or-later

// Command pairing is the MeleeVS pairing server: it introduces two players
// who asked for the same topic and never carries game traffic. See the
// "Phase C" plan and src/pc/net_rendezvous.c for the client side.
//
//	pairing -genkey server.key       write a new signing key, print its public half
//	pairing -key server.key [-listen :27720]
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"strings"
	"time"
)

func loadKey(path string) (ed25519.PrivateKey, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	seed, err := hex.DecodeString(strings.TrimSpace(string(b)))
	if err != nil || len(seed) != ed25519.SeedSize {
		return nil, errors.New("key file must hold 64 hex digits (an Ed25519 seed)")
	}
	return ed25519.NewKeyFromSeed(seed), nil
}

func genKey(path string) error {
	seed := make([]byte, ed25519.SeedSize)
	if _, err := rand.Read(seed); err != nil {
		return err
	}
	if err := os.WriteFile(path, []byte(hex.EncodeToString(seed)+"\n"), 0o600); err != nil {
		return err
	}
	pub := ed25519.NewKeyFromSeed(seed).Public().(ed25519.PublicKey)
	fmt.Println(hex.EncodeToString(pub))
	return nil
}

func main() {
	listen := flag.String("listen", ":27720", "UDP address to listen on")
	keyPath := flag.String("key", "", "file with the server's Ed25519 seed (hex)")
	gen := flag.String("genkey", "", "write a new key to this file, print the public key, and exit")
	flag.Parse()
	if *gen != "" {
		if err := genKey(*gen); err != nil {
			log.Fatal(err)
		}
		return
	}
	if *keyPath == "" {
		log.Fatal("-key is required (make one with -genkey)")
	}
	key, err := loadKey(*keyPath)
	if err != nil {
		log.Fatal(err)
	}
	addr, err := net.ResolveUDPAddr("udp4", *listen)
	if err != nil {
		log.Fatal(err)
	}
	conn, err := net.ListenUDP("udp4", addr)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("pairing: listening on %s, public key %s", conn.LocalAddr(),
		hex.EncodeToString(key.Public().(ed25519.PublicKey)))

	s := NewServer(key)
	buf := make([]byte, 1500)
	lastSweep, lastLog := time.Now(), time.Now()
	for {
		// One goroutine does everything, so the queues need no locking;
		// the deadline wakes the loop for sweeping when nothing arrives.
		conn.SetReadDeadline(time.Now().Add(time.Second))
		n, from, err := conn.ReadFromUDPAddrPort(buf)
		now := time.Now()
		if err == nil {
			for _, o := range s.Handle(buf[:n], from, now) {
				conn.WriteToUDPAddrPort(o.Data, o.To)
			}
		} else if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
			log.Printf("pairing: read: %v", err)
		}
		if now.Sub(lastSweep) >= time.Second {
			s.Sweep(now)
			lastSweep = now
		}
		if now.Sub(lastLog) >= 10*time.Minute {
			log.Printf("pairing: %s", strings.TrimSpace(s.Stats()))
			lastLog = now
		}
	}
}
