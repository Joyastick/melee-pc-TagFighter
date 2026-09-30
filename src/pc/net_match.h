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
/* PC_MATCH_PARTY: a party link (pc_net_match_party_start) is made; the search itself is over. */
enum PcNetMatchState { PC_MATCH_SEARCH, PC_MATCH_CONNECT, PC_MATCH_READY, PC_MATCH_FAIL, PC_MATCH_PARTY };
#define PC_NET_CONTACTS_MAX 16
typedef struct PcNetContact {
    char code[18];       /* NAME#SUFFIX as last seen */
    int64_t last_played; /* unix seconds */
} PcNetContact;
/* Direct Connect opponents this profile has played, newest first; a code is
 * only saved once a match actually connected. */
int pc_net_match_contacts(PcNetContact* out, int max);
bool pc_net_match_publish_rank(void);
void pc_net_match_poll_publication(void);
/* 0 idle, 1 pending, 2 acknowledged, -1 failed (durable save preserved). */
int pc_net_match_publication(const char** reason);
bool pc_net_match_start(enum PcNetMatchMode mode, const char* target_code);
/* stop closes the DHT node; idle ends the attempt but keeps the node open.
 * warm opens (if needed) and polls an idle node while no search is running,
 * so menus and code entry pre-bootstrap it. */
/* A 3-4 machine Direct match: the host passes NULL, guests the host's connect
 * code, and the usual pc_net_match_poll/state drive it to PC_MATCH_READY. The
 * host sees how many machines have joined (itself included) and closes the
 * lobby with group_begin (false with fewer than two). */
bool pc_net_match_group_start(const char* host_code);
int pc_net_match_group_count(void);
bool pc_net_match_group_begin(void);
/* A party link: the same lobby as a group, but with two machines that then
 * stay linked over the DHT socket (no game session) to agree on their
 * fighters and who is point, before they queue for Matchmaking together.
 * The host passes NULL and starts the lobby's roster on its own once the
 * partner is in; the search state ends at PC_MATCH_PARTY. The link lasts
 * until pc_net_party_leave, the partner goes silent, or the DHT node is
 * stopped (leaving the Online menus). */
bool pc_net_match_party_start(const char* host_code);
bool pc_net_party_linked(void);
/* The partner as a short label (its code's name, else its suffix), its
 * fighter (a CharacterKind, -1 unknown) and whether it is point. */
bool pc_net_party_partner(char label[10], int* fighter, bool* partner_is_point);
void pc_net_party_set_pick(int fighter); /* our own fighter, sent to the partner */
void pc_net_party_toggle_point(void);
void pc_net_party_leave(void);
void pc_net_party_poll(void);
/* Party Matchmaking. The machine that starts the search is the party's
 * leader and queues alone (the pairing server needs no change: it sees an
 * ordinary two-player search, on a topic of its own so a party is only
 * matched with another party). When the two leaders have paired, they swap
 * their parties, each sends the agreed roster to its partner, and all four
 * machines open one mesh session (group_connect). The partner is brought
 * into the lobby scene by pc_net_party_go_pending() and joins with
 * pc_net_match_party_follow(). */
bool pc_net_match_party_search(void);
bool pc_net_party_partner_searching(void);
bool pc_net_party_go_pending(void);
bool pc_net_match_party_follow(void);
/* The agreed party match while its session is up: our machine number, each
 * machine's fighter (a CharacterKind, machine order = team A's two, then B's)
 * and which of each team's two machines starts on point (0 or 1). */
typedef struct PcNetGroupMatch {
    int machines; /* 3 or 4 */
    int local;    /* our machine number: team A's machines first */
    int team_size[2]; /* machines per team: 2 is a party, 1 a solo or couch team */
    int fighter[4];   /* team A's two fighters, then B's; a party's are its machines' */
    int point[2];     /* which of each team's two fighters starts on point */
    bool human1[2];   /* a team of one: its second fighter is a couch player, else a CPU */
    int cpu_level[2]; /* ... at this level */
} PcNetGroupMatch;
bool pc_net_group_match(PcNetGroupMatch* out);
/* After a party match: link with our teammate again (their endpoint from the
 * roster, no lobby), so the Party page shows them once both are back in the
 * menus. False when there was no party match. */
bool pc_net_party_restore(void);
/* The machines of the last party match (even numbers are the teams' leaders). */
bool pc_net_party_is_leader(void);
/* Rematch with the same four machines after the session ended, each with the
 * fighter it now has: every machine tells the other three it is ready (over the
 * DHT socket, to the endpoints the last roster named), and when all four have
 * heard each other they open the session again on a fresh seed. Runs as a
 * search (PC_MATCH_SEARCH), then connects like any match. */
bool pc_net_match_party_rematch(int fighter);
/* The party teams as a lobby shows them: ours (ourselves first) or the
 * opponents', two fighters (a CharacterKind, -1 unknown) and which of them
 * starts on point (0 or 1). From the party link while searching, from the
 * opposing party once swapped, from the match once there is one. */
bool pc_net_party_team(bool ours, int fighter[2], int* point, int* second);
/* `second` is what the team's second fighter is: 0 a CPU assist, 1 a couch
 * player, 2 a teammate on another machine. */ /* every frame, from anywhere: keeps the link alive */
/* Machine i of the lobby as "NAME#SUFFIX" (the host while collecting, every
 * machine once the roster is out); false when not known yet. */
bool pc_net_match_group_member(int i, char out[18]);
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
