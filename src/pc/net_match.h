/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_MATCH_H
#define PC_NET_MATCH_H
#include "net_identity.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum PcNetMatchMode { PC_MATCH_DIRECT, PC_MATCH_UNRANKED, PC_MATCH_RANKED };
enum PcNetMatchState { PC_MATCH_SEARCH, PC_MATCH_CONNECT, PC_MATCH_READY, PC_MATCH_FAIL };
bool pc_net_match_publish_rank(void);
void pc_net_match_poll_publication(void);
/* 0 idle, 1 pending, 2 acknowledged, -1 failed (durable save preserved). */
int pc_net_match_publication(const char** reason);
bool pc_net_match_start(enum PcNetMatchMode mode, const char* target_code);
/* stop closes the DHT node; idle ends the attempt but keeps the node open.
 * warm opens (if needed) and polls an idle node while no search is running,
 * so menus and code entry pre-bootstrap it. */
void pc_net_match_stop(void);
void pc_net_match_idle(void);
void pc_net_match_warm(void);
/* The next pc_net_match_start also Hellos the last opponent's endpoint at
 * once (after-match rematch against the same player). */
void pc_net_match_rematch_hint(void);
/* Direct Connect code entry: warm the DHT and publish our direct record early. */
void pc_net_match_prepublish(void);
void pc_net_match_poll(void);
int pc_net_match_state(const char** why);
/* Matchmaking found an opponent and waits for the player to Accept or
 * Decline; the match only starts once both accept, and no answer within
 * 10 s is a decline. ping_ms is the round trip to it (-1 while measuring),
 * choice this side's PC_MATCH_CHOICE_*. False otherwise, including for
 * Direct Connect, which accepts on its own. */
enum { PC_MATCH_CHOICE_NONE, PC_MATCH_CHOICE_ACCEPT, PC_MATCH_CHOICE_DECLINE };
bool pc_net_match_pending(int* ping_ms, int* seconds_left, int* choice);
void pc_net_match_decide(bool accept);
bool pc_net_match_is_host(void);
int32_t pc_net_match_start_frame(void);
uint32_t pc_net_match_seed(void);
const char* pc_net_match_local_code(void);
const char* pc_net_match_profile_error(void);
const char* pc_net_match_profile_directory(void);
enum PcNetMatchMode pc_net_match_mode(void);
const char* pc_net_match_opponent_code(void);
const PcNetIdentity* pc_net_match_identity(void);
const uint8_t* pc_net_match_peer_key(void);
#ifdef __cplusplus
}
#endif
#endif
