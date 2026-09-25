/* SPDX-License-Identifier: GPL-3.0-or-later */
/* UPnP IGD client (see net_upnp.h): SSDP discovery, the device
 * description, and three SOAP actions (AddPortMapping,
 * GetExternalIPAddress, DeletePortMapping). Only what a UDP mapping needs,
 * all of it on one worker thread so a slow or silent router never stalls
 * the game. */
#include "net_upnp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE closesocket
typedef SOCKET UpnpSocket;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define CLOSE close
typedef int UpnpSocket;
#endif

void pc_log_line(const char* fmt, ...);
bool pc_get_net_upnp(void);

#define LEASE_SECONDS 3600
#define RENEW_MS (LEASE_SECONDS / 2 * 1000ull)
#define RETRY_MS 60000ull /* after a failed discovery or mapping */
#define DISCOVER_MS 2500
#define IO_MS 3000
#define QUIT_IO_MS 1000 /* the exit path's DeletePortMapping */
#define REPLY_BYTES 16384

typedef struct {
    uint32_t ip; /* network order */
    uint16_t port;
    char path[256];
} Url;
typedef struct {
    bool found;
    Url control;
    char service[80];
    uint32_t local_ip; /* our address toward the router, network order */
} Igd;

static SDL_Mutex* lock;
static SDL_Condition* wake;
static SDL_Thread* thread;
/* Guarded by lock. */
static uint16_t want_port;
static bool want_changed, quit;
static struct pc_dht_endpoint mapped; /* port 0 = no usable mapping */
static SDL_AtomicInt io_ms = {IO_MS};

static bool public_ip(uint32_t address) {
    uint32_t ip = ntohl(address);
    return (ip >> 24) != 0 && (ip >> 24) != 10 && (ip >> 24) != 127 && (ip >> 28) < 14 &&
           (ip >> 16) != 0xa9fe && (ip >> 20) != 0xac1 && (ip >> 16) != 0xc0a8 &&
           (ip >> 22) != 0x191;
}
static void ip_text(uint32_t address, char out[16]) {
    uint32_t ip = ntohl(address);
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

/* ---- sockets --------------------------------------------------------------- */
static void set_blocking(UpnpSocket s, bool blocking) {
#ifdef _WIN32
    u_long on = !blocking;
    ioctlsocket(s, FIONBIO, &on);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}
static void set_timeouts(UpnpSocket s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
}
static bool wait_socket(UpnpSocket s, bool write, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
    return select((int)s + 1, write ? NULL : &set, write ? &set : NULL, NULL, &t) > 0;
}
/* A connect bounded by io_ms (a plain one can hang ~20 s on a dead host). */
static bool tcp_connect(const Url* u, UpnpSocket* out) {
    UpnpSocket s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET)
        return false;
#else
    if (s < 0)
        return false;
#endif
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = u->ip;
    to.sin_port = htons(u->port);
    set_blocking(s, false);
    if (connect(s, (struct sockaddr*)&to, sizeof to)) {
#ifdef _WIN32
        bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        bool pending = errno == EINPROGRESS;
#endif
        int error = 0;
#ifdef _WIN32
        int size = sizeof error;
#else
        socklen_t size = sizeof error;
#endif
        if (!pending || !wait_socket(s, true, SDL_GetAtomicInt(&io_ms)) ||
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&error, &size) || error)
        {
            CLOSE(s);
            return false;
        }
    }
    set_blocking(s, true);
    set_timeouts(s, SDL_GetAtomicInt(&io_ms));
    *out = s;
    return true;
}

/* ---- HTTP ------------------------------------------------------------------ */
/* "http://a.b.c.d[:port][/path]" (IPv4 literals only, which is what routers
 * put in their LOCATION and URLBase). */
