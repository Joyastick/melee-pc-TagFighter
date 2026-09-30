/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_GROUP_LOBBY_H
#define PC_NET_GROUP_LOBBY_H
#include "net_group.h"
#include "net_identity.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* The lobby that forms a 3-4 machine Direct match (and, later, a party): the
 * host publishes its connect code, up to three guests send JOIN to it, the
 * host settles a GroupRoster and every machine learns it, and then all of them
 * open the session (group_connect). It is a small state machine with no
 * sockets of its own: the caller hands it received datagrams and a send
 * callback, and polls it, so the same code runs over the DHT socket in the
 * game and over an in-memory network in the test.
 *
 * Everything is signed with the sender's identity key. A guest checks the host
 * against the key suffix of the code it dialled, and the host's roster and GO
 * against that key; the host trusts a JOIN for the key inside it and learns the
 * guest's public endpoint from where the JOIN came from. Roster, ACK and GO
 * carry the host's lobby nonce, so a capture from one lobby cannot drive
 * another. */

enum GroupLobbyState {
    GL_IDLE,
    GL_COLLECTING, /* host: taking JOINs; guest: JOIN sent, waiting for JOINED */
    GL_JOINED,     /* guest: in, waiting for the host to start */
    GL_SETTLING,   /* roster out, waiting for everyone's ACK / GO */
    GL_READY,      /* every machine knows the roster: open the session */
    GL_FAILED,
};

typedef void (*GroupSend)(void* ctx, uint32_t ip, uint16_t port, const void* data, int len);

typedef struct GroupLobby {
    int state;
    bool host;
    PcNetIdentity id;
    GroupSend send;
    void* ctx;
    GroupMember self; /* our own endpoints as we know them */
    /* host */
    GroupRoster roster;       /* member i = machine i once settled */
    uint8_t acked[GROUP_MAX]; /* ROSTER ack'd */
    uint8_t goacked[GROUP_MAX];
    uint64_t lobby_nonce;
    /* guest */
    uint32_t host_ip;
    uint16_t host_port;
    char host_suffix[9];
    uint8_t host_key[32];
    bool host_key_known;
    uint64_t join_nonce;
    int local; /* our machine number once the roster is known */
    /* both */
    uint64_t next_send_ms, started_ms, timeout_ms;
    const char* why; /* GL_FAILED: what went wrong */
} GroupLobby;

/* Host a lobby. `self` is our own key-less endpoint (public and LAN). */
void group_lobby_host(GroupLobby* l, const PcNetIdentity* id, const GroupMember* self,
    GroupSend send, void* ctx, uint64_t now_ms, uint64_t timeout_ms);
/* Join the lobby of the host whose connect code ends in `host_suffix`, reachable at ip:port. */
void group_lobby_join(GroupLobby* l, const PcNetIdentity* id, const GroupMember* self,
    const char* host_suffix, uint32_t host_ip, uint16_t host_port, GroupSend send, void* ctx,
    uint64_t now_ms, uint64_t timeout_ms);
/* A datagram from src; true when it was a lobby message (whatever came of it). */
bool group_lobby_receive(GroupLobby* l, const void* data, int len, uint32_t src_ip,
    uint16_t src_port, uint64_t now_ms);
void group_lobby_poll(GroupLobby* l, uint64_t now_ms);
/* Host: how many machines are in, ourselves included. */
int group_lobby_count(const GroupLobby* l);
/* Host: close the lobby with the machines that are in and send the roster.
 * False with fewer than two, or when not the host / not collecting. */
bool group_lobby_start(GroupLobby* l, uint64_t now_ms);
/* Valid at GL_READY: the agreed roster and our machine number in it. */
const GroupRoster* group_lobby_roster(const GroupLobby* l, int* local);

#ifdef __cplusplus
}
#endif
#endif
