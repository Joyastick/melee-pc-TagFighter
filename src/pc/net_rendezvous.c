/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_rendezvous.h"
#include "net_identity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_timer.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif
#ifndef PC_RDV_NO_THREAD
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_thread.h>
#endif

void pc_log_line(const char* fmt, ...);

/* Compiled-in server, empty until one is deployed; the environment
 * overrides both. */
#ifndef PC_PAIRING_SERVER
#define PC_PAIRING_SERVER ""
#endif
#ifndef PC_PAIRING_KEY
#define PC_PAIRING_KEY ""
#endif

#define HDR 6
#define SIG 64
#define HELLO_SIZE 100
#define COOKIE_SIZE 100
#define JOIN_SIZE 110
#define QUEUED_SIZE 50
#define LEAVE_SIZE 50
#define MATCH_SIZE 110
#define HELLO_RETRY_MS 1000
#define HELLO_TRIES 3
#define JOIN_RETRY_MS 1000  /* until the first QUEUED */
#define KEEPALIVE_MS 10000  /* the server drops an entry silent for 30 s */
#define DOWN_RETRY_MS 30000 /* unreachable: try again this much later */
#define RESOLVE_RETRY_MS 60000

enum { RDV_OFF, RDV_RESOLVING, RDV_HELLO, RDV_QUEUE, RDV_MATCHED, RDV_DOWN };

static struct {
    int state;
    PcRdvSend send;
    uint8_t topic[20], nonce[8], cookie[16];
    struct pc_dht_endpoint lan, avoid, peer, peer_lan;
    bool queued, match_ready;
    int tries, unanswered;
    uint64_t next_send, started;
} rdv;

/* Process-wide configuration and the resolved server address. */
static bool config_loaded, enabled;
static char server_host[256], server_port[8];
static uint8_t server_key[32];
static struct pc_dht_endpoint server;
static bool server_known;
static uint32_t public_ip;
static uint64_t resolve_failed_at;
static bool resolve_failed;

static bool hex32(const char* s, uint8_t out[32]) {
    if (strlen(s) != 64)
        return false;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static void load_config(void) {
    if (config_loaded)
        return;
    config_loaded = true;
    const char* where = getenv("MELEE_PAIRING_SERVER");
    const char* key = getenv("MELEE_PAIRING_KEY");
    if (!where || !*where)
        where = PC_PAIRING_SERVER;
    if (!key || !*key)
        key = PC_PAIRING_KEY;
    const char* colon = strrchr(where, ':');
    if (!*where || !colon || colon == where || (size_t)(colon - where) >= sizeof server_host ||
        strlen(colon + 1) >= sizeof server_port || !hex32(key, server_key))
    {
        pc_log_line("pairing: no server configured, DHT only");
        return;
    }
    memcpy(server_host, where, (size_t)(colon - where));
    server_host[colon - where] = 0;
    snprintf(server_port, sizeof server_port, "%s", colon + 1);
    enabled = true;
}

static bool lookup(struct pc_dht_endpoint* out) {
    struct addrinfo hints = {0}, *list = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server_host, server_port, &hints, &list) || !list)
        return false;
    const struct sockaddr_in* a = (const struct sockaddr_in*)list->ai_addr;
    out->address = a->sin_addr.s_addr;
    out->port = ntohs(a->sin_port);
    freeaddrinfo(list);
    return true;
}

/* A numeric address resolves at once; a name resolves on a thread, as the
 * DHT's bootstrap names do, so a slow DNS never stalls a frame. */
#ifndef PC_RDV_NO_THREAD
static SDL_AtomicInt resolve_done; /* 0 running, 1 ok, 2 failed */
static struct pc_dht_endpoint resolved;
static bool resolving;
static int resolve_main(void* unused) {
    (void)unused;
    SDL_SetAtomicInt(&resolve_done, lookup(&resolved) ? 1 : 2);
    return 0;
}
#endif

