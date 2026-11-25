/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_group_lobby.h"

#include <stdio.h>
#include <string.h>

extern void pc_dht_sha1(const void* data, size_t length, uint8_t out[20]);

#define GL_MAGIC 0x4d504731u /* MPG1 */
#define GL_VERSION 1
#define JOIN_RETRY_MS 500
#define SETTLE_RETRY_MS 300

enum { T_JOIN = 1, T_JOINED, T_ROSTER, T_ACK, T_GO, T_GOACK };

#pragma pack(push, 1)
typedef struct Join {
    uint32_t magic;
    uint8_t ver, type;
    uint64_t nonce; /* the guest's, echoed in JOINED */
    uint8_t key[32];
    char host_suffix[8];
    uint32_t lan_ip;
    uint16_t lan_port;
    uint32_t pub_ip; /* what the sender believes its public endpoint is (0 = unknown) */
    uint16_t pub_port;
    char name[8]; /* the label of the sender's connect code */
    uint8_t sig[64];
} Join;
typedef struct Joined {
    uint32_t magic;
    uint8_t ver, type, index, count;
    uint64_t join_nonce, lobby_nonce;
    uint8_t host_key[32];
    uint8_t sig[64];
} Joined;
typedef struct RosterMsg {
    uint32_t magic;
    uint8_t ver, type;
    uint64_t lobby_nonce;
    uint8_t len;
    uint8_t wire[GROUP_WIRE_MAX];
    uint8_t sig[64];
} RosterMsg;
typedef struct AckMsg { /* T_ACK (roster taken) and T_GOACK (go taken) */
    uint32_t magic;
    uint8_t ver, type;
    uint64_t lobby_nonce;
    uint8_t key[32];
    uint8_t sig[64];
} AckMsg;
typedef struct GoMsg {
    uint32_t magic;
    uint8_t ver, type;
    uint64_t lobby_nonce;
    uint8_t sig[64];
} GoMsg;
#pragma pack(pop)

static void key_suffix(const uint8_t key[32], char out[9]) {
    uint8_t h[20];
    pc_dht_sha1(key, 32, h);
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint64_t bits = (uint64_t)h[0] << 32 | (uint64_t)h[1] << 24 | (uint64_t)h[2] << 16 |
                    (uint64_t)h[3] << 8 | h[4];
    for (int i = 0; i < 8; i++) {
        out[i] = a[(bits >> (35 - 5 * i)) & 31];
    }
    out[8] = 0;
}

/* Every message ends in a 64-byte signature over the bytes before it. */
#define SIGN(l, m) pc_identity_sign(&(l)->id, (m)->sig, (m), sizeof *(m) - sizeof(m)->sig)
#define VERIFY(key, m) pc_identity_verify((key), (m)->sig, (m), sizeof *(m) - sizeof(m)->sig)

static void fail(GroupLobby* l, const char* why) {
    l->state = GL_FAILED;
    l->why = why;
}

/* A member's address as we should dial it: its LAN one when it shares our
 * public IP (no hairpin NAT), else its public one. */
static void dial(const GroupLobby* l, const GroupMember* m, uint32_t* ip, uint16_t* port) {
    bool lan = m->lan_ip != 0 && m->pub_ip == l->self.pub_ip;
    *ip = lan ? m->lan_ip : m->pub_ip;
    *port = lan ? m->lan_port : m->pub_port;
}

static void base(GroupLobby* l, const PcNetIdentity* id, const GroupMember* self, GroupSend send,
    void* ctx, uint64_t now, uint64_t timeout) {
    memset(l, 0, sizeof *l);
    l->id = *id;
    l->self = *self;
    memcpy(l->self.key, id->public_key, 32);
    group_name_clean(l->self.name, self->name);
    l->send = send;
    l->ctx = ctx;
    l->started_ms = now;
    l->timeout_ms = timeout;
    l->state = GL_COLLECTING;
    l->local = -1;
}

