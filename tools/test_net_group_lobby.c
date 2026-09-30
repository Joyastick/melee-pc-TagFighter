/* Group lobby over an in-memory lossy network: a host and up to three guests
 * end up holding one signed roster; forgeries, strangers and a full lobby do
 * nothing. Standalone:
 *   gcc -Isrc/pc -Iextern/monocypher tools/test_net_group_lobby.c src/pc/net_group.c \
 *       src/pc/net_group_lobby.c src/pc/net_identity.c extern/monocypher/monocypher.c \
 *       extern/monocypher/monocypher-ed25519.c -lbcrypt (Windows) */
#include "../src/pc/net_group_lobby.h"
#include "monocypher.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* net_identity.c wants these two from the rest of the game. */
void pc_dht_sha1(const void* data, size_t length, uint8_t out[20]) {
    uint8_t h[32];
    crypto_blake2b(h, sizeof h, data, length);
    memcpy(out, h, 20);
}
void pc_log_line(const char* fmt, ...) {
    (void)fmt;
}
bool pc_net_connect_group(intptr_t socket, int local, int machines, const char* const* ips,
    const uint16_t* ports, uint32_t seed) {
    (void)socket, (void)local, (void)machines, (void)ips, (void)ports, (void)seed;
    return true;
}

#define MAXN 5
typedef struct Node {
    GroupLobby l;
    PcNetIdentity id;
    uint32_t ip;
    uint16_t port;
    char suffix[9];
} Node;
typedef struct Packet {
    uint64_t at;
    uint32_t dst_ip, src_ip;
    uint16_t dst_port, src_port;
    int len;
    uint8_t data[512];
} Packet;

static Node s_node[MAXN];
static int s_nodes;
static Packet s_q[4096];
static int s_qn;
static uint64_t s_now;
static int s_loss_pct;
static unsigned s_rng = 12345;
static int s_sent;

static unsigned rnd(unsigned n) {
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) % n;
}

static void net_send(void* ctx, uint32_t ip, uint16_t port, const void* data, int len) {
    Node* from = ctx;
    s_sent++;
    if ((int)rnd(100) < s_loss_pct || s_qn >= 4096 || len > 512) {
        return;
    }
    Packet* p = &s_q[s_qn++];
    p->at = s_now + rnd(200);
    p->dst_ip = ip;
    p->dst_port = port;
    p->src_ip = from->ip;
    p->src_port = from->port;
    p->len = len;
    memcpy(p->data, data, (size_t)len);
}

