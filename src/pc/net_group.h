/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_GROUP_H
#define PC_NET_GROUP_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* A 2 to 4 machine match as the lobby agrees it, before any session exists.
 * The host (machine 0) collects members, settles this roster and sends it to
 * each of them; every machine then opens a session on the same socket to all
 * the others (pc_net_connect_group). The roster is also what a party brings
 * into Matchmaking: a party is 1 or 2 machines that share a team and are
 * never split or merged, and a match is two teams. */
#define GROUP_MAX 4

typedef struct GroupMember {
    uint8_t key[32];    /* identity key: who this is */
    uint32_t pub_ip;    /* NAT-observed address as a.b.c.d = a<<24 | b<<16 | c<<8 | d */
    uint16_t pub_port;  /* host order */
    uint32_t lan_ip;    /* 0 if not known */
    uint16_t lan_port;
    uint8_t team;       /* 0 = A, 1 = B */
    uint8_t party;      /* members that queued together share it */
} GroupMember;

/* Machine number = index; machine 0 hosts. Machines are ordered team A first,
 * then team B, so a team's machines are contiguous. */
typedef struct GroupRoster {
    uint8_t n;
    GroupMember m[GROUP_MAX];
} GroupRoster;

/* NULL when the roster can start a match, else why not: 2 to 4 machines,
 * distinct keys, usable endpoints, two teams of at most two machines with A
 * before B, and no party split across teams. */
const char* group_roster_check(const GroupRoster* r);

/* Build a roster from parties: party[i] holds `size[i]` members (1 or 2), and
 * exactly two parties make a match. Party 0 becomes team A and hosts. False
 * (roster untouched) unless that holds. */
bool group_from_parties(GroupRoster* out, const GroupMember* const* party, const int* size,
    int parties);

/* The game port machine `machine` plays on: team t owns ports 2t and 2t+1,
 * its machines take them in machine order, and a team of one leaves its
 * second port to a CPU assist. -1 for a bad machine. */
int group_port(const GroupRoster* r, int machine);

/* Wire image: n, then per member key/endpoints/team/party, big-endian; the
 * caller signs the bytes. decode validates lengths but not the roster (run
 * group_roster_check). Returns bytes used, 0 on failure. */
#define GROUP_WIRE_MAX (1 + GROUP_MAX * (32 + 4 + 2 + 4 + 2 + 1 + 1))
int group_roster_encode(const GroupRoster* r, uint8_t* out, int cap);
int group_roster_decode(GroupRoster* r, const uint8_t* in, int len);

/* Open the session for machine `local` of an agreed roster on `socket` (-1
 * binds a new one): each other machine is dialled on its LAN address when it
 * shares our public IP, else on its public one. */
bool group_connect(const GroupRoster* r, int local, intptr_t socket, uint32_t seed);

#ifdef __cplusplus
}
#endif
#endif