static bool parse_url(const char* text, size_t n, Url* u) {
    char host[64];
    if (n < 8 || SDL_strncasecmp(text, "http://", 7))
        return false;
    text += 7;
    n -= 7;
    size_t h = 0;
    while (h < n && text[h] != ':' && text[h] != '/')
        h++;
    if (!h || h >= sizeof host)
        return false;
    memcpy(host, text, h);
    host[h] = 0;
    if (inet_pton(AF_INET, host, &u->ip) != 1)
        return false;
    u->port = 80;
    size_t p = h;
    if (p < n && text[p] == ':') {
        u->port = (uint16_t)atoi(text + p + 1);
        while (p < n && text[p] != '/')
            p++;
    }
    size_t len = n - p;
    if (!u->port || len >= sizeof u->path)
        return false;
    if (!len) {
        strcpy(u->path, "/");
    } else {
        memcpy(u->path, text + p, len);
        u->path[len] = 0;
    }
    return true;
}
/* Undo Transfer-Encoding: chunked in place. */
static void dechunk(char* body) {
    char *in = body, *out = body;
    for (;;) {
        char* end;
        size_t size = (size_t)strtoul(in, &end, 16);
        char* line = strstr(end, "\r\n");
        if (!size || !line)
            break;
        in = line + 2;
        size_t have = strlen(in);
        if (size > have)
            size = have;
        memmove(out, in, size);
        out += size;
        in += size;
        if (in[0] == '\r' && in[1] == '\n')
            in += 2;
    }
    *out = 0;
}
/* One request, Connection: close. Returns the status code (0 on failure)
 * with the body in reply; local_ip, when given, gets our address toward
 * the server. */
static int http(const Url* u, const char* method, const char* headers, const char* body,
    char* reply, size_t cap, uint32_t* local_ip) {
    UpnpSocket s;
    if (!tcp_connect(u, &s))
        return 0;
    if (local_ip) {
        struct sockaddr_in self;
#ifdef _WIN32
        int size = sizeof self;
#else
        socklen_t size = sizeof self;
#endif
        *local_ip = getsockname(s, (struct sockaddr*)&self, &size) ? 0 : self.sin_addr.s_addr;
    }
    char host[16];
    ip_text(u->ip, host);
    char head[768];
    int n = snprintf(head, sizeof head,
        "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n%sContent-Length: %u\r\n\r\n",
        method, u->path, host, u->port, headers, (unsigned)(body ? strlen(body) : 0));
    bool sent = n > 0 && n < (int)sizeof head && send(s, head, n, 0) == n &&
                (!body || send(s, body, (int)strlen(body), 0) == (int)strlen(body));
    size_t have = 0;
    while (sent && have + 1 < cap) {
        int got = recv(s, reply + have, (int)(cap - 1 - have), 0);
        if (got <= 0)
            break;
        have += (size_t)got;
    }
    CLOSE(s);
    reply[have] = 0;
    int status = 0;
    char* split = strstr(reply, "\r\n\r\n");
    if (!sent || !split || sscanf(reply, "HTTP/%*d.%*d %d", &status) != 1)
        return 0;
    *split = 0;
    bool chunked = SDL_strcasestr(reply, "transfer-encoding: chunked") != NULL;
    memmove(reply, split + 4, strlen(split + 4) + 1);
    if (chunked)
        dechunk(reply);
    return status;
}
/* The text of <tag>...</tag> at or after from, NULL-safe; returns its
 * length (0 when absent) and where it starts. */
static size_t tag_text(const char* from, const char* limit, const char* tag, const char** at) {
    char open[48], close[48];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const char* a = from ? strstr(from, open) : NULL;
    if (!a || (limit && a >= limit))
        return 0;
    a += strlen(open);
    const char* b = strstr(a, close);
    if (!b || (limit && b > limit))
        return 0;
    while (a < b && (*a == ' ' || *a == '\r' || *a == '\n' || *a == '\t'))
        a++;
    *at = a;
    return (size_t)(b - a);
}

/* ---- IGD ------------------------------------------------------------------- */
/* Read a device description and pick its WAN connection service. */
static bool describe(const char* location, size_t n, Igd* g) {
    static const char* const services[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    Url where;
    char* xml = malloc(REPLY_BYTES);
    if (!xml || !parse_url(location, n, &where) ||
        http(&where, "GET", "", NULL, xml, REPLY_BYTES, &g->local_ip) != 200)
    {
        free(xml);
        return false;
    }
    Url base = where;
    const char* at;
    size_t len = tag_text(xml, NULL, "URLBase", &at);
    if (len)
        parse_url(at, len, &base);
    for (size_t i = 0; i < SDL_arraysize(services) && !g->found; i++) {
        for (const char* s = strstr(xml, "<service>"); s && !g->found;
            s = strstr(s + 1, "<service>"))
        {
            const char* end = strstr(s, "</service>");
            const char* type;
            size_t type_len = tag_text(s, end, "serviceType", &type);
            if (type_len != strlen(services[i]) || memcmp(type, services[i], type_len))
                continue;
            const char* control;
            size_t control_len = tag_text(s, end, "controlURL", &control);
            if (!control_len)
                continue;
            if (control_len >= 7 && !SDL_strncasecmp(control, "http://", 7)) {
                if (!parse_url(control, control_len, &g->control))
                    continue;
            } else {
                g->control = base;
                bool slash = control[0] == '/';
                if (control_len + !slash >= sizeof g->control.path)
                    continue;
                snprintf(g->control.path, sizeof g->control.path, "%s%.*s", slash ? "" : "/",
                    (int)control_len, control);
            }
            snprintf(g->service, sizeof g->service, "%s", services[i]);
            g->found = true;
        }
    }
    free(xml);
    return g->found;
}
static bool discover(Igd* g) {
    static const char* const targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:2",
        "urn:schemas-upnp-org:service:WANIPConnection:1"};
    memset(g, 0, sizeof *g);
    UpnpSocket s = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET)
        return false;
