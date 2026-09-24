/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <aurora/aurora.h>
#ifdef __cplusplus
extern "C" {
#endif
void pc_launcher_configure(AuroraConfig* config);
/* 1: disc opened, 0: user quit, -1: launcher initialization failed. */
int pc_launcher_run(const char* command_line_disc, SDL_Window* window);
void pc_menu_init(SDL_Window* window);
void pc_menu_update(void);
void pc_menu_toggle(void);
union SDL_Event;
void pc_menu_event(const union SDL_Event* event);
bool pc_menu_is_open(void);
/* Friend's connect code, persisted; written by the in-game Direct Connect
 * entry as well as the launcher and F1 menu fields. */
void pc_set_net_target(const char* code);
/* MeleeVS Matchmaking team saved by TEAM SELECT (the 9-byte PcNetTeam wire
 * image, net.h), persisted in the launcher settings. get returns false when
 * none has been saved yet. */
bool pc_get_meleevs_team(uint8_t out[9]);
void pc_set_meleevs_team(const uint8_t team[9]);
#ifdef __cplusplus
}
#endif