void group_lobby_host(GroupLobby* l, const PcNetIdentity* id, const GroupMember* self,
    GroupSend send, void* ctx, uint64_t now_ms, uint64_t timeout_ms) {
    base(l, id, self, send, ctx, now_ms, timeout_ms);
    l->host = true;
    l->local = 0;
    l->roster.n = 1;
    l->roster.m[0] = l->self;
    if (!pc_identity_random(&l->lobby_nonce, sizeof l->lobby_nonce)) {
        fail(l, "no random source");
    }
}

void group_lobby_join(GroupLobby* l, const PcNetIdentity* id, const GroupMember* self,
    const char* host_suffix, uint32_t host_ip, uint16_t host_port, GroupSend send, void* ctx,
    uint64_t now_ms, uint64_t timeout_ms) {
    base(l, id, self, send, ctx, now_ms, timeout_ms);
    l->host_ip = host_ip;
    l->host_port = host_port;
    snprintf(l->host_suffix, sizeof l->host_suffix, "%s", host_suffix);
    if (!pc_identity_random(&l->join_nonce, sizeof l->join_nonce)) {
        fail(l, "no random source");
    }
}

int group_lobby_count(const GroupLobby* l) {
    return l->host ? l->roster.n : 0;
}

/* A source address that only exists behind a router (host order). */
static bool private_ip(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 ||
           (ip >> 16) == 0xA9FE || (ip >> 22) == (0x64400000u >> 22);
}

static int member_of(const GroupLobby* l, const uint8_t key[32]) {
    for (int i = 0; i < l->roster.n; i++) {
        if (memcmp(l->roster.m[i].key, key, 32) == 0) {
            return i;
        }
    }
    return -1;
}

static void send_join(GroupLobby* l) {
    Join j;
    memset(&j, 0, sizeof j);
    j.magic = GL_MAGIC;
    j.ver = GL_VERSION;
    j.type = T_JOIN;
    j.nonce = l->join_nonce;
    memcpy(j.key, l->id.public_key, 32);
    memcpy(j.host_suffix, l->host_suffix, 8);
    j.lan_ip = l->self.lan_ip;
    j.lan_port = l->self.lan_port;
    j.pub_ip = l->self.pub_ip;
    j.pub_port = l->self.pub_port;
    memcpy(j.name, l->self.name, 8);
    SIGN(l, &j);
    l->send(l->ctx, l->host_ip, l->host_port, &j, (int)sizeof j);
}

static void send_roster(GroupLobby* l, int to) {
    RosterMsg r;
    memset(&r, 0, sizeof r);
    r.magic = GL_MAGIC;
    r.ver = GL_VERSION;
    r.type = T_ROSTER;
    r.lobby_nonce = l->lobby_nonce;
    r.len = (uint8_t)group_roster_encode(&l->roster, r.wire, sizeof r.wire);
    SIGN(l, &r);
    uint32_t ip;
    uint16_t port;
    dial(l, &l->roster.m[to], &ip, &port);
    l->send(l->ctx, ip, port, &r, (int)sizeof r);
}

static void send_go(GroupLobby* l, int to) {
    GoMsg g;
    memset(&g, 0, sizeof g);
    g.magic = GL_MAGIC;
    g.ver = GL_VERSION;
    g.type = T_GO;
    g.lobby_nonce = l->lobby_nonce;
    SIGN(l, &g);
    uint32_t ip;
    uint16_t port;
    dial(l, &l->roster.m[to], &ip, &port);
    l->send(l->ctx, ip, port, &g, (int)sizeof g);
}

static void send_ack(GroupLobby* l, int type) {
    AckMsg a;
    memset(&a, 0, sizeof a);
    a.magic = GL_MAGIC;
    a.ver = GL_VERSION;
    a.type = (uint8_t)type;
    a.lobby_nonce = l->lobby_nonce;
    memcpy(a.key, l->id.public_key, 32);
    SIGN(l, &a);
    l->send(l->ctx, l->host_ip, l->host_port, &a, (int)sizeof a);
}