static bool resolve_step(uint64_t now) {
    if (server_known)
        return true;
    struct in_addr numeric;
    if (inet_pton(AF_INET, server_host, &numeric) == 1) {
        server.address = numeric.s_addr;
        server.port = (uint16_t)atoi(server_port);
        server_known = true;
        return true;
    }
    if (resolve_failed && now < resolve_failed_at + RESOLVE_RETRY_MS)
        return false;
    resolve_failed = false;
#ifdef PC_RDV_NO_THREAD
    server_known = lookup(&server);
#else
    if (!resolving) {
        SDL_SetAtomicInt(&resolve_done, 0);
        SDL_Thread* t = SDL_CreateThread(resolve_main, "pairing resolve", NULL);
        if (!t) {
            resolve_failed = true;
            resolve_failed_at = now;
            return false;
        }
        SDL_DetachThread(t);
        resolving = true;
        return false;
    }
    int done = SDL_GetAtomicInt(&resolve_done);
    if (!done)
        return false;
    resolving = false;
    if (done == 1) {
        server = resolved;
        server_known = true;
    }
#endif
    if (!server_known) {
        pc_log_line("pairing: cannot resolve %s, DHT only", server_host);
        resolve_failed = true;
        resolve_failed_at = now;
    }
    return server_known;
}

static void put_endpoint(uint8_t* p, const struct pc_dht_endpoint* ep) {
    memcpy(p, &ep->address, 4);
    p[4] = (uint8_t)(ep->port >> 8);
    p[5] = (uint8_t)ep->port;
}

static struct pc_dht_endpoint get_endpoint(const uint8_t* p) {
    struct pc_dht_endpoint ep;
    memcpy(&ep.address, p, 4);
    ep.port = (uint16_t)(p[4] << 8 | p[5]);
    return ep;
}

static void header(uint8_t* p, char type) {
    memcpy(p, "MPS1", 4);
    p[4] = 1;
    p[5] = (uint8_t)type;
}

static void send_hello(void) {
    uint8_t p[HELLO_SIZE] = {0};
    header(p, 'H');
    memcpy(p + HDR, rdv.nonce, 8);
    rdv.send(p, sizeof p, &server);
}

static void send_join(void) {
    uint8_t p[JOIN_SIZE] = {0}, *q = p + HDR;
    header(p, 'J');
    memcpy(q, rdv.nonce, 8);
    memcpy(q + 8, rdv.cookie, 16);
    memcpy(q + 24, rdv.topic, 20);
    put_endpoint(q + 44, &rdv.lan);
    q[50] = 2; /* players wanted; 4 with 3-4 player online */
    put_endpoint(q + 51, &rdv.avoid);
    rdv.send(p, sizeof p, &server);
}

static void send_leave(void) {
    uint8_t p[LEAVE_SIZE] = {0};
    header(p, 'L');
    memcpy(p + HDR, rdv.nonce, 8);
    memcpy(p + HDR + 8, rdv.cookie, 16);
    memcpy(p + HDR + 24, rdv.topic, 20);
    rdv.send(p, sizeof p, &server);
}

static void log_ip(const char* what, const struct pc_dht_endpoint* ep, uint64_t ms) {
    uint32_t ip = ntohl(ep->address);
    pc_log_line("pairing: %s %u.%u.%u.%u:%u after %u ms", what, ip >> 24, (ip >> 16) & 0xFF,
        (ip >> 8) & 0xFF, ip & 0xFF, ep->port, (unsigned)ms);
}

void pc_rdv_start(const uint8_t topic[20], uint32_t lan_ip, uint16_t lan_port, PcRdvSend send) {
    pc_rdv_stop();
    load_config();
    if (!enabled || !send || !pc_identity_random(rdv.nonce, sizeof rdv.nonce))
        return;
    rdv.send = send;
    memcpy(rdv.topic, topic, 20);
    rdv.lan.address = lan_ip;
    rdv.lan.port = lan_port;
    rdv.state = RDV_RESOLVING;
}

void pc_rdv_stop(void) {
    if (rdv.state == RDV_QUEUE && rdv.queued)
        send_leave();
    memset(&rdv, 0, sizeof rdv);
}