#else
    if (s < 0)
        return false;
#endif
    unsigned char ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    for (size_t i = 0; i < SDL_arraysize(targets); i++) {
        char search[256];
        int n = snprintf(search, sizeof search,
            "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
            "MX: 2\r\nST: %s\r\n\r\n",
            targets[i]);
        sendto(s, search, n, 0, (struct sockaddr*)&to, sizeof to);
    }
    char tried[4][256];
    int tried_count = 0;
    uint64_t deadline = SDL_GetTicks() + DISCOVER_MS;
    while (!g->found) {
        uint64_t now = SDL_GetTicks();
        if (now >= deadline || !wait_socket(s, false, (int)(deadline - now)))
            break;
        char reply[1536];
        int got = recv(s, reply, sizeof reply - 1, 0);
        if (got <= 0)
            continue;
        reply[got] = 0;
        const char* location = SDL_strcasestr(reply, "\nlocation:");
        if (!location)
            continue;
        location += 10;
        while (*location == ' ')
            location++;
        size_t len = strcspn(location, "\r\n");
        if (!len || len >= sizeof tried[0])
            continue;
        bool seen = false;
        for (int i = 0; i < tried_count; i++)
            seen |= strlen(tried[i]) == len && !memcmp(tried[i], location, len);
        if (seen || tried_count == 4)
            continue;
        memcpy(tried[tried_count], location, len);
        tried[tried_count++][len] = 0;
        if (!describe(location, len, g))
            pc_log_line("upnp: %s has no usable WAN service", tried[tried_count - 1]);
    }
    CLOSE(s);
    if (!g->found)
        pc_log_line("upnp: no router answered (UPnP off or unsupported), not forwarding");
    return g->found;
}
static int soap(const Igd* g, const char* action, const char* args, char* reply, size_t cap) {
    char headers[256], body[1024];
    snprintf(headers, sizeof headers,
        "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n", g->service,
        action);
    snprintf(body, sizeof body,
        "<?xml version=\"1.0\"?>\r\n<s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s "
        "xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n",
        action, g->service, args, action);
    return http(&g->control, "POST", headers, body, reply, cap, NULL);
}
static int soap_error(const char* reply) {
    const char* at;
    return tag_text(reply, NULL, "errorCode", &at) ? atoi(at) : 0;
}
static bool add_mapping(const Igd* g, uint16_t port, bool* permanent) {
    char local[16], args[640], reply[2048];
    ip_text(g->local_ip, local);
    int status = 0;
    for (int lease = LEASE_SECONDS;; lease = 0) {
        snprintf(args, sizeof args,
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
            "<NewProtocol>UDP</NewProtocol><NewInternalPort>%u</NewInternalPort>"
            "<NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
            "<NewPortMappingDescription>MeleeVS</NewPortMappingDescription>"
            "<NewLeaseDuration>%d</NewLeaseDuration>",
            port, port, local, lease);
        status = soap(g, "AddPortMapping", args, reply, sizeof reply);
        *permanent = !lease;
        /* 725 OnlyPermanentLeasesSupported: retry with an unlimited lease,
         * which DeletePortMapping at exit then takes down. */
        if (status == 200 || !lease || soap_error(reply) != 725)
            break;
    }
    if (status != 200)
        pc_log_line("upnp: router refused to forward UDP %u (HTTP %d, error %d)", port, status,
            soap_error(reply));
    return status == 200;
}
static uint32_t external_ip(const Igd* g) {
    char reply[2048], text[16];
    const char* at;
    uint32_t ip = 0;
    size_t len;
    if (soap(g, "GetExternalIPAddress", "", reply, sizeof reply) == 200 &&
        (len = tag_text(reply, NULL, "NewExternalIPAddress", &at)) && len < sizeof text)
    {
        memcpy(text, at, len);
        text[len] = 0;
        if (inet_pton(AF_INET, text, &ip) != 1)
            ip = 0;
    }
    return ip;
}
static void delete_mapping(const Igd* g, uint16_t port) {
    char args[256], reply[2048];
    snprintf(args, sizeof args,
        "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
        "<NewProtocol>UDP</NewProtocol>",
        port);
    int status = soap(g, "DeletePortMapping", args, reply, sizeof reply);
    pc_log_line("upnp: removed the UDP %u forward (HTTP %d)", port, status);
}

