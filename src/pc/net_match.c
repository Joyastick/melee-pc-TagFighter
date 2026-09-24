/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_match.h"
#include "net_dht.h"
#include "net_dht_item.h"
#include "net.h"
#include "net_lan.h"
#include "net_rank_session.h"
#include "pc.h"
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET MatchSocket;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int MatchSocket;
#endif

extern const char* pc_get_net_name(void);
/* From melee/mod/tag_assist.h. Declared rather than included: that header
 * pulls in the decomp headers (DISC_STRUCT, debug.h's __assert), which the
 * standalone netplay tests cannot compile against. */
bool TagAssist_IsTagBattleOn(void);
extern int pc_get_net_port(void);
extern const char* pc_lan_disc_id(void);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak)) void pc_log_line(const char* fmt, ...) {
    (void)fmt;
}
#endif

#define MATCH_MAGIC 0x4d504d31u /* MPM1 */
#define MATCH_VERSION 4         /* v4: hello signs its sender's own public and LAN address */
#define RETRY_MS 250
/* How long a peer that answered our Hello gets to finish Offer/Ack before we
 * drop it and keep searching. The exchange is one round trip retried every
 * RETRY_MS, so 3s is a dozen resends; 8s only delayed the next candidate. */
#define TIMEOUT_MS 3000
/* Direct connect rendezvous record (BEP44 mutable item). The slot key is
 * derived from the host's connect code, so a dialer can address it knowing
 * only the code; the value inside is signed by the host's real identity key,
 * which the dialer checks against the code the same way a Hello is. */
#define DIRECT_SALT "meleepc/direct/v1"
/* magic 4, key 32, public ip 4 + port 2, time 8, LAN ip 4 + port 2, sig 64 */
#define DIRECT_RECORD_BYTES 120
#define DIRECT_SIGNED_BYTES 56
#define DIRECT_MAX_AGE 300        /* seconds a published endpoint stays usable */
#define DIRECT_REPUBLISH_MS 90000 /* refresh well inside DIRECT_MAX_AGE */
#define DIRECT_REPOLL_MS 3000     /* dialer re-fetch: pick up a fresher record fast */
/* Extra Hello destinations beyond DHT candidates (direct record addresses,
 * same-network LAN routes), resent every HELLO_TARGET_MS until paired. */
#define HELLO_TARGETS 4
#define HELLO_TARGET_MS 1000

#pragma pack(push, 1)
typedef struct MatchHello {
    uint32_t magic;
    uint8_t version, type, mode, reserved;
    uint64_t nonce;
    uint8_t public_key[32], compatibility[20], topic[20];
    char code[18];
    /* Where this Hello may come from (network order, 0 = not known yet):
     * the sender's NAT-observed public IP and its LAN IP. Signed, so a
     * relay that forwards the Hello cannot swap in its own address. */
    uint32_t from_public, from_lan;
    uint8_t signature[64];
} MatchHello;
typedef struct MatchOffer {
    uint32_t magic;
    uint8_t version, type, mode, reserved;
    uint64_t host_nonce, guest_nonce;
    uint8_t host_key[32], guest_key[32], compatibility[20], topic[20];
    uint32_t seed;
    uint8_t signature[64];
} MatchOffer;
typedef struct MatchAck {
    uint32_t magic;
    uint8_t version, type, mode, reserved;
    uint64_t host_nonce, guest_nonce;
    uint8_t host_key[32], guest_key[32], offer_hash[20];
    uint8_t signature[64];
} MatchAck;
#pragma pack(pop)