bool group_lobby_start(GroupLobby* l, uint64_t now_ms) {
    if (!l->host || l->state != GL_COLLECTING || l->roster.n < 2) {
        return false;
    }
    /* Teams by join order: the first half is A. A plain Direct match takes its
     * teams from the CSS colours later; this is the roster's own tidy split. */
    int n = l->roster.n, a = (n + 1) / 2;
    for (int i = 0; i < n; i++) {
        l->roster.m[i].team = (uint8_t)(i < a ? 0 : 1);
        l->roster.m[i].party = (uint8_t)i;
    }
    const char* bad = group_roster_check(&l->roster);
    if (bad != NULL) {
        fail(l, bad);
        return false;
    }
    l->state = GL_SETTLING;
    l->started_ms = now_ms; /* the settle timeout runs from here */
    l->next_send_ms = now_ms;
    return true;
}

const GroupMember* group_lobby_member(const GroupLobby* l, int i) {
    bool known = l->host || l->state == GL_SETTLING || l->state == GL_READY;
    return known && i >= 0 && i < l->roster.n ? &l->roster.m[i] : NULL;
}

const GroupRoster* group_lobby_roster(const GroupLobby* l, int* local) {
    if (l->state != GL_READY) {
        return NULL;
    }
    if (local != NULL) {
        *local = l->local;
    }
    return &l->roster;
}

bool group_lobby_receive(
    GroupLobby* l, const void* data, int len, uint32_t src_ip, uint16_t src_port, uint64_t now_ms) {
    (void)now_ms;
    if (len < 6 || l->state == GL_IDLE || l->state == GL_FAILED) {
        return false;
    }
    uint32_t magic;
    memcpy(&magic, data, 4);
    const uint8_t* b = data;
    if (magic != GL_MAGIC || b[4] != GL_VERSION) {
        return false;
    }
    switch (b[5]) {
    case T_JOIN: {
        Join j;
        if (!l->host || len != (int)sizeof j) {
            return l->host;
        }
        memcpy(&j, data, sizeof j);
        char own[9];
        key_suffix(l->id.public_key, own);
        if (memcmp(j.host_suffix, own, 8) != 0 || !VERIFY(j.key, &j) ||
            memcmp(j.key, l->id.public_key, 32) == 0)
        {
            return true;
        }
        int i = member_of(l, j.key);
        if (i < 0) {
            if (l->state != GL_COLLECTING || l->roster.n >= GROUP_MAX) {
                return true; /* closed or full: the guest times out */
            }
            i = l->roster.n++;
            memset(&l->roster.m[i], 0, sizeof l->roster.m[i]);
            memcpy(l->roster.m[i].key, j.key, 32);
        }
        if (l->state == GL_COLLECTING) { /* endpoints only move before the roster is out */
            /* Seen from outside, the source is its public endpoint. A guest on
             * our own network is seen at its LAN address, which is no use to
             * anyone else, so take the public one it reports (it can only
             * misdirect itself). */
            bool lan_src = private_ip(src_ip) && j.pub_ip != 0;
            l->roster.m[i].pub_ip = lan_src ? j.pub_ip : src_ip;
            l->roster.m[i].pub_port = lan_src ? j.pub_port : src_port;
            l->roster.m[i].lan_ip = j.lan_ip;
            l->roster.m[i].lan_port = j.lan_port;
            group_name_clean(l->roster.m[i].name, j.name);
        }
        Joined r;
        memset(&r, 0, sizeof r);
        r.magic = GL_MAGIC;
        r.ver = GL_VERSION;
        r.type = T_JOINED;
        r.index = (uint8_t)i;
        r.count = l->roster.n;
        r.join_nonce = j.nonce;
        r.lobby_nonce = l->lobby_nonce;
        memcpy(r.host_key, l->id.public_key, 32);
        SIGN(l, &r);
        l->send(l->ctx, src_ip, src_port, &r, (int)sizeof r);
        return true;
    }
    case T_JOINED: {
        Joined r;
        if (l->host || len != (int)sizeof r) {
            return !l->host;
        }
        memcpy(&r, data, sizeof r);
        char own[9];
        key_suffix(r.host_key, own);
        if (r.join_nonce != l->join_nonce || strcmp(own, l->host_suffix) != 0 ||
            !VERIFY(r.host_key, &r))
        {
            return true; /* not from the host we dialled */
        }
        if (l->state == GL_COLLECTING) {
            memcpy(l->host_key, r.host_key, 32);
            l->host_key_known = true;
            l->lobby_nonce = r.lobby_nonce;
            l->state = GL_JOINED;
        }
        return true;
    }
    case T_ROSTER: {
        RosterMsg r;
        if (l->host || !l->host_key_known || len != (int)sizeof r) {
            return !l->host;
        }
        memcpy(&r, data, sizeof r);
        if (r.lobby_nonce != l->lobby_nonce || !VERIFY(l->host_key, &r)) {
            return true;
        }
        if (l->state == GL_JOINED) {
            GroupRoster ro;
            if (group_roster_decode(&ro, r.wire, r.len) != r.len ||
                group_roster_check(&ro) != NULL || memcmp(ro.m[0].key, l->host_key, 32) != 0)
            {
                return true;
            }
            int me = -1;
            for (int i = 1; i < ro.n; i++) {
                if (memcmp(ro.m[i].key, l->id.public_key, 32) == 0) {
                    me = i;
                }
            }
            if (me < 0) {
                fail(l, "the roster does not have us in it");
                return true;
            }
            l->roster = ro;
            l->local = me;
            l->self = ro.m[me]; /* our endpoints as the host saw them */
            l->state = GL_SETTLING;
        }
        if (l->state == GL_SETTLING || l->state == GL_READY) {
            send_ack(l, T_ACK); /* every copy is answered: the host may have missed the last */
        }
        return true;
    }
    case T_GO: {
        GoMsg g;
        if (l->host || len != (int)sizeof g) {
            return !l->host;
        }
        memcpy(&g, data, sizeof g);
        if (!l->host_key_known || g.lobby_nonce != l->lobby_nonce || !VERIFY(l->host_key, &g)) {
            return true;
        }
        if (l->state == GL_SETTLING) {
            l->state = GL_READY;
        }
        if (l->state == GL_READY) {
            send_ack(l, T_GOACK);
        }
        return true;
    }
    case T_ACK:
    case T_GOACK: {
        AckMsg a;
        if (!l->host || len != (int)sizeof a) {
            return l->host;
        }
        memcpy(&a, data, sizeof a);
        int i = member_of(l, a.key);
        if (i <= 0 || l->state != GL_SETTLING || a.lobby_nonce != l->lobby_nonce ||
            !VERIFY(a.key, &a))
        {
            return true;
        }
        if (b[5] == T_ACK) {
            l->acked[i] = 1;
        } else if (l->acked[i]) {
            l->goacked[i] = 1;
        }
        return true;
    }
    default:
        return false;
    }
}

