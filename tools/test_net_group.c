/* Group roster: parties into teams, ports, wire round trip, refusals.
 * Standalone (no game, no sockets): gcc -Isrc/pc tools/test_net_group.c src/pc/net_group.c */
#include "../src/pc/net_group.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int s_connect_machines, s_connect_local;
static char s_ip1[16];
bool pc_net_connect_group(intptr_t socket, int local, int machines, const char* const* ips,
    const uint16_t* ports, uint32_t seed) {
    (void)socket, (void)ports, (void)seed;
    s_connect_local = local;
    s_connect_machines = machines;
    snprintf(s_ip1, sizeof s_ip1, "%s", ips[machines > 1 ? (local == 1 ? 0 : 1) : 0]);
    return true;
}

static GroupMember member(uint8_t k, uint32_t ip) {
    GroupMember m;
    memset(&m, 0, sizeof m);
    memset(m.key, k, sizeof m.key);
    m.pub_ip = ip;
    m.pub_port = 7000 + k;
    m.lan_ip = 0xC0A80000u | k; /* 192.168.0.k */
    m.lan_port = 8000 + k;
    snprintf(m.name, sizeof m.name, "P%u", k);
    return m;
}

int main(void) {
    GroupMember a1 = member(1, 0x01010101), a2 = member(2, 0x02020202);
    GroupMember b1 = member(3, 0x03030303), b2 = member(4, 0x04040404);
    GroupRoster r;

    /* duo vs duo: team A machines 0,1 on ports 0,1; team B on 2,3 */
    const GroupMember* duo_a[] = {&a1, &a2};
    const GroupMember* duo_b[] = {&b1, &b2};
    const GroupMember* both[2];
    int sizes[2] = {2, 2};
    both[0] = (const GroupMember*)duo_a;
    /* party arrays are arrays of members, not of pointers */
    GroupMember pa[2] = {a1, a2}, pb[2] = {b1, b2}, solo[1] = {b1};
    const GroupMember* parties[2] = {pa, pb};
    (void)duo_a, (void)duo_b, (void)both;
    assert(group_from_parties(&r, parties, sizes, 2) && r.n == 4);
    for (int i = 0; i < 4; i++) {
        assert(group_port(&r, i) == i);
    }

    /* duo vs solo: the solo keeps port 2, port 3 is its CPU assist */
    const GroupMember* parties2[2] = {pa, solo};
    int sizes2[2] = {2, 1};
    assert(group_from_parties(&r, parties2, sizes2, 2) && r.n == 3);
    assert(group_port(&r, 0) == 0 && group_port(&r, 1) == 1 && group_port(&r, 2) == 2);

    /* solo vs solo is the existing two machine match: ports 0 and 2 */
    GroupMember sa[1] = {a1};
    const GroupMember* parties3[2] = {sa, solo};
    int sizes3[2] = {1, 1};
    assert(group_from_parties(&r, parties3, sizes3, 2) && r.n == 2);
    assert(group_port(&r, 0) == 0 && group_port(&r, 1) == 2);
    assert(group_port(&r, 2) == -1 && group_port(&r, -1) == -1);

    /* refusals: a third party, an empty or oversize party, the same player twice */
    int one3[3] = {1, 1, 1};
    const GroupMember* three[3] = {sa, solo, pa};
    assert(!group_from_parties(&r, three, one3, 3));
    int zero[2] = {0, 1};
    assert(!group_from_parties(&r, parties3, zero, 2));
    int big[2] = {3, 1};
    assert(!group_from_parties(&r, parties3, big, 2));
    const GroupMember* twice[2] = {sa, sa};
    assert(!group_from_parties(&r, twice, sizes3, 2));

    /* check: split party, wrong team order, no endpoint */
    assert(group_from_parties(&r, parties, sizes, 2));
    GroupRoster bad = r;
    bad.m[1].team = 1;
    bad.m[1].party = bad.m[0].party; /* party 0 now spans both teams */
    assert(group_roster_check(&bad) != NULL);
    bad = r;
    bad.m[0].team = 1;
    assert(group_roster_check(&bad) != NULL); /* B before A */
    bad = r;
    bad.m[2].pub_port = 0;
    assert(group_roster_check(&bad) != NULL);
    bad = r;
    bad.n = 5;
    assert(group_roster_check(&bad) != NULL);

    /* wire round trip, and truncation is refused */
    uint8_t wire[GROUP_WIRE_MAX];
    int n = group_roster_encode(&r, wire, sizeof wire);
    assert(n > 0 && n <= GROUP_WIRE_MAX);
    GroupRoster back;
    assert(group_roster_decode(&back, wire, n) == n && memcmp(&back, &r, sizeof r) == 0);
    assert(group_roster_decode(&back, wire, n - 1) == 0);
    assert(group_roster_encode(&r, wire, n - 1) == 0);
    wire[0] = 9;
    assert(group_roster_decode(&back, wire, n) == 0);

    /* connect: a member behind our own public IP is dialled on its LAN address */
    assert(group_from_parties(&r, parties, sizes, 2));
    r.m[1].pub_ip = r.m[0].pub_ip; /* machines 0 and 1 share a router */
    assert(group_connect(&r, 0, -1, 1) && s_connect_machines == 4 && s_connect_local == 0);
    assert(strcmp(s_ip1, "192.168.0.2") == 0);
    assert(group_connect(&r, 2, -1, 1) && strcmp(s_ip1, "1.1.1.1") == 0); /* 3 dials 0 publicly */
    assert(!group_connect(&r, 4, -1, 1));

    puts("PASS: parties make teams, ports follow teams, roster round-trips, bad rosters refused");
    return 0;
}