static PcNetIdentity identity;
static enum PcNetMatchMode mode;
static int state = PC_MATCH_FAIL;
static const char* failure = "not started";
static char target[18], opponent[18];
static uint8_t peer_key[32], compatibility[20], topic[20], offer_hash[20];
static uint64_t local_nonce, peer_nonce, deadline, next_send;
static struct pc_dht_endpoint peer;
static bool have_peer, host;
static uint32_t seed;
static int32_t start_frame = -1;
static MatchHello hello;
static MatchOffer offer;
static MatchAck ack;
static bool identity_loaded;
static const char* profile_error;
static char profile_directory[4096];
static bool handshake_done, barrier_sent, barrier_received;
static bool rank_begun, proofs_ready, ack_received, offer_received;
static int proof_step; /* local GET, local PUT, peer GET, ancestry GET, complete */
static bool proof_pending;
static bool recovery_immutable;
static uint8_t recovery_wire[PC_RANK_RECORD_BYTES];
static uint8_t local_public[PC_RANK_PUBLIC_BYTES];
static int publication; /* 0 idle, 1 waiting, 2 acknowledged, -1 failed */
static uint64_t publication_deadline;
static const char* publication_reason;
static bool publication_record;
static uint8_t publication_wire[PC_RANK_RECORD_BYTES];
static PcNetIdentity direct_slot;
static bool direct_pending;
static bool direct_put_inflight; /* direct_pending is a publish, not a lookup */
static bool direct_get_logged;
static uint64_t direct_fresh_until; /* our record stays valid until then ... */
static uint16_t direct_fresh_port;  /* ... as long as the node keeps this port */
static uint64_t direct_next;
static struct pc_dht_endpoint direct_endpoint;
static struct pc_dht_endpoint hello_targets[HELLO_TARGETS];
static unsigned hello_target_count, hello_target_cursor;
static uint64_t next_target_hello;
static int64_t public_sequence(const uint8_t* p) {
    return (int64_t)((uint32_t)p[16] << 24 | (uint32_t)p[17] << 16 | (uint32_t)p[18] << 8 | p[19]);
}
static void proof_result(const PcDhtItemResult* r, void* context) {
    (void)context;
    proof_pending = false;
    uint8_t genesis[PC_RANK_PUBLIC_BYTES];
    pc_rank_session_initial_public(genesis);
    unsigned player = host ? 0 : 1;
    if (proof_step == 3) {
        if (r->status != PC_DHT_ITEM_OK || !pc_rank_session_chain_record(r->value, r->value_length))
        {
            proof_step = -1;
            return;
        }
        if (!pc_rank_session_chain_target(NULL)) {
            proof_step = 4;
            proofs_ready = true;
        }
        return;
    }
    if (proof_step == 1) {
        if (r->status != PC_DHT_ITEM_OK || !r->acknowledgements) {
            proof_step = -1;
            return;
        }
        if (recovery_immutable) {
            recovery_immutable = false;
            return;
        }
        if (!pc_rank_session_proof(
                player, local_public, sizeof local_public, public_sequence(local_public)))
        {
            proof_step = -1;
            return;
        }
        proof_step = 2;
        return;
    }
    if (r->status != PC_DHT_ITEM_OK && r->status != PC_DHT_ITEM_NOT_FOUND) {
        proof_step = -1;
        return;
    }
    const void* value = r->status == PC_DHT_ITEM_NOT_FOUND ? genesis : r->value;
    size_t length = r->status == PC_DHT_ITEM_NOT_FOUND ? sizeof genesis : r->value_length;
    int64_t sequence = r->status == PC_DHT_ITEM_NOT_FOUND ? 0 : r->sequence;
    /* A failed prior publication may leave an older verified head in DHT.
     * Durable local history is authoritative for our own strictly newer
     * sequence; never overwrite an equal-sequence conflict or newer head. */
    if (proof_step == 0 &&
        ((r->status == PC_DHT_ITEM_NOT_FOUND && memcmp(local_public, genesis, sizeof genesis)) ||
            (r->status == PC_DHT_ITEM_OK && sequence >= 0 &&
                sequence < public_sequence(local_public))))
    {
        if (!pc_rank_session_latest_record(recovery_wire)) {
            proof_step = -1;
            return;
        }
        recovery_immutable = true;
        proof_step = 1;
        return;
    }
    if (proof_step == 0 &&
        (length != sizeof local_public || memcmp(value, local_public, sizeof local_public)))
    {
        proof_step = -1;
        return;
    }
    if (proof_step == 0 && public_sequence(local_public) > 0) {
        if (!pc_rank_session_latest_record(recovery_wire)) {
            proof_step = -1;
            return;
        }
        recovery_immutable = true;
        proof_step = 1;
        return;
    }
    if (!pc_rank_session_proof(proof_step == 0 ? player : 1 - player, value, length, sequence)) {
        proof_step = -1;
        return;
    }
    proof_step = proof_step == 0 ? 2 : pc_rank_session_chain_target(NULL) ? 3 : 4;
    proofs_ready = proof_step == 4;
}
static bool begin_rank(void) {
    if (mode != PC_MATCH_RANKED || rank_begun)
        return true;
    rank_begun = true;
    return pc_rank_session_begin(profile_directory, &identity, peer_key, host ? 0 : 1, seed) &&
           pc_rank_session_public_state(local_public);
}
static void poll_proofs(void) {
    if (!rank_begun || proofs_ready || proof_pending || proof_step < 0 || !pc_dht_ready())
        return;
    const char* salt = PC_RANK_BEP44_SALT;
    if (proof_step == 3) {
        uint8_t locator[20];
        if (!pc_rank_session_chain_target(locator)) {
            proof_step = -1;
            return;
        }
        proof_pending = pc_dht_item_get_immutable(locator, proof_result, NULL);
    } else if (proof_step == 1 && recovery_immutable)
        proof_pending =
            pc_dht_item_put_immutable(recovery_wire, sizeof recovery_wire, proof_result, NULL);
    else if (proof_step == 1)
        proof_pending = pc_dht_item_put(&identity, salt, strlen(salt),
            public_sequence(local_public), local_public, sizeof local_public, proof_result, NULL);
    else
        proof_pending = pc_dht_item_get(proof_step == 0 ? identity.public_key : peer_key, salt,
            strlen(salt), 0, proof_result, NULL);
}
static void publication_result(const PcDhtItemResult* r, void* context) {
    (void)context;
    if (publication_record && r->status == PC_DHT_ITEM_OK && r->acknowledgements) {
        publication_record = false;
        proof_pending = false;
        return;
    }
    publication = r->status == PC_DHT_ITEM_OK && r->acknowledgements ? 2 : -1;
    publication_reason = publication == 2 ?
                             "rank saved and published" :
                             "rank saved locally; publication failed, retry available";
}

static bool load_identity(void) {
    if (identity_loaded)
        return true;
    char* dir = SDL_GetPrefPath(NULL, "melee-pc_TagFighter");
    const char* name = pc_get_net_name();
    bool ok = dir && pc_identity_load(&identity, dir, name && *name ? name : "PLAYER");
    if (dir && ok)
        snprintf(profile_directory, sizeof profile_directory, "%s", dir);
    SDL_free(dir);
    identity_loaded = ok;
    profile_error = ok ? NULL : "identity unavailable";
    return ok;
}

