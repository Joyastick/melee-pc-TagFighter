/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_UPNP_H
#define PC_NET_UPNP_H
/* Automatic port forwarding through the router's UPnP Internet Gateway
 * Device. Behind a symmetric ("strict") NAT every destination sees its own
 * public port, so the port we announce reaches nobody; a mapping of the
 * online socket's UDP port (external port = local port) makes it
 * reachable from anyone. Because the external port equals the local one,
 * the DHT announce and the pairing server's MATCH (which carries our local
 * port) already point at it; the Direct records use pc_upnp_mapped().
 *
 * Everything runs on one background thread and fails quietly: no router,
 * UPnP turned off, a refused mapping or a router that is itself behind
 * another NAT only leave a line in the log. Launcher toggle
 * "Automatic port forwarding (UPnP)", on by default; MELEE_UPNP=0 turns it
 * off for a run. */
#include "net_dht.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* The online socket is bound to port: keep it mapped (renewed until exit).
 * 0 means "not known right now" and keeps the current mapping, since the
 * game session borrows the same socket. Cheap; call it every poll. */
void pc_upnp_want(uint16_t port);
/* Our public endpoint through the mapping, once the router made one and
 * reported a public WAN address. */
bool pc_upnp_mapped(struct pc_dht_endpoint* out);
/* Remove the mapping and stop the thread (exit path). */
void pc_upnp_shutdown(void);
#ifdef __cplusplus
}
#endif
#endif
