/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_FIREWALL_H
#define PC_NET_FIREWALL_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Windows Firewall only asks about a UDP program once another machine's
 * packet has already been dropped, so the first same-network connect fails
 * before the player ever sees a prompt. The online lobby checks up front
 * instead and offers to add the rule itself. Off Windows nothing blocks. */
enum PcFirewallState {
    PC_FIREWALL_OPEN,    /* inbound play reaches this melee.exe */
    PC_FIREWALL_BLOCKED, /* no rule lets it in on the current network */
    PC_FIREWALL_ASKING,  /* pc_firewall_allow is waiting on the admin prompt */
    PC_FIREWALL_FAILED,  /* the prompt was declined or the rule did not help */
};
/* Rechecks now (a few ms) unless already known open, and returns the state. */
int pc_firewall_check(void);
int pc_firewall_state(void);
/* Asks for admin (UAC, always drawn on top) and replaces this melee.exe's
 * inbound rules with one allow rule, on a worker thread; the state goes
 * ASKING, then OPEN or FAILED. */
void pc_firewall_allow(void);
#ifdef __cplusplus
}
#endif
#endif