void pc_rdv_poll(uint64_t now) {
    switch (rdv.state) {
    case RDV_RESOLVING:
        if (!rdv.started)
            rdv.started = now;
        if (resolve_step(now)) {
            rdv.state = RDV_HELLO;
            rdv.tries = 0;
            rdv.next_send = now;
        } else if (resolve_failed && !server_known) {
            rdv.state = RDV_OFF; /* DHT only for this search */
        }
        break;
    case RDV_HELLO:
        if (now < rdv.next_send)
            break;
        if (rdv.tries >= HELLO_TRIES) {
            pc_log_line(
                "pairing: server unreachable, DHT only (retrying in %d s)", DOWN_RETRY_MS / 1000);
            rdv.state = RDV_DOWN;
            rdv.next_send = now + DOWN_RETRY_MS;
            break;
        }
        send_hello();
        rdv.tries++;
        rdv.next_send = now + HELLO_RETRY_MS;
        break;
    case RDV_QUEUE:
        if (now < rdv.next_send)
            break;
        if (rdv.unanswered >= 3) {
            /* Probably an expired cookie (or a restarted server): start over. */
            rdv.state = RDV_HELLO;
            rdv.tries = 0;
            rdv.unanswered = 0;
            rdv.queued = false;
            rdv.next_send = now;
            break;
        }
        send_join();
        rdv.unanswered++;
        rdv.next_send = now + (rdv.queued ? KEEPALIVE_MS : JOIN_RETRY_MS);
        break;
    case RDV_DOWN:
        if (now >= rdv.next_send) {
            rdv.state = RDV_HELLO;
            rdv.tries = 0;
            rdv.next_send = now;
        }
        break;
    default:
        break;
    }
    if (rdv.state != RDV_OFF && !rdv.started)
        rdv.started = now;
}

bool pc_rdv_receive(const void* data, size_t size, const struct pc_dht_endpoint* from) {
    const uint8_t* p = data;
    if (size < HDR || memcmp(p, "MPS1", 4))
        return false;
    if (!server_known || from->address != server.address || from->port != server.port ||
        p[4] != 1 || rdv.state == RDV_OFF || memcmp(p + HDR, rdv.nonce, 8))
        return true;
    uint64_t now = SDL_GetTicks();
    switch (p[5]) {
    case 'C':
        if (size != COOKIE_SIZE || rdv.state != RDV_HELLO ||
            !pc_identity_verify(server_key, p + size - SIG, p, size - SIG))
            break;
        memcpy(rdv.cookie, p + HDR + 8, 16);
        {
            struct pc_dht_endpoint self = get_endpoint(p + HDR + 24);
            public_ip = self.address;
            log_ip("server cookie, we are", &self, now - rdv.started);
        }
        rdv.state = RDV_QUEUE;
        rdv.unanswered = 0;
        rdv.next_send = 0; /* JOIN now */
        break;
    case 'Q':
        if (size != QUEUED_SIZE || rdv.state != RDV_QUEUE || memcmp(p + HDR + 24, rdv.topic, 20))
            break;
        /* Unsigned: a forged one can at worst hand us a bad cookie, and it
         * already had to echo our random nonce. */
        memcpy(rdv.cookie, p + HDR + 8, 16);
        if (!rdv.queued)
            pc_log_line("pairing: queued");
        rdv.queued = true;
        rdv.unanswered = 0;
        break;
    case 'M':
        if (size != MATCH_SIZE || (rdv.state != RDV_QUEUE && rdv.state != RDV_HELLO) ||
            memcmp(p + HDR + 8, rdv.topic, 20) ||
            !pc_identity_verify(server_key, p + size - SIG, p, size - SIG))
            break;
        rdv.peer = get_endpoint(p + HDR + 28);
        rdv.peer_lan = get_endpoint(p + HDR + 34);
        rdv.match_ready = true;
        rdv.queued = false; /* the server already took us out of the queue */
        rdv.state = RDV_MATCHED;
        log_ip("MATCH with", &rdv.peer, now - rdv.started);
        break;
    default:
        break;
    }
    return true;
}

bool pc_rdv_take_match(struct pc_dht_endpoint* peer, struct pc_dht_endpoint* peer_lan) {
    if (!rdv.match_ready)
        return false;
    rdv.match_ready = false;
    *peer = rdv.peer;
    *peer_lan = rdv.peer_lan;
    return true;
}

uint32_t pc_rdv_public_ip(void) {
    return public_ip;
}

void pc_rdv_retry_avoiding(const struct pc_dht_endpoint* peer) {
    if (rdv.state != RDV_MATCHED && rdv.state != RDV_QUEUE)
        return;
    rdv.avoid = *peer;
    rdv.state = RDV_QUEUE;
    rdv.queued = false;
    rdv.unanswered = 0;
    rdv.next_send = 0;
}