/* ---- worker ---------------------------------------------------------------- */
static void publish(uint32_t ip, uint16_t port) {
    SDL_LockMutex(lock);
    mapped.address = ip;
    mapped.port = port;
    SDL_UnlockMutex(lock);
}
static int worker(void* unused) {
    (void)unused;
#ifdef _WIN32
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w))
        return 0;
#endif
    Igd g = {0};
    uint16_t have = 0;
    uint64_t renew_at = 0, retry_at = 0;
    SDL_LockMutex(lock);
    while (!quit) {
        uint64_t now = SDL_GetTicks();
        if (want_changed) {
            want_changed = false;
            retry_at = 0;
        }
        uint16_t want = want_port;
        bool drop = have && have != want;
        bool map = want && (have != want || now >= renew_at) && now >= retry_at;
        if (!drop && !map) {
            uint64_t next = want ? (have == want ? renew_at : retry_at) : now + RETRY_MS;
            uint64_t wait = next > now ? next - now : 1;
            SDL_WaitConditionTimeout(wake, lock, (Sint32)(wait < RETRY_MS ? wait : RETRY_MS));
            continue;
        }
        SDL_UnlockMutex(lock);
        if (drop) {
            publish(0, 0);
            if (g.found)
                delete_mapping(&g, have);
            have = 0;
        }
        if (map) {
            bool permanent = false;
            if (!g.found && !discover(&g)) {
                retry_at = now + RETRY_MS;
            } else if (!add_mapping(&g, want, &permanent)) {
                g.found = false; /* look for the router again next time */
                retry_at = now + RETRY_MS;
            } else {
                uint32_t ip = external_ip(&g);
                char wan[16], local[16];
                ip_text(ip, wan);
                ip_text(g.local_ip, local);
                if (have != want)
                    pc_log_line("upnp: forwarding UDP %u -> %s:%u (%s lease, WAN %s)", want, local,
                        want, permanent ? "permanent" : "1 h", wan);
                have = want;
                renew_at = now + RENEW_MS; /* also redoes a permanent one a router reboot lost */
                if (public_ip(ip)) {
                    publish(ip, want);
                } else {
                    /* A router behind another NAT (carrier-grade, or a
                     * second router): the forward stops at the outer one. */
                    pc_log_line("upnp: the router's WAN address %s is not public, so the "
                                "forward does not help",
                        wan);
                    publish(0, 0);
                }
            }
        }
        SDL_LockMutex(lock);
    }
    SDL_UnlockMutex(lock);
    if (have && g.found)
        delete_mapping(&g, have);
    return 0;
}

void pc_upnp_want(uint16_t port) {
    bool enabled = pc_get_net_upnp();
    if (!lock) {
        if (!enabled || !port)
            return;
        lock = SDL_CreateMutex();
        wake = SDL_CreateCondition();
        if (lock && wake)
            thread = SDL_CreateThread(worker, "UPnP", NULL);
        if (!thread) {
            pc_log_line("upnp: could not start its thread");
            return;
        }
    }
    if (!thread)
        return;
    if (!enabled)
        port = 0;
    else if (!port)
        return;
    SDL_LockMutex(lock);
    if (want_port != port) {
        want_port = port;
        want_changed = true;
        SDL_SignalCondition(wake);
    }
    SDL_UnlockMutex(lock);
}
bool pc_upnp_mapped(struct pc_dht_endpoint* out) {
    if (!thread)
        return false;
    SDL_LockMutex(lock);
    bool ok = mapped.port != 0;
    if (ok && out)
        *out = mapped;
    SDL_UnlockMutex(lock);
    return ok;
}
void pc_upnp_shutdown(void) {
    if (!thread)
        return;
    SDL_LockMutex(lock);
    quit = true;
    SDL_SetAtomicInt(&io_ms, QUIT_IO_MS);
    SDL_SignalCondition(wake);
    SDL_UnlockMutex(lock);
    SDL_WaitThread(thread, NULL);
    thread = NULL;
}
