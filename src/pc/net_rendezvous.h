/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_RENDEZVOUS_H
#define PC_NET_RENDEZVOUS_H
/* Client of the MeleeVS pairing server (server/pairing/). The server only
 * introduces two players who asked for the same topic; the signed
 * Hello/Offer/Ack in net_match.c still decides the match, so a server that
 * is down, slow or lying costs time and nothing else. It runs beside the DHT
 * search, never instead of it.
 *
 * Wire format (server/pairing/protocol.go must match): "MPS1", version 1,
 * a type byte, then
 *   'H' HELLO   {nonce 8}, zero-padded to 100 bytes
 *   'C' COOKIE  {nonce 8, cookie 16, observed ip 4 + port 2, sig 64}  (100)
 *   'J' JOIN    {nonce 8, cookie 16, topic 20, lan ip 4 + port 2, want 1,
 *                avoid ip 4 + port 2}, zero-padded to 110 bytes
 *   'Q' QUEUED  {nonce 8, fresh cookie 16, topic 20}                  (50)
 *   'L' LEAVE   {nonce 8, cookie 16, topic 20}                        (50)
 *   'M' MATCH   {nonce 8, topic 20, peer ip 4 + port 2,
 *                peer lan ip 4 + port 2, sig 64}                      (110)
 * IPs in network order, ports big-endian. sig is the server's Ed25519
 * signature over everything before it; nonce is ours, echoed, so a reply
 * from an earlier search is refused. */
#include "net_dht.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef bool (*PcRdvSend)(const void* data, size_t size, const struct pc_dht_endpoint* to);
/* Queue for topic, sending through send (the DHT socket, so the server sees
 * the NAT mapping the peer will reach). No-op when no server is configured:
 * MELEE_PAIRING_SERVER=host:port and MELEE_PAIRING_KEY=<64 hex digits>. */
void pc_rdv_start(const uint8_t topic[20], uint32_t lan_ip, uint16_t lan_port, PcRdvSend send);
/* Leave the queue (if in it) and go idle. */
void pc_rdv_stop(void);
void pc_rdv_poll(uint64_t now_ms);
/* True when data was a pairing-server packet (consumed, valid or not). */
bool pc_rdv_receive(const void* data, size_t size, const struct pc_dht_endpoint* from);
/* A MATCH not yet taken: the peer's public and LAN endpoints. */
bool pc_rdv_take_match(struct pc_dht_endpoint* peer, struct pc_dht_endpoint* peer_lan);
/* Our public IP as the server saw it (network order), 0 while unknown. */
uint32_t pc_rdv_public_ip(void);
/* The attempt with peer failed: queue again at once, not to be paired with
 * it straight back. */
void pc_rdv_retry_avoiding(const struct pc_dht_endpoint* peer);
#ifdef __cplusplus
}
#endif
#endif