static void suffix_of(const uint8_t key[32], char out[9]) {
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

static void make_nodes(int n) {
    memset(s_node, 0, sizeof s_node);
    s_nodes = n;
    s_qn = 0;
    s_now = 1000;
    for (int i = 0; i < n; i++) {
        char m[16];
        snprintf(m, sizeof m, "node-%d", i);
        pc_identity_derive(&s_node[i].id, m, strlen(m));
        s_node[i].ip = 0x08080800u + (uint32_t)i; /* each on its own public IP */
        s_node[i].port = (uint16_t)(7000 + i);
        suffix_of(s_node[i].id.public_key, s_node[i].suffix);
    }
}

static GroupMember self_of(const Node* n) {
    GroupMember m;
    memset(&m, 0, sizeof m);
    m.pub_ip = n->ip;
    m.pub_port = n->port;
    m.lan_ip = 0xC0A80000u | (n->ip & 255);
    m.lan_port = n->port;
    return m;
}

/* Run the network: deliver what is due, poll everyone, one ms at a time. */
static void run(uint64_t ms) {
    for (uint64_t end = s_now + ms; s_now < end; s_now++) {
        for (int i = 0; i < s_qn;) {
            if (s_q[i].at > s_now) {
                i++;
                continue;
            }
            Packet p = s_q[i];
            s_q[i] = s_q[--s_qn];
            for (int k = 0; k < s_nodes; k++) {
                if (s_node[k].ip == p.dst_ip && s_node[k].port == p.dst_port) {
                    group_lobby_receive(&s_node[k].l, p.data, p.len, p.src_ip, p.src_port, s_now);
                }
            }
        }
        for (int k = 0; k < s_nodes; k++) {
            group_lobby_poll(&s_node[k].l, s_now);
        }
    }
}

static void start_lobby(int guests) {
    make_nodes(1 + guests);
    GroupMember hs = self_of(&s_node[0]);
    group_lobby_host(&s_node[0].l, &s_node[0].id, &hs, net_send, &s_node[0], s_now, 20000);
    for (int i = 1; i <= guests; i++) {
        GroupMember gs = self_of(&s_node[i]);
        group_lobby_join(&s_node[i].l, &s_node[i].id, &gs, s_node[0].suffix, s_node[0].ip,
            s_node[0].port, net_send, &s_node[i], s_now, 20000);
    }
}

static void all_ready(int n) {
    for (int i = 0; i < n; i++) {
        assert(s_node[i].l.state == GL_READY);
    }
    int local;
    const GroupRoster* host = group_lobby_roster(&s_node[0].l, &local);
    assert(host != NULL && local == 0 && host->n == n && group_roster_check(host) == NULL);
    for (int i = 1; i < n; i++) {
        const GroupRoster* r = group_lobby_roster(&s_node[i].l, &local);
        if (r != NULL && memcmp(r, host, sizeof *r) != 0) {
            for (int k = 0; k < (int)sizeof *r; k++) {
                if (((const uint8_t*)r)[k] != ((const uint8_t*)host)[k]) {
                    printf("diff at byte %d (member %d): %02x vs %02x\n", k,
                        k / (int)sizeof(GroupMember), ((const uint8_t*)r)[k],
                        ((const uint8_t*)host)[k]);
                    break;
                }
            }
        }
        /* machine numbers are join order, so a guest's need not be its node number */
        assert(r != NULL && local >= 1 && local < n && memcmp(r, host, sizeof *r) == 0);
        assert(memcmp(r->m[local].key, s_node[i].id.public_key, 32) == 0);
        /* the host recorded the endpoint the guest was seen at */
        assert(r->m[local].pub_ip == s_node[i].ip && r->m[local].pub_port == s_node[i].port);
    }
}

int main(void) {
    /* 1. host + 3 guests over a network that loses a quarter of everything */
    s_loss_pct = 25;
    start_lobby(3);
    run(6000);
    assert(group_lobby_count(&s_node[0].l) == 4);
    assert(!group_lobby_start(&s_node[1].l, s_now)); /* only the host starts it */
    assert(group_lobby_start(&s_node[0].l, s_now));
    run(15000);
    all_ready(4);
    const GroupRoster* r = group_lobby_roster(&s_node[0].l, NULL);
    assert(r->m[0].team == 0 && r->m[1].team == 0 && r->m[2].team == 1 && r->m[3].team == 1);

    /* 2. three machines: the host may start once two are in; teams 2 v 1 */
    start_lobby(2);
    run(4000);
    assert(group_lobby_start(&s_node[0].l, s_now));
    run(10000);
    all_ready(3);
    r = group_lobby_roster(&s_node[0].l, NULL);
    assert(r->m[0].team == 0 && r->m[1].team == 0 && r->m[2].team == 1);

    /* 3. a lone host cannot start; a full lobby turns a fifth machine away */
    s_loss_pct = 0;
    start_lobby(3);
    assert(!group_lobby_start(&s_node[0].l, s_now));
    make_nodes(5);
    GroupMember hs = self_of(&s_node[0]);
    group_lobby_host(&s_node[0].l, &s_node[0].id, &hs, net_send, &s_node[0], s_now, 20000);
    for (int i = 1; i < 5; i++) {
        GroupMember gs = self_of(&s_node[i]);
        group_lobby_join(&s_node[i].l, &s_node[i].id, &gs, s_node[0].suffix, s_node[0].ip,
            s_node[0].port, net_send, &s_node[i], s_now, 3000);
    }
    run(2000);
    assert(group_lobby_count(&s_node[0].l) == 4);
    int waiting = 0, turned_away = -1; /* whoever arrived last is never answered ... */
    for (int i = 1; i < 5; i++) {
        if (s_node[i].l.state == GL_COLLECTING) {
            waiting++;
            turned_away = i;
        }
    }
    assert(waiting == 1);
    run(2000);
    assert(s_node[turned_away].l.state == GL_FAILED); /* ... and gives up */

    /* 4. a guest that dialled the wrong code is never let in, and a stranger's
     *    roster (signed by another key) is refused by a guest that is in */
    start_lobby(2);
    make_nodes(3);
    hs = self_of(&s_node[0]);
    group_lobby_host(&s_node[0].l, &s_node[0].id, &hs, net_send, &s_node[0], s_now, 20000);
    GroupMember g1 = self_of(&s_node[1]);
    group_lobby_join(&s_node[1].l, &s_node[1].id, &g1, "WRONGONE", s_node[0].ip, s_node[0].port,
        net_send, &s_node[1], s_now, 20000);
    run(3000);
    assert(group_lobby_count(&s_node[0].l) == 1 && s_node[1].l.state == GL_COLLECTING);

    start_lobby(2);
    run(2000);
    assert(s_node[1].l.state == GL_JOINED && group_lobby_count(&s_node[0].l) == 3);
    /* a roster signed by node 2 (a guest, not the host) with the right nonce */
    GroupLobby fake;
    GroupMember fs = self_of(&s_node[2]);
    group_lobby_host(&fake, &s_node[2].id, &fs, net_send, &s_node[2], s_now, 0);
    fake.lobby_nonce = s_node[1].l.lobby_nonce;
    fake.roster = s_node[0].l.roster;
    fake.roster.n = 3;
    fake.roster.m[0].team = 0;
    fake.roster.m[1].team = 0;
    fake.roster.m[2].team = 1;
    fake.state = GL_SETTLING;
    memcpy(fake.roster.m[1].key, s_node[1].id.public_key, 32);
    memcpy(fake.roster.m[2].key, s_node[2].id.public_key, 32);
    fake.acked[1] = fake.acked[2] = 0;
    fake.self = fs;
    /* deliver its ROSTER to node 1 directly */
    s_qn = 0;
    fake.send = net_send;
    fake.ctx = &s_node[2];
    fake.roster.m[1].pub_ip = s_node[1].ip;
    fake.roster.m[1].pub_port = s_node[1].port;
    fake.roster.m[1].lan_ip = 0;
    group_lobby_poll(&fake, s_now + 1);
    run(1500);
    assert(s_node[1].l.state == GL_JOINED); /* took nothing from a stranger */

    /* 5. a tampered JOIN (one bit of the key) is ignored by the host */
    start_lobby(0);
    s_node[0].l.state = GL_COLLECTING;
    GroupLobby joiner;
    GroupMember js = self_of(&s_node[0]);
    memcpy(&joiner, &s_node[0].l, sizeof joiner);
    PcNetIdentity other;
    pc_identity_derive(&other, "other", 5);
    group_lobby_join(&joiner, &other, &js, s_node[0].suffix, s_node[0].ip, s_node[0].port, net_send,
        &s_node[0], s_now, 1000);
    group_lobby_poll(&joiner, s_now);
    assert(s_qn == 1);
    Packet p = s_q[0];
    p.data[14] ^= 1; /* inside the key */
    assert(group_lobby_receive(&s_node[0].l, p.data, p.len, 1, 1, s_now));
    assert(group_lobby_count(&s_node[0].l) == 1);
    p = s_q[0]; /* the untouched one gets in */
    assert(group_lobby_receive(&s_node[0].l, p.data, p.len, 1, 1, s_now));
    assert(group_lobby_count(&s_node[0].l) == 2);

    puts("PASS: lobby forms a signed roster over 25% loss, refuses strangers, forgeries and a fifth");
    return 0;
}