void group_lobby_poll(GroupLobby* l, uint64_t now) {
    if (l->state == GL_IDLE || l->state == GL_FAILED || l->state == GL_READY) {
        return;
    }
    bool waiting =
        !(l->host && l->state == GL_COLLECTING); /* a host may wait for guests for ever */
    if (waiting && l->timeout_ms != 0 && now - l->started_ms > l->timeout_ms) {
        fail(l, "the lobby timed out");
        return;
    }
    if (now < l->next_send_ms) {
        return;
    }
    if (l->host) {
        if (l->state != GL_SETTLING) {
            return;
        }
        bool all_acked = true, all_go = true;
        for (int i = 1; i < l->roster.n; i++) {
            all_acked = all_acked && l->acked[i];
            all_go = all_go && l->goacked[i];
        }
        if (all_acked && all_go) {
            l->state = GL_READY;
            return;
        }
        for (int i = 1; i < l->roster.n; i++) {
            if (!l->acked[i]) {
                send_roster(l, i);
            } else if (!l->goacked[i]) {
                send_go(l, i);
            }
        }
        l->next_send_ms = now + SETTLE_RETRY_MS;
    } else if (l->state == GL_COLLECTING) {
        send_join(l);
        l->next_send_ms = now + JOIN_RETRY_MS;
    }
}
