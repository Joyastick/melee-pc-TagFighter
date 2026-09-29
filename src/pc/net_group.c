/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_group.h"
#include "net.h"

#include <stdio.h>
#include <string.h>

const char* group_roster_check(const GroupRoster* r) {
    if (r->n < 2 || r->n > GROUP_MAX) {
        return "a match is 2 to 4 machines";
    }
    int teams[2] = {0, 0};
    for (int i = 0; i < r->n; i++) {
        const GroupMember* a = &r->m[i];
        if (a->team > 1) {
            return "bad team";
        }
        if (a->pub_ip == 0 || a->pub_port == 0) {
            return "member with no public endpoint";
        }
        if (i > 0 && a->team < r->m[i - 1].team) {
            return "machines must be ordered team A first";
        }
        teams[a->team]++;
        for (int j = 0; j < i; j++) {
            const GroupMember* b = &r->m[j];
            if (memcmp(a->key, b->key, sizeof a->key) == 0) {
                return "the same player twice";
            }
            if (a->party == b->party && a->team != b->team) {
                return "a party is split across teams";
            }
        }
    }
    if (teams[0] < 1 || teams[1] < 1 || teams[0] > 2 || teams[1] > 2) {
        return "each team is 1 or 2 machines";
    }
    return NULL;
}

bool group_from_parties(
    GroupRoster* out, const GroupMember* const* party, const int* size, int parties) {
    if (parties != 2) {
        return false;
    }
    GroupRoster r;
    memset(&r, 0, sizeof r);
    for (int p = 0; p < 2; p++) {
        if (size[p] < 1 || size[p] > 2) {
            return false;
        }
        for (int i = 0; i < size[p]; i++) {
            GroupMember* m = &r.m[r.n++];
            *m = party[p][i];
            m->team = (uint8_t)p;
            m->party = (uint8_t)p;
        }
    }
    if (group_roster_check(&r) != NULL) {
        return false;
    }
    *out = r;
    return true;
}

int group_port(const GroupRoster* r, int machine) {
    if (machine < 0 || machine >= r->n) {
        return -1;
    }
    int slot = 0;
    for (int i = 0; i < machine; i++) {
        slot += r->m[i].team == r->m[machine].team;
    }
    return 2 * r->m[machine].team + slot;
}

static void put32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int group_roster_encode(const GroupRoster* r, uint8_t* out, int cap) {
    int need = 1 + r->n * (32 + 4 + 2 + 4 + 2 + 1 + 1);
    if (r->n > GROUP_MAX || cap < need) {
        return 0;
    }
    uint8_t* p = out;
    *p++ = r->n;
    for (int i = 0; i < r->n; i++) {
        const GroupMember* m = &r->m[i];
        memcpy(p, m->key, 32);
        p += 32;
        put32(p, m->pub_ip);
        p += 4;
        *p++ = (uint8_t)(m->pub_port >> 8);
        *p++ = (uint8_t)m->pub_port;
        put32(p, m->lan_ip);
        p += 4;
        *p++ = (uint8_t)(m->lan_port >> 8);
        *p++ = (uint8_t)m->lan_port;
        *p++ = m->team;
        *p++ = m->party;
    }
    return (int)(p - out);
}

int group_roster_decode(GroupRoster* r, const uint8_t* in, int len) {
    if (len < 1 || in[0] > GROUP_MAX) {
        return 0;
    }
    int n = in[0];
    int need = 1 + n * (32 + 4 + 2 + 4 + 2 + 1 + 1);
    if (len < need) {
        return 0;
    }
    memset(r, 0, sizeof *r);
    r->n = (uint8_t)n;
    const uint8_t* p = in + 1;
    for (int i = 0; i < n; i++) {
        GroupMember* m = &r->m[i];
        memcpy(m->key, p, 32);
        p += 32;
        m->pub_ip = get32(p);
        p += 4;
        m->pub_port = (uint16_t)(p[0] << 8 | p[1]);
        p += 2;
        m->lan_ip = get32(p);
        p += 4;
        m->lan_port = (uint16_t)(p[0] << 8 | p[1]);
        p += 2;
        m->team = *p++;
        m->party = *p++;
    }
    return need;
}

bool group_connect(const GroupRoster* r, int local, intptr_t socket, uint32_t seed) {
    if (group_roster_check(r) != NULL || local < 0 || local >= r->n) {
        return false;
    }
    char text[GROUP_MAX][16];
    const char* ips[GROUP_MAX];
    uint16_t ports[GROUP_MAX];
    for (int i = 0; i < r->n; i++) {
        const GroupMember* m = &r->m[i];
        bool lan = i != local && m->lan_ip != 0 && m->pub_ip == r->m[local].pub_ip;
        uint32_t ip = lan ? m->lan_ip : m->pub_ip;
        ports[i] = lan ? m->lan_port : m->pub_port;
        snprintf(text[i], sizeof text[i], "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255,
            (ip >> 8) & 255, ip & 255);
        ips[i] = text[i];
    }
    return pc_net_connect_group(socket, local, r->n, ips, ports, seed);
}