static void digest(void) {
    char text[256];
    /* Folding the Tag Battle flag in here means a Tag Battle peer and a
     * plain-VS peer hash differently and never complete a handshake, same
     * as a build mismatch - see receive()'s compatibility check below. */
    int n = snprintf(text, sizeof text, "%s\n%s\n%u", pc_app_rev(), pc_lan_disc_id(),
        TagAssist_IsTagBattleOn() ? 1u : 0u);
    /* snprintf returns the length it WOULD have written, so an over-long
     * MELEE_APP_REV (it is returned verbatim) would make the hash read past
     * this stack buffer. Hash what is actually in it. */
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n > sizeof text) {
        n = (int)sizeof text;
    }
    pc_dht_sha1(text, (size_t)n, compatibility);
}
static bool send_packet(const void* p, size_t n, const struct pc_dht_endpoint* ep) {
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = ep->address;
    to.sin_port = htons(ep->port);
    return sendto((MatchSocket)pc_dht_socket(), p, (int)n, 0, (struct sockaddr*)&to, sizeof to) ==
           (int)n;
}
static bool key_code_matches(const uint8_t key[32], const char* code) {
    if (!pc_identity_code_valid(code))
        return false;
    uint8_t h[20];
    pc_dht_sha1(key, 32, h);
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint64_t bits = (uint64_t)h[0] << 32 | (uint64_t)h[1] << 24 | (uint64_t)h[2] << 16 |
                    (uint64_t)h[3] << 8 | h[4];
    size_t n = strlen(code);
    for (int i = 0; i < 8; i++)
        if (code[n - 8 + i] != a[(bits >> (35 - 5 * i)) & 31])
            return false;
    return true;
}
static bool signed_ok(const uint8_t key[32], const uint8_t sig[64], const void* p, size_t n) {
    return pc_identity_verify(key, sig, p, n - 64);
}
static void sign_packet(void* p, size_t n) {
    pc_identity_sign(&identity, (uint8_t*)p + n - 64, p, n - 64);
}
static void fail(const char* why) {
    pc_log_line("match: fail '%s' (state was %d, net active %d)", why ? why : "?", (int)state,
        (int)pc_net_active());
    state = PC_MATCH_FAIL;
    failure = why;
    /* Keep the node warm for the retry; only the search stops. */
    pc_dht_idle();
    if (pc_net_active())
        pc_net_disconnect();
    if (mode == PC_MATCH_RANKED && pc_rank_session_state(NULL) != PC_RANK_SESSION_SAVED)
        pc_rank_session_abort(why);
}

static void pairing_topic(enum PcNetMatchMode m, const char* direct, uint8_t out[20]) {
    char text[64];
    /* Direct already refuses a Tag Battle/plain-VS mismatch via digest()'s
     * compatibility hash (fail("Peer is on a different build, disc or game
     * mode")) - a manually-exchanged code is already scoped to one peer, so
     * there is nothing to separate here. Ranked has no Tag Battle entry
     * point (SEL_TAG_UNRANKED is the only online row this mod's menu adds
     * past Direct/LAN) and stays untouched. Unranked pairs with whoever else
     * is searching the same pool, so a Tag Battle searcher's own topic hash
     * needs to differ from a plain-VS one - otherwise receive()'s
     * memcmp(h->topic, topic, 20) at line 361 would pass for a plain-VS
     * candidate, waste a MatchHello/MatchOffer round trip, and only then
     * fail on the mode check the offer/ack path already carries (the same
     * outcome, just later and noisier) instead of never matching at all.
     * The DHT announce topic is split the same way (unranked_pool() below),
     * so the two pools never even see each other as candidates. */
    int n = m == PC_MATCH_DIRECT ?
                snprintf(text, sizeof text, "meleepc/match/v1/direct/%s", direct) :
                snprintf(text, sizeof text,
                    m == PC_MATCH_RANKED      ? "meleepc/match/v1/ranked" :
                    TagAssist_IsTagBattleOn() ? "meleepc/match/v1/unranked/tag" :
                                                "meleepc/match/v1/unranked");
    pc_dht_sha1(text, n > 0 ? (size_t)n : 0, out);
}

/* PC_DHT_UNRANKED pool name: Tag Battle searchers announce under their own
 * DHT topic, plain VS keeps the original one. */
static const char* unranked_pool(void) {
    return TagAssist_IsTagBattleOn() ? "tag" : NULL;
}

static void direct_slot_for(const char* code) {
    char material[64];
    int n = snprintf(material, sizeof material, "%s/%s", DIRECT_SALT, code);
    pc_identity_derive(&direct_slot, material, n > 0 ? (size_t)n : 0);
}
static void put_be(uint8_t* p, uint64_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--, v >>= 8)
        p[i] = (uint8_t)v;
}
/* Our own address on the local network: the source address the OS would use
 * to reach the internet. connect() on UDP only picks a route; nothing is sent. */
static uint32_t lan_address(void) {
    uint32_t result = 0;
    MatchSocket s = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET)
        return 0;
#else
    if (s < 0)
        return 0;
#endif
    struct sockaddr_in to = {0}, self = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);
    socklen_t length = sizeof self;
    if (!connect(s, (struct sockaddr*)&to, sizeof to) &&
        !getsockname(s, (struct sockaddr*)&self, &length))
        result = self.sin_addr.s_addr;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return result;
}
/* Hairpin workaround: two players behind the same router see each other's
 * public address, and many routers drop packets sent to their own public
 * address from inside. Slippi dials the LAN address in that case; we add it
 * (or a LAN broadcast) as an extra Hello destination alongside the public one. */
static bool add_hello_target(struct pc_dht_endpoint ep) {
    for (unsigned i = 0; i < hello_target_count; i++)
        if (hello_targets[i].address == ep.address && hello_targets[i].port == ep.port)
            return false;
    if (hello_target_count < HELLO_TARGETS)
        hello_targets[hello_target_count++] = ep;
    else
        hello_targets[hello_target_cursor++ % HELLO_TARGETS] = ep;
    next_target_hello = 0;
    return true;
}
static bool same_public_ip(uint32_t address) {
    struct pc_dht_endpoint self;
    return pc_dht_external_endpoint(&self) && self.address == address;
}
static bool direct_record_read(
    const uint8_t* v, size_t n, struct pc_dht_endpoint* out, struct pc_dht_endpoint* lan) {
    if (n != DIRECT_RECORD_BYTES || memcmp(v, "MPD1", 4) || !key_code_matches(v + 4, target) ||
        !pc_identity_verify(v + 4, v + DIRECT_SIGNED_BYTES, v, DIRECT_SIGNED_BYTES))
        return false;
    int64_t when = 0;
    for (int i = 0; i < 8; i++)
        when = (int64_t)((uint64_t)when << 8 | v[42 + i]);
    int64_t now = (int64_t)time(NULL);
    if (when < now - DIRECT_MAX_AGE || when > now + DIRECT_MAX_AGE)
        return false;
    memcpy(&out->address, v + 36, 4);
    out->port = (uint16_t)(v[40] << 8 | v[41]);
    memcpy(&lan->address, v + 50, 4);
    lan->port = (uint16_t)(v[54] << 8 | v[55]);
    return out->address && out->port;
}
static void direct_put_result(const PcDhtItemResult* r, void* context) {
    (void)context;
    direct_pending = direct_put_inflight = false;
    if (r->status == PC_DHT_ITEM_CANCELLED)
        return;
    bool ok = r->status == PC_DHT_ITEM_OK && r->acknowledgements;
    uint32_t ip = ntohl(direct_endpoint.address);
    pc_log_line("match: direct record %s for %u.%u.%u.%u:%u (status=%d acks=%u)",
        ok ? "published" : "not published", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF,
        ip & 0xFF, direct_endpoint.port, (int)r->status, r->acknowledgements);
    direct_next = SDL_GetTicks() + (ok ? DIRECT_REPUBLISH_MS : 10000);
    if (ok) {
        direct_fresh_until = direct_next;
        direct_fresh_port = pc_dht_port();
    }
}
static void direct_get_result(const PcDhtItemResult* r, void* context) {
    (void)context;
    direct_pending = false;
    if (r->status == PC_DHT_ITEM_CANCELLED)
        return;
    struct pc_dht_endpoint ep, lan;
    if (r->status == PC_DHT_ITEM_OK && direct_record_read(r->value, r->value_length, &ep, &lan)) {
        uint32_t ip = ntohl(ep.address);
        bool local = lan.address && lan.port && same_public_ip(ep.address);
        pc_log_line("match: direct record for %s -> %u.%u.%u.%u:%u%s", target, ip >> 24,
            (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, ep.port,
            local ? " (same network, also dialing its LAN address)" : "");
        add_hello_target(ep);
        if (local)
            add_hello_target(lan);
        direct_next = SDL_GetTicks() + DIRECT_REPOLL_MS; /* pick up a host re-publish */
    } else {
        pc_log_line("match: no direct record for %s yet (status=%d)", target, (int)r->status);
        direct_next = SDL_GetTicks() + 2000;
    }
}
/* Direct connect fast path, alongside (not instead of) the announce search:
 * the host publishes its NAT-observed endpoint, the dialer fetches it and
 * starts sending Hellos straight away. A host behind a restrictive NAT still
 * needs to find the dialer through the announce search to open its side. */
/* Publish our NAT-observed endpoint under our connect code. Needs the DHT's
 * NAT consensus, so it can decline; the caller retries. */
static bool direct_publish(void) {
    struct pc_dht_endpoint ep;
    if (!pc_dht_external_endpoint(&ep))
        return false; /* NAT consensus needs a few more DHT replies */
    uint8_t record[DIRECT_RECORD_BYTES];
    int64_t when = (int64_t)time(NULL);
    memcpy(record, "MPD1", 4);
    memcpy(record + 4, identity.public_key, 32);
    memcpy(record + 36, &ep.address, 4);
    put_be(record + 40, ep.port, 2);
    put_be(record + 42, (uint64_t)when, 8);
    uint32_t lan = lan_address();
    memcpy(record + 50, &lan, 4);
    put_be(record + 54, pc_dht_port(), 2);
    pc_identity_sign(&identity, record + DIRECT_SIGNED_BYTES, record, DIRECT_SIGNED_BYTES);
    direct_endpoint = ep;
    direct_pending = direct_put_inflight = pc_dht_item_put(&direct_slot, DIRECT_SALT,
        strlen(DIRECT_SALT), when, record, sizeof record, direct_put_result, NULL);
    if (direct_pending) {
        uint32_t ip = ntohl(ep.address);
        pc_log_line("match: publishing direct record for %u.%u.%u.%u:%u", ip >> 24,
            (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF, ep.port);
    }
    return direct_pending;
}
static void direct_poll(uint64_t now) {
    if (mode != PC_MATCH_DIRECT || have_peer)
        return;
    if (direct_pending || now < direct_next || !pc_dht_ready() || pc_dht_item_busy())
        return;
    if (target[0]) {
        direct_pending = pc_dht_item_get_first(direct_slot.public_key, DIRECT_SALT,
            strlen(DIRECT_SALT), (int64_t)time(NULL) - DIRECT_MAX_AGE, direct_get_result, NULL);
        if (direct_pending && !direct_get_logged) {
            direct_get_logged = true;
            pc_log_line("match: direct record lookup started for %s", target);
        }
    } else {
        direct_publish();
    }
    if (!direct_pending)
        direct_next = now + 2000;
}
/* Direct Connect code entry: the host code is fixed per identity, so start
 * publishing the record while the player is still on the entry screen. The
 * put takes tens of seconds; begun here it is ready (or nearly) by the time
 * a friend dials, instead of starting only after START. */
void pc_net_match_prepublish(void) {
    pc_net_match_warm();
    if (state == PC_MATCH_SEARCH || state == PC_MATCH_CONNECT || state == PC_MATCH_READY ||
        pc_net_active() || !load_identity())
        return;
    uint64_t now = SDL_GetTicks();
    if (direct_pending || now < direct_next || !pc_dht_ready() || pc_dht_item_busy())
        return;
    direct_slot_for(identity.code);
    if (!direct_publish())
        direct_next = now + 2000;
}

static bool accept_ack(const MatchAck* a) {
    if (ntohl(a->magic) != MATCH_MAGIC || a->version != MATCH_VERSION || a->type != 'A' ||
        a->mode != (uint8_t)mode || a->host_nonce != local_nonce || a->guest_nonce != peer_nonce ||
        memcmp(a->host_key, identity.public_key, 32) || memcmp(a->guest_key, peer_key, 32) ||
        memcmp(a->offer_hash, offer_hash, 20) || !signed_ok(peer_key, a->signature, a, sizeof *a))
        return false;
    ack = *a;
    ack_received = true;
    if (mode == PC_MATCH_RANKED && !proofs_ready)
        return true;
    intptr_t fd = pc_dht_take_socket();
    char ip[INET_ADDRSTRLEN];
    /* peer.address is already in network byte order, so hand it to inet_ntop
     * as-is. Wrapping it in `struct in_addr addr = {peer.address}` truncates
     * to the first octet wherever in_addr is a union with a u_char[4] member
     * first (MinGW), which dialled 74.0.0.0 for a peer at 74.244.47.247 and
     * left the match stuck until the connect timeout (#87). */
    inet_ntop(AF_INET, &peer.address, ip, sizeof ip);
    pc_log_line(
        "match: accept_ack -> connecting socket as host to %s:%u (seed=%u)", ip, peer.port, seed);
    if (!pc_net_connect_socket(fd, ip, peer.port, 0, seed)) {
#ifdef _WIN32
        closesocket((SOCKET)fd);
#else
        close((int)fd);
#endif
        fail("connection failed");
        return true;
    }
    state = PC_MATCH_CONNECT;
    pc_net_set_datagram_handler(NULL);
    return true;
}
static bool after_handoff(const void* data, size_t n, uint32_t address, uint16_t port) {
    if (!host && address == peer.address && port == peer.port && n == sizeof(MatchOffer)) {
        const MatchOffer* o = data;
        uint8_t hash[20];
        pc_dht_sha1(o, sizeof *o, hash);
        if (ntohl(o->magic) == MATCH_MAGIC && o->version == MATCH_VERSION && o->type == 'O' &&
            o->mode == (uint8_t)mode && o->host_nonce == peer_nonce &&
            o->guest_nonce == local_nonce && !memcmp(o->host_key, peer_key, 32) &&
            !memcmp(o->guest_key, identity.public_key, 32) && !memcmp(hash, offer_hash, 20) &&
            signed_ok(peer_key, o->signature, o, sizeof *o))
        {
            for (int p = 0; p < 3; p++) {
                pc_net_send_datagram(&ack, sizeof ack, address, port);
            }
            return true;
        }
    }
    return false;
}
static bool private_ip(uint32_t address) {
    uint32_t ip = ntohl(address);
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 ||
           (ip >> 16) == 0xA9FE || (ip >> 22) == (0x64400000u >> 22);
}
/* Relay defence: a Hello is only good from an address its sender signed.
 * IP only, not port: a NAT with per-destination port mapping shows each
 * peer a different port than the one the DHT nodes saw. A sender that does
 * not know its public IP yet is only believed from a private address (same
 * LAN); its retries carry the IP once the DHT's NAT consensus has it. */
static bool hello_source_ok(const MatchHello* h, uint32_t source) {
    if (h->from_public && source == h->from_public)
        return true;
    if (h->from_lan && source == h->from_lan)
        return true;
    return !h->from_public && private_ip(source);
}
/* Fill in (and re-sign for) our own addresses once they are known. */
static void hello_refresh_from(void) {
    static uint32_t lan;
    struct pc_dht_endpoint self;
    uint32_t pub = pc_dht_external_endpoint(&self) ? self.address : 0;
    if (!lan)
        lan = lan_address();
    if (pub == hello.from_public && lan == hello.from_lan)
        return;
    hello.from_public = pub;
    hello.from_lan = lan;
    sign_packet(&hello, sizeof hello);
}
static void receive(const void* data, size_t n, const struct pc_dht_endpoint* ep, void* unused) {
    (void)unused;
    if (n == sizeof(MatchHello)) {
        const MatchHello* h = data;
        const char* terminator = memchr(h->code, '\0', sizeof h->code);
        if (ntohl(h->magic) != MATCH_MAGIC || h->version != MATCH_VERSION || h->type != 'H' ||
            h->mode != (uint8_t)mode || h->nonce == local_nonce || memcmp(h->topic, topic, 20) ||
            !terminator || !key_code_matches(h->public_key, h->code) ||
            (mode == PC_MATCH_DIRECT && target[0] && strcmp(h->code, target)) ||
            !signed_ok(h->public_key, h->signature, h, sizeof *h))
            return;
        /* A genuine, authenticated Hello for our search that only disagrees
         * on compatibility (build/disc/Tag-Battle-vs-VS) gets a real reason
         * instead of silently timing out - everything else above stays a
         * bare drop, since those are the anti-forgery/anti-spoof checks. */
        if (memcmp(h->compatibility, compatibility, 20)) {
            if (!failure)
                fail("Peer is on a different build, disc or game mode");
            return;
        }
        if (!hello_source_ok(h, ep->address)) {
            static uint32_t logged;
            if (logged != ep->address) {
                uint32_t sip = ntohl(ep->address), cip = ntohl(h->from_public);
                logged = ep->address;
                pc_log_line("match: dropped %s's Hello relayed via %u.%u.%u.%u (it signed "
                            "%u.%u.%u.%u)",
                    h->code, sip >> 24, (sip >> 16) & 0xFF, (sip >> 8) & 0xFF, sip & 0xFF,
                    cip >> 24, (cip >> 16) & 0xFF, (cip >> 8) & 0xFF, cip & 0xFF);
            }
            return;
        }
        bool fresh = !have_peer;
        if (have_peer && (h->nonce != peer_nonce || memcmp(h->public_key, peer_key, 32) ||
                             ep->address != peer.address || ep->port != peer.port))
            return;
        memcpy(peer_key, h->public_key, 32);
        peer_nonce = h->nonce;
        peer = *ep;
        have_peer = true;
        failure = NULL;
        memcpy(opponent, h->code, sizeof opponent);
        host = memcmp(identity.public_key, peer_key, 32) < 0;
        uint32_t rip = ntohl(ep->address);
        pc_log_line("match: recv valid MatchHello from %u.%u.%u.%u:%u (peer=%s host=%d fresh=%d)",
            rip >> 24, (rip >> 16) & 0xFF, (rip >> 8) & 0xFF, rip & 0xFF, ep->port, h->code, host,
            fresh);
        if (fresh) {
            for (int p = 0; p < 3; p++) {
                send_packet(&hello, sizeof hello, ep); /* answer one-sided discovery burst */
            }
        }
        if (fresh)
            deadline = SDL_GetTicks() + (mode == PC_MATCH_RANKED ? 90000 : TIMEOUT_MS);
        if (host) {
            memset(&offer, 0, sizeof offer);
            offer.magic = htonl(MATCH_MAGIC);
            offer.version = MATCH_VERSION;
            offer.type = 'O';
            offer.mode = (uint8_t)mode;
            offer.host_nonce = local_nonce;
            offer.guest_nonce = peer_nonce;
            memcpy(offer.host_key, identity.public_key, 32);
            memcpy(offer.guest_key, peer_key, 32);
            memcpy(offer.compatibility, compatibility, 20);
            memcpy(offer.topic, topic, 20);
            if (!seed && !pc_identity_random(&seed, sizeof seed)) {
                fail("random source failed");
                return;
            }
            offer.seed = htonl(seed);
            sign_packet(&offer, sizeof offer);
            pc_dht_sha1(&offer, sizeof offer, offer_hash);
            if (!begin_rank()) {
                fail("rank history unavailable");
                return;
            }
            pc_log_line("match: sending MatchOffer to peer (seed=%u)", seed);
            send_packet(&offer, sizeof offer, &peer);
            next_send = SDL_GetTicks() + RETRY_MS;
        }
    } else if (n == sizeof(MatchOffer)) {
        const MatchOffer* o = data;
        if (ntohl(o->magic) != MATCH_MAGIC || o->version != MATCH_VERSION || o->type != 'O' ||
            !have_peer || ep->address != peer.address || ep->port != peer.port ||
            o->mode != (uint8_t)mode || o->guest_nonce != local_nonce ||
            o->host_nonce != peer_nonce || memcmp(o->guest_key, identity.public_key, 32) ||
            memcmp(o->host_key, peer_key, 32) || memcmp(o->compatibility, compatibility, 20) ||
            memcmp(o->topic, topic, 20) || !signed_ok(peer_key, o->signature, o, sizeof *o))
            return;
        if (host || (offer_received && memcmp(&offer, o, sizeof offer)))
            return;
        offer_received = true;
        seed = ntohl(o->seed);
        offer = *o;
        pc_dht_sha1(o, sizeof *o, offer_hash);
        memset(&ack, 0, sizeof ack);
        ack.magic = htonl(MATCH_MAGIC);
        ack.version = MATCH_VERSION;
        ack.type = 'A';
        ack.mode = (uint8_t)mode;
        ack.host_nonce = peer_nonce;
        ack.guest_nonce = local_nonce;
        memcpy(ack.host_key, peer_key, 32);
        memcpy(ack.guest_key, identity.public_key, 32);
        memcpy(ack.offer_hash, offer_hash, 20);
        sign_packet(&ack, sizeof ack);
        if (!begin_rank()) {
            fail("rank history unavailable");
            return;
        }
        if (mode == PC_MATCH_RANKED && !proofs_ready)
            return;
        for (int p = 0; p < 3; p++) {
            send_packet(&ack, sizeof ack, ep);
        }
        intptr_t fd = pc_dht_take_socket();
        char ip[INET_ADDRSTRLEN];
        /* Network byte order already: see the host path above (#87). */
        inet_ntop(AF_INET, &ep->address, ip, sizeof ip);
        pc_log_line("match: recv valid MatchOffer -> sending MatchAck and connecting socket as "
                    "guest to %s:%u (seed=%u)",
            ip, ep->port, seed);
        if (!pc_net_connect_socket(fd, ip, ep->port, 1, seed)) {
#ifdef _WIN32
            closesocket((SOCKET)fd);
#else
            close((int)fd);
#endif
            fail("connection failed");
            return;
        }
        host = false;
        state = PC_MATCH_CONNECT;
        pc_net_set_datagram_handler(after_handoff);
    } else if (host && n == sizeof(MatchAck) && ep->address == peer.address &&
               ep->port == peer.port)
        accept_ack(data);
}

static void reset(bool keep_node);
bool pc_net_match_start(enum PcNetMatchMode m, const char* code) {
    /* Hosting Direct: a publish begun on the code entry screen must survive
     * this restart (reset() and pc_dht_start() would both cancel it). */
    bool keep_publish = m == PC_MATCH_DIRECT && !(code && *code) && direct_put_inflight;
    pc_dht_keep_item_on_start(keep_publish);
    reset(true);
    pc_dht_keep_item_on_start(false);
    if (keep_publish)
        direct_pending = direct_put_inflight = true;
    failure = NULL;
    mode = m;
    start_frame = -1;
    seed = 0;
    opponent[0] = 0;
    have_peer = false;
    handshake_done = barrier_sent = barrier_received = false;
    publication = 0;
    publication_reason = NULL;
    rank_begun = proofs_ready = ack_received = offer_received = proof_pending = false;
    recovery_immutable = false;
    proof_step = 0;
    identity_loaded = false; /* Reload display code from the same persistent key. */
    if (code && *code && (!pc_identity_code_valid(code) || strlen(code) >= sizeof target)) {
        fail("invalid connect code");
        return false;
    }
    snprintf(target, sizeof target, "%s", code ? code : "");
    if (!load_identity() || !pc_identity_random(&local_nonce, sizeof local_nonce)) {
        fail("identity unavailable");
        return false;
    }
    digest();
    const char* direct = target[0] ? target : identity.code;
    pairing_topic(m, direct, topic);
    direct_get_logged = false;
    direct_next = 0;
    hello_target_count = hello_target_cursor = 0;
    next_target_hello = 0;
    if (m == PC_MATCH_DIRECT)
        direct_slot_for(direct);
    pc_dht_keep_item_on_start(keep_publish);
    bool started = pc_dht_start((enum pc_dht_mode)m,
        m == PC_MATCH_UNRANKED ? unranked_pool() : direct, 0, (uint16_t)pc_get_net_port());
    pc_dht_keep_item_on_start(false);
    if (!started) {
        fail("DHT unavailable");
        return false;
    }
    /* A record published earlier is still good while the node kept its port. */
    if (m == PC_MATCH_DIRECT && !target[0] && SDL_GetTicks() < direct_fresh_until &&
        pc_dht_port() == direct_fresh_port)
        direct_next = direct_fresh_until;
    int on = 1; /* LAN broadcast Hellos, see add_hello_target() */
    setsockopt((MatchSocket)pc_dht_socket(), SOL_SOCKET, SO_BROADCAST, (const char*)&on, sizeof on);
    memset(&hello, 0, sizeof hello);
    hello.magic = htonl(MATCH_MAGIC);
    hello.version = MATCH_VERSION;
    hello.type = 'H';
    hello.mode = (uint8_t)m;
    hello.nonce = local_nonce;
    memcpy(hello.public_key, identity.public_key, 32);
    memcpy(hello.compatibility, compatibility, 20);
    memcpy(hello.topic, topic, 20);
    memcpy(hello.code, identity.code, sizeof hello.code);
    sign_packet(&hello, sizeof hello);
    hello_refresh_from();
    pc_dht_set_datagram_callback(receive, NULL);
    state = PC_MATCH_SEARCH;
    deadline = 0;
    next_send = 0;
    return true;
}
void pc_net_match_poll(void) {
    uint64_t now = SDL_GetTicks();
    if (state == PC_MATCH_SEARCH) {
        pc_dht_poll();
        if (state != PC_MATCH_SEARCH)
            return;
        if (mode == PC_MATCH_RANKED) {
            poll_proofs();
            if (proof_step < 0) {
                fail("rank public history verification failed");
                return;
            }
            if (proofs_ready) {
                if (host && ack_received) {
                    accept_ack(&ack);
                    if (state != PC_MATCH_SEARCH)
                        return;
                } else if (!host && offer_received) {
                    receive(&offer, sizeof offer, &peer, NULL);
                    if (state != PC_MATCH_SEARCH)
                        return;
                }
            }
        }
        direct_poll(now);
        hello_refresh_from();
        struct pc_dht_endpoint ep;
        while (pc_dht_next_candidate(&ep)) {
            /* Paired already: a Hello now would lock that player onto us while
             * receive() ignores their answer, costing them TIMEOUT_MS. The DHT
             * rediscovers them if this attempt expires. */
            if (have_peer)
                continue;
            /* The DHT still holds announcements from our own earlier sessions
             * (same public IP, one of our recent ports): not a peer. Skipped
             * only in random-port mode, where a fixed port could genuinely
             * match another machine behind our router. */
            struct pc_dht_endpoint own;
            if (!pc_get_net_port() && pc_dht_external_endpoint(&own) && own.address == ep.address &&
                pc_dht_is_own_port(ep.port))
                continue;
            uint32_t cip = ntohl(ep.address);
            pc_log_line("match: sending MatchHello to %u.%u.%u.%u:%u (target=%s)", cip >> 24,
                (cip >> 16) & 0xFF, (cip >> 8) & 0xFF, cip & 0xFF, ep.port, target);
            for (int p = 0; p < 3; p++) {
                send_packet(&hello, sizeof hello, &ep);
            }
            /* Same public IP: another player behind our router (our own
             * announce has our own external port and is skipped). Routers
             * usually keep the local port as the public one, so broadcast
             * the Hello on the LAN to that port. */
            struct pc_dht_endpoint self;
            if (pc_dht_external_endpoint(&self) && self.address == ep.address &&
                self.port != ep.port &&
                add_hello_target((struct pc_dht_endpoint){htonl(INADDR_BROADCAST), ep.port}))
                pc_log_line(
                    "match: candidate shares our public IP, broadcasting on LAN port %u", ep.port);
        }
        if (!have_peer && hello_target_count && now >= next_target_hello) {
            for (unsigned i = 0; i < hello_target_count; i++)
                send_packet(&hello, sizeof hello, &hello_targets[i]);
            next_target_hello = now + HELLO_TARGET_MS;
        }
        if (have_peer && now >= next_send) {
            send_packet(
                host ? (void*)&offer : (void*)&hello, host ? sizeof offer : sizeof hello, &peer);
            next_send = now + RETRY_MS;
        }
        if (have_peer && now >= deadline) {
            if (mode == PC_MATCH_RANKED) {
                fail("rank proof pairing timed out");
                return;
            }
            have_peer = false;
            peer_nonce = 0;
            memset(peer_key, 0, sizeof peer_key);
            opponent[0] = 0;
            seed = 0;
            next_send = 0; /* peer attempt expired: remain queued in DHT */
            failure = "Could not connect to opponent. Searching again...";
        }
    } else if (state == PC_MATCH_FAIL) {
        pc_net_match_warm();
    } else if (state == PC_MATCH_CONNECT) {
        pc_net_poll();
        if (!handshake_done)
            handshake_done = host ? pc_net_host_match(seed, &start_frame) :
                                    pc_net_guest_wait_match(&seed, &start_frame);
        if (handshake_done && !barrier_sent) {
            if (!pc_net_send_reliable(0x11, NULL, 0)) {
                state = PC_MATCH_FAIL;
                failure = "ready barrier not sent";
                pc_log_line("match: ready barrier not sent");
            } else
                barrier_sent = true;
        }
        if (barrier_sent && !barrier_received) {
            uint8_t type, buf[256];
            int n;
            while ((n = pc_net_recv_reliable(&type, buf, sizeof buf)) >= 0) {
                if (type == 0x11 && n == 0)
                    barrier_received = true;
                else if (mode == PC_MATCH_RANKED)
                    pc_rank_session_receive(type, buf, n);
            }
        }
        if (barrier_received) {
            if (mode == PC_MATCH_RANKED) {
                pc_rank_session_poll();
                const char* why = NULL;
                int rs = pc_rank_session_state(&why);
                if (rs == PC_RANK_SESSION_FAILED) {
                    fail(why);
                    return;
                }
                if (rs == PC_RANK_SESSION_PLAY) {
                    if (pc_net_frame() > start_frame) {
                        fail("late ranked ready barrier");
                        return;
                    }
                    state = PC_MATCH_READY;
                    pc_net_set_datagram_handler(NULL);
                }
            } else if (pc_net_frame() > start_frame) {
                state = PC_MATCH_FAIL;
                failure = "late ready barrier";
                pc_log_line(
                    "match: late ready barrier (frame %d > start %d)", pc_net_frame(), start_frame);
            } else {
                state = PC_MATCH_READY;
                pc_net_set_datagram_handler(NULL);
            }
        }
        if (pc_net_handshake_state() == 3) {
            state = PC_MATCH_FAIL;
            failure = "match handshake failed";
            pc_log_line("match: match handshake failed");
        }
    }
}
/* Warm DHT between searches: after a failure the node stays open (idle) so a
 * retry skips bootstrap; it still needs polling to keep its table fresh. */
static void reset(bool keep_node) {
    if (pc_net_active())
        pc_log_line("match: reset (keep node %d) drops the active session, state was %d",
            (int)keep_node, (int)state);
    if (keep_node)
        pc_dht_idle();
    else
        pc_dht_stop();
    direct_pending = direct_put_inflight = false;
    hello_target_count = 0;
    publication = 0;
    publication_reason = NULL;
    proof_pending = false;
    pc_rank_session_stop();
    pc_net_set_datagram_handler(NULL);
    if (pc_net_active())
        pc_net_disconnect();
    state = PC_MATCH_FAIL;
    failure = "cancelled";
}
void pc_net_match_stop(void) {
    reset(false);
}
void pc_net_match_idle(void) {
    reset(true);
}
void pc_net_match_warm(void) {
    static uint64_t next_attempt;
    if (state == PC_MATCH_SEARCH || state == PC_MATCH_CONNECT || state == PC_MATCH_READY ||
        publication == 1 || pc_net_active())
        return;
    uint64_t now = SDL_GetTicks();
    if (pc_dht_socket() < 0) {
        if (now < next_attempt)
            return;
        next_attempt = now + 5000; /* e.g. port busy: do not rebind every frame */
        if (!pc_dht_warm((uint16_t)pc_get_net_port()))
            return;
    }
    pc_dht_poll();
}
int pc_net_match_state(const char** why) {
    if (why)
        *why = failure;
    return state;
}
bool pc_net_match_is_host(void) {
    return host;
}
int32_t pc_net_match_start_frame(void) {
    return start_frame;
}
uint32_t pc_net_match_seed(void) {
    return seed;
}
const char* pc_net_match_local_code(void) {
    return load_identity() ? identity.code : "";
}
const char* pc_net_match_profile_error(void) {
    load_identity();
    return profile_error;
}
const char* pc_net_match_profile_directory(void) {
    return load_identity() ? profile_directory : "";
}
enum PcNetMatchMode pc_net_match_mode(void) {
    return mode;
}
const char* pc_net_match_opponent_code(void) {
    return opponent;
}
const PcNetIdentity* pc_net_match_identity(void) {
    return &identity;
}
const uint8_t* pc_net_match_peer_key(void) {
    return peer_key;
}

/* Durable append is retained even when publication fails. Retry after leaving
 * the game; this deliberately disconnects before starting another DHT socket. */
bool pc_net_match_publish_rank(void) {
    if (publication == 1 || !pc_rank_session_record() ||
        !pc_rank_session_public_state(local_public))
        return false;
    if (pc_net_active())
        pc_net_disconnect();
    pc_dht_stop();
    pc_net_set_datagram_handler(NULL);
    const PcNetRankRecord* record = pc_rank_session_record();
    PcNetRankSet set;
    if (!pc_rank_set(record->games, record->game_count, &set) ||
        !pc_rank_encode(record, publication_wire))
        return false;
    publication_record = set.winner == (host ? 0 : 1);
    publication = 1;
    proof_pending = false;
    publication_deadline = SDL_GetTicks() + 60000;
    publication_reason = "rank saved locally; publishing";
    if (!pc_dht_start(PC_DHT_RANKED, identity.code, 0, (uint16_t)pc_get_net_port())) {
        publication = -1;
        publication_reason = "rank saved locally; DHT unavailable, retry available";
        return false;
    }
    return true;
}
void pc_net_match_poll_publication(void) {
    if (publication != 1)
        return;
    pc_dht_poll();
    if (publication == 1 && SDL_GetTicks() >= publication_deadline) {
        publication = -1;
        publication_reason = "rank saved locally; publication timed out, retry available";
    }
    if (publication != 1) {
        pc_dht_stop();
        return;
    }
    if (!proof_pending && pc_dht_ready()) {
        const char* salt = PC_RANK_BEP44_SALT;
        if (publication_record)
            proof_pending = pc_dht_item_put_immutable(
                publication_wire, sizeof publication_wire, publication_result, NULL);
        else
            proof_pending =
                pc_dht_item_put(&identity, salt, strlen(salt), public_sequence(local_public),
                    local_public, sizeof local_public, publication_result, NULL);
    }
}
int pc_net_match_publication(const char** reason) {
    if (reason)
        *reason = publication_reason;
    return publication;
}
