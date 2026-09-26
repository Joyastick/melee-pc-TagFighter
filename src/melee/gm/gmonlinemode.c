#include "gmonlinemode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <melee/lb/forward.h>

#include "forward.h"
#include "gm_1601.h"
#include "gm_1A36.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmresult.h"
#include "gmscene.h"
#include "gmvsmelee.h"
#include "types.h"
#include <dolphin/pad.h>
#include <melee/if/if_2FD9.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/inlines.h>
#include <melee/mn/types.h>
#include <melee/mod/tag_assist.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#ifdef TARGET_PC
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/net_identity.h"
#include "pc/net_match.h"
#include "pc/net_rank_session.h"
#include "pc/net_rendezvous.h"
extern const char* pc_get_net_target(void);
extern void pc_set_net_target(const char* code);
extern bool pc_get_meleevs_team(uint8_t out[9]);
extern void pc_set_meleevs_team(const uint8_t team[9]);
#include "pc/pc.h"
#endif

/* GM_ONLINE: lobby -> CSS -> SSS -> VS -> (sudden death) -> results -> CSS,
 * the vanilla VS flow (gmvsmode.c) on a private VsModeData that both peers
 * reset identically on entering the lobby. The lobby is the Double Dash LAN
 * counter screen (docs/netcode-plan.md §8): pc_lan_* announces us, counts
 * peers, and the first Start elects the host; once pc_lan_state() reports
 * the match (2) both peers leave for the CSS on the same synced frame. From
 * there every scene runs on synced inputs: the local player is P1 when
 * hosting and P2 as guest, ports 3/4 report no controller, and the RULES
 * handshake (net.c) has made GameRules/GamePrefs/unlock/frozen-stadium
 * identical on both sides. B on the CSS goes back to the lobby. */

enum {
    state_lobby = 0,
    state_css = 1,
    state_sss = 2,
    state_vs = 3,
    state_sudden_death = 4,
    state_results = 5, /* last: gmVsMelee_ExitResults skips challengers */
};

static bool awaiting_rank_result;
static void onEnterLobby(GameModeState*);
static void onEnterCss(GameModeState*);
static void onExitCss(GameModeState*);
static void onEnterSss(GameModeState*);
static void onExitSss(GameModeState*);
static void onEnterVs(GameModeState*);
static void onExitVs(GameModeState*);
static void onEnterSuddenDeath(GameModeState*);
static void onExitSuddenDeath(GameModeState*);
static void onEnterResults(GameModeState*);
static void onExitResults(GameModeState*);

GameModeState gm_Mode_Online_States[] = {
    {
        state_lobby,
        lbDvdPreload_3,
        0,
        onEnterLobby,
        NULL,
        {
            GS_ONLINE_LOBBY,
            NULL,
            NULL,
        },
    },
    {
        state_css,
        lbDvdPreload_3,
        0,
        onEnterCss,
        onExitCss,
        {
            GS_CSS,
            &gmVsMelee_CssData,
            &gmVsMelee_CssData,
        },
    },
    {
        state_sss,
        lbDvdPreload_3,
        0,
        onEnterSss,
        onExitSss,
        {
            GS_SSS,
            &gmVsMelee_SssData,
            &gmVsMelee_SssData,
        },
    },
    {
        state_vs,
        lbDvdPreload_3,
        0,
        onEnterVs,
        onExitVs,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        state_sudden_death,
        lbDvdPreload_3,
        0,
        onEnterSuddenDeath,
        onExitSuddenDeath,
        {
            GS_SUDDEN_DEATH,
            &gmVsMelee_StartData,
            &gmVsMelee_SuddenDeathExitInfo,
        },
    },
    {
        state_results,
        lbDvdPreload_3,
        0,
        onEnterResults,
        onExitResults,
        {
            GS_RESULTS,
            &gmVsMelee_ResultsEnterData,
            NULL,
        },
    },
    { GM_GAMEMODESTATE_TERMINATE },
};

static OnlineKind online_kind;
static VsModeData online_vs;

void gmOnline_SetKind(OnlineKind kind)
{
    online_kind = kind;
}

OnlineKind gmOnline_GetKind(void)
{
    return online_kind;
}

/* MeleeVS TEAM SELECT runs the CSS inside GM_ONLINE with no network: the
 * lobby hands straight to the CSS, and confirming it saves the team the
 * next Matchmaking search takes along (the launcher's meleevs_team). */
static bool team_select_css;
static bool team_select_then_search;
static bool team_select_then_rematch; /* confirming reconnects to the last opponent */

/* After a Matchmaking game: RESULTS leaves for the lobby with the session
 * still up (as Ranked's awaiting_rank_result does) and each side picks one
 * of four options. Both machines read both picks from the synced pads, so
 * they reach the same outcome on the same frame without a message. */
enum {
    AM_SAME_REMATCH,
    AM_CHANGE_REMATCH,
    AM_SAME_SEARCH,
    AM_CHANGE_SEARCH,
    AM_MENU, /* B with nothing picked: leave */
};
static bool after_match;
static bool am_live;          /* false once the opponent is gone */
static int am_cursor[2];      /* per machine, 0 = host */
static int am_pick[2];        /* -1 while choosing */
static unsigned am_game;      /* rematches played in this session */
/* The last matchmade game's result for the after-match title: the host's
 * team is team 0 (red), the guest's team 1 (blue), see matchmadeBuild. */
enum { AM_RESULT_RED, AM_RESULT_BLUE, AM_RESULT_NONE };
static int am_result = AM_RESULT_NONE;
/* A decision that ends the session is carried out AM_HOLD_FRAMES later:
 * in lockstep that guarantees the other machine has also simulated the
 * decision frame, so its goodbye can never be mistaken for a quit. */
#define AM_HOLD_FRAMES 30
enum { AM_OUT_NONE, AM_OUT_LEAVE, AM_OUT_TEAM_REMATCH };
static int am_outcome;
static int am_leave_frame;
static bool rematch_direct;   /* the lobby reconnects to the last opponent */
static bool rematch_host;
static char rematch_code[18]; /* the host's connect code */

bool gmOnline_IsTeamSelect(void)
{
    return team_select_css;
}

void gmOnline_SetTeamSelectThenSearch(bool value)
{
    team_select_then_search = value;
}

#ifdef TARGET_PC
static bool loadSavedTeam(PcNetTeam* team)
{
    uint8_t raw[9];
    _Static_assert(sizeof(PcNetTeam) == sizeof raw, "PcNetTeam wire image");
    if (!pc_get_meleevs_team(raw)) {
        return false;
    }
    memcpy(team, raw, sizeof raw);
    for (int i = 0; i < 2; i++) {
        if (team->fighter[i].ckind < 0 || team->fighter[i].ckind >= CKind_Playable_Count) {
            return false;
        }
    }
    return team->point <= 1;
}

bool gmOnline_HasSavedTeam(void)
{
    PcNetTeam team;
    return loadSavedTeam(&team);
}

/* The TEAM SELECT CSS starts on the saved team (or an empty port 1 and a
 * CPU port 2): both on Red, CPU level 9, point as saved. */
static void teamSelectPrefill(void)
{
    PlayerInitData* p = online_vs.start.players;
    PcNetTeam team;
    bool saved = loadSavedTeam(&team);
    for (int i = 0; i < 2; i++) {
        p[i].slot_type = i == 0 || (saved && team.fighter[1].human) ? Gm_PKind_Human
                                                                    : Gm_PKind_Cpu;
        p[i].team = 0;
        p[i].cpu_level = 9;
        if (saved) {
            p[i].ckind = team.fighter[i].ckind;
        }
    }
    TagAssist_SetExplicitPoint(0, saved ? team.point : 0);
}

/* What TEAM SELECT's CSS confirmed: port 1 is always this player, port 2
 * the partner or CPU, point by the CSS's own Z choice. Costumes are left
 * to Matchmaking's team colors. */
static void teamSelectSave(const PlayerInitData* p)
{
    PcNetTeam team;
    memset(&team, 0, sizeof team);
    for (int i = 0; i < 2; i++) {
        team.fighter[i].ckind = (int8_t) p[i].ckind;
        team.fighter[i].human = i == 0 || p[i].slot_type == Gm_PKind_Human;
        team.fighter[i].cpu_level = team.fighter[i].human ? 0 : 9;
    }
    team.point = TagAssist_IsPortPoint(1) ? 1 : 0;
    pc_set_meleevs_team((const uint8_t*) &team);
    pc_log_line("online: team select saved %d%s + %d (%s)%s", team.fighter[0].ckind,
                team.point == 0 ? " point" : "", team.fighter[1].ckind,
                team.fighter[1].human ? "human" : "cpu", team.point == 1 ? " point" : "");
}
#else
bool gmOnline_HasSavedTeam(void)
{
    return false;
}
#endif

#ifdef TARGET_PC
static bool internetLobby(void);
#endif

void onEnterLobby(UNUSED GameModeState* state)
{
#ifdef TARGET_PC
    /* Terminal sets leave Results deterministically. Keep transport alive
     * here while both peers finish signing and durable saving. */
    awaiting_rank_result = online_kind == ONLINE_KIND_RANKED &&
        (pc_rank_session_set_complete() ||
         pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    if (!awaiting_rank_result && !after_match) {
        if (pc_net_active()) {
            pc_log_line("lobby: entering the lobby scene drops the active session");
        }
        /* Internet lobbies keep the DHT node the online menu warmed up; LAN
         * and Profile close it so LAN can bind the same port. TEAM SELECT
         * keeps it too: Matchmaking usually comes next. */
        if (internetLobby() || online_kind == ONLINE_KIND_TEAM_SELECT) {
            pc_net_match_idle();
        } else {
            pc_net_match_stop();
        }
        pc_net_disconnect();
    }
    pc_lan_stop();
#endif
    /* Same CSS start state on both peers: two human doors, nothing picked. */
    gm_InitVsMode(&online_vs);
    online_vs.start.players[0].slot_type = Gm_PKind_Human;
    online_vs.start.players[1].slot_type = Gm_PKind_Human;
    for (int i = 2; i < GM_MAX_PLAYERS; ++i)
        online_vs.start.players[i].slot_type = Gm_PKind_NA;
#ifdef TARGET_PC
    if (online_kind == ONLINE_KIND_TEAM_SELECT) {
        teamSelectPrefill();
    }
    if (after_match) {
        am_live = true;
        am_cursor[0] = am_cursor[1] = 0;
        am_pick[0] = am_pick[1] = -1;
        am_outcome = AM_OUT_NONE;
    }
#endif
}

#ifdef TARGET_PC
static bool rankedMode(void) { return online_kind == ONLINE_KIND_RANKED; }

static void rankedRules(StartMeleeData* start, UNUSED StartMeleeData* previous)
{
    if (!rankedMode() || !pc_rank_session_active()) return;
    start->rules.match_kind = MatchKind_Stock;
    start->rules.is_stock = true;
    start->rules.is_teams = false;
    start->rules.timer_enabled = true;
    start->rules.timer_counts_up = false;
    start->rules.time_limit = pc_rank_session_seconds();
    start->rules.disable_pausing = true;
    start->rules.item_freq = -1;
    start->rules.x20 = 0;
    start->rules.x30 = 1.0f;
    start->rules.game_speed = 1.0f;
    start->rules.stkind = pc_rank_session_stage();
}

static void rankedPlayer(PlayerInitData* start, PlayerInitData* previous)
{
    if (!rankedMode() || !pc_rank_session_active()) return;
    start->stocks = pc_rank_session_stocks();
    start->handicap = 5;
    start->attack_ratio = start->defense_ratio = start->model_scale = 1.0f;
    start->damage = start->damage1 = 0;
    start->vs_metal = start->vs_invisible = false;
    if (previous == &online_vs.start.players[0] || previous == &online_vs.start.players[1])
        start->slot_type = Gm_PKind_Human;
    else
        start->slot_type = Gm_PKind_NA;
}
#endif

void onEnterCss(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter CSS at frame %d", pc_net_frame());
#endif
    team_select_css = online_kind == ONLINE_KIND_TEAM_SELECT;
    gmVsMelee_EnterCss(state, &online_vs, VS_MELEE);
}

void onExitCss(GameModeState* state)
{
#ifdef TARGET_PC
    if (team_select_css) {
        CSSData* css = gm_GetGameModeStateExitData(state);
        bool search = team_select_then_search;
        bool rematch = team_select_then_rematch;
        team_select_css = false;
        team_select_then_search = false;
        team_select_then_rematch = false;
        if (css->pending_scene_change == CSSPendingSceneChange_2) {
            gm_ChangeGameModeAfterCurrentScene(GM_MENU); /* backed out */
            return;
        }
        teamSelectSave(css->vs.start.players);
        if (rematch) {
            /* Change team + rematch: back to the same opponent. */
            online_kind = ONLINE_KIND_DIRECT;
            rematch_direct = true;
            gm_SetNextGameModeStateId(state_lobby);
        } else if (search) {
            /* MATCHMAKING with no saved team: now search with it. */
            online_kind = ONLINE_KIND_UNRANKED;
            gm_SetNextGameModeStateId(state_lobby);
        } else {
            gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        }
        return;
    }
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    CSSData* css = gm_GetGameModeStateExitData(state);
    if (css->pending_scene_change == CSSPendingSceneChange_2) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#ifdef TARGET_PC
    if (rankedMode() && !pc_rank_session_active()) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitCss(state, &online_vs);
}

void onEnterSss(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter SSS at frame %d", pc_net_frame());
#endif
    gmVsMelee_EnterSss(state, &online_vs);
#ifdef TARGET_PC
    if (rankedMode() && pc_rank_session_active()) {
        SSSData* sss = gm_GetGameModeStateEnterData(state);
        pc_rank_session_stage_begin();
        sss->force_stage_id = pc_rank_session_stage() ? (int) pc_rank_session_stage() : -1;
        sss->no_lras = true;
    }
#endif
}

void onExitSss(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitSss(state, &online_vs, state_css);
#ifdef TARGET_PC
    if (rankedMode() && !((SSSData*) gm_GetGameModeStateExitData(state))->start_game) {
        pc_rank_session_abort("ranked stage selection cancelled");
        gm_SetNextGameModeStateId(state_lobby);
    }
#endif
}

void onEnterVs(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter VS at frame %d", pc_net_frame());
#endif
#ifdef TARGET_PC
    gmVsMelee_EnterVs(state, &online_vs, rankedRules, rankedPlayer);
#else
    gmVsMelee_EnterVs(state, &online_vs, NULL, NULL);
#endif
}

#ifdef TARGET_PC
static void matchmadeFinish(MatchEnd* end);
#endif

void onExitVs(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        // Tag Fighter: same cleanup as the normal-end path below, needed
        // here too -- a mid-match disconnect skips straight to state_lobby
        // and never reaches it otherwise, leaving a human+CPU control-role
        // swap and stale point/assist GObj pointers in place exactly like
        // gmvsmode.c's onExitVs already guards against for the offline/
        // LAN-clean-end paths (see its own comments for both hazards).
        TagAssist_RevertControlRolesForMatchEnd();
        TagAssist_OnReset();
        if (rankedMode()) {
            pc_rank_session_abort("peer disconnected");
        }
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    MatchExitInfo* mei;
    ssize_t i;

    // Tag Fighter: this GM_ONLINE state table has its own onExitVs, separate
    // from gmvsmode.c's -- online Tag Battle matches never went through that
    // one, so they never got this cleanup, and TagAssist_Tick's unconditional
    // per-frame TagAssist_CheckPointElimination call (docs comment on
    // TagAssist_Tick) then dereferenced a stale/NULL team->point on the very
    // next tick after the RESULTS scene entered. Confirmed root cause of a
    // 100%-reproducible crash right after a real online Tag Battle match
    // ended naturally (crash log: fault in TagAssist_Tick, called from the
    // ordinary per-frame loop, one tick after "scene 2 -> 5"; rollbacks 0
    // the whole match ruled out a rollback-resimulation cause).
    TagAssist_RevertControlRolesForMatchEnd();

    gmVsMelee_ExitVs(state, state_results, state_sudden_death);
    mei = gm_GetGameModeStateExitData(state);
#ifdef TARGET_PC
    if (rankedMode()) {
        MatchEnd* end = &mei->match_end;
        if (gm_WasMatchCanceled(end->outcome) || end->is_teams ||
            end->player_standings[0].pkind != Gm_PKind_Human ||
            end->player_standings[1].pkind != Gm_PKind_Human) {
            pc_rank_session_abort("ranked game cancelled or invalid");
        } else {
            unsigned winner = end->n_winners == 1 ? end->winners[0] : PC_RANK_TIE;
            int a = end->player_standings[0].stocks;
            int b = end->player_standings[1].stocks;
            pc_rank_session_game(winner, a < 0 ? 0 : a, b < 0 ? 0 : b,
                                 gmVsMelee_StartData.rules.stkind, end->frame_count);
        }
        /* Ties return through CSS to a fresh one-stock, three-minute game;
         * vanilla sudden death would begin at 300 percent. */
        gm_SetNextGameModeStateId(state_results);
    }
#endif
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        // Tag Fighter: same patch gmvsmode.c's onExitVs applies -- a tag
        // mid-match leaves this snapshot pointing at whichever port ended up
        // playing point, not the port CSS originally assigned, so stats
        // below would attribute to the wrong slot without this.
        Gm_PKind original_pkind = TagAssist_GetOriginalPkindForMatchEnd((int) i);
        if (original_pkind != Gm_PKind_NA) {
            mei->match_end.player_standings[i].pkind = original_pkind;
        }
        if (mei->match_end.player_standings[i].pkind != Gm_PKind_NA) {
            gm_80162A98(mei->match_end.player_standings[i].x20);
            gm_RecordSelfDestructs(
                mei->match_end.player_standings[i].self_destructs);
            gm_80162A4C(mei->match_end.player_standings[i].x44);
        }
    }
    TagAssist_OnReset();
#ifdef TARGET_PC
    /* Matchmaking skips RESULTS: the after-match lobby says who won and
     * asks what next. A tie still goes through sudden death first. */
    if (pc_net_matchmade() && !gm_MatchHasMultipleWinners(&mei->match_end)) {
        matchmadeFinish(&mei->match_end);
    }
#endif
}

void onEnterSuddenDeath(GameModeState* state)
{
    gmVsMelee_EnterSuddenDeath(state, &online_vs, NULL, NULL);
}

void onExitSuddenDeath(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitSuddenDeath(state);
#ifdef TARGET_PC
    if (pc_net_matchmade()) {
        matchmadeFinish(&gmVsMelee_VsExitInfo.match_end);
    }
#endif
}

void onEnterResults(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter RESULTS at frame %d", pc_net_frame());
#endif
    gmVsMelee_EnterResults(state);
}

void onExitResults(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitResults(state, &online_vs, state_css);
#ifdef TARGET_PC
    /* Matchmaking has no online CSS to go back to: the lobby asks what next. */
    if (pc_net_matchmade()) {
        after_match = true;
        gm_SetNextGameModeStateId(state_lobby);
    }
    if (rankedMode()) {
        /* The results exit is driven by synchronized pads. Reliable-message
         * arrival and disk speed must never choose different next scenes.
         * Both peers leave a terminal set for the lobby; that scene keeps
         * the connection alive until signing/saving finishes. */
        if (pc_rank_session_set_complete() ||
            gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome))
            gm_SetNextGameModeStateId(state_lobby);
    }
#endif
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}

#ifdef TARGET_PC
/* What leaving RESULTS did for a matchmade game, done straight from VS (or
 * sudden death): the records the results exit keeps, then the lobby. Both
 * machines run it on the same synced frame, so they agree on the scene. */
static void matchmadeFinish(MatchEnd* end)
{
    GameModeState* results = gm_Mode_Online_States;
    while (results->id != state_results) {
        results++;
    }
    /* The results entry is last in the table, which is what keeps
     * gmVsMelee_ExitResults from starting a "new challenger" scene. */
    gmVsMelee_ExitResults(results, &online_vs, state_css);
    if (!gm_WasMatchCanceled(end->outcome)) {
        gm_801623A4(end);
    }
    am_result = gm_WasMatchCanceled(end->outcome) || end->n_team_winners != 1 ?
                    AM_RESULT_NONE :
                end->team_winners[0] == 0 ? AM_RESULT_RED :
                                            AM_RESULT_BLUE;
    pc_log_line("online: matchmade game over at frame %d, %s", pc_net_frame(),
                am_result == AM_RESULT_RED  ? "red team wins" :
                am_result == AM_RESULT_BLUE ? "blue team wins" :
                                              "no winner");
    after_match = true;
    gm_SetNextGameModeStateId(state_lobby);
}
#endif

/* ---- lobby scene ------------------------------------------------------- */
#ifdef TARGET_PC
static char profile_message[ONLINE_LOBBY_MSG_LEN];
static void profileRefresh(void) {
    const char* code = pc_net_match_local_code();
    if (!code || !*code) {
        snprintf(profile_message, sizeof profile_message, "Identity unavailable. Check your profile files.");
        return;
    }
    PcNetRankStoreResult result;
    PcNetRankStore* store = pc_rank_store_open(pc_net_match_profile_directory(),
        pc_net_match_identity()->public_key, &result);
    PcNetRating rating;
    unsigned count = 0;
    if (store && pc_rank_store_current(store, &rating, NULL, &count)) {
        double score = pc_rank_display(&rating);
        const char* tier = count < 5 ? "Placement" : score < 1050 ? "Bronze" :
            score < 1200 ? "Silver" : score < 1350 ? "Gold" : score < 1500 ? "Platinum" :
            score < 1650 ? "Diamond" : "Master";
        snprintf(profile_message, sizeof profile_message, "%s %.0f - %u sets. Community rating, unverified. B: back", tier, score, count);
    } else snprintf(profile_message, sizeof profile_message, "Rating history unavailable or damaged. B: back");
    pc_rank_store_close(store);
}
static bool internetLobby(void) {
    return online_kind != ONLINE_KIND_LAN && online_kind != ONLINE_KIND_PROFILE &&
           online_kind != ONLINE_KIND_TEAM_SELECT &&
           !(online_kind == ONLINE_KIND_DIRECT && getenv("MELEE_LAN_DIRECT"));
}

/* MeleeVS Matchmaking (online_kind UNRANKED with Tag Battle on) skips the
 * online CSS and SSS: each machine brings a team fixed before searching,
 * and the match starts straight from the two teams on a random legal stage.
 * Direct Connect and LAN keep the regular MeleeVS CSS and stage select.
 * The team is the one TEAM SELECT saved; without one, the last offline
 * MeleeVS CSS's picks (port 1 and its teammate). The handshake falls back
 * to Fox/Falco for a missing pick. */
static bool matchmadeMode(void)
{
    return online_kind == ONLINE_KIND_UNRANKED && TagAssist_IsTagBattleOn();
}

static PcNetTeam matchmadeLocalTeam(void)
{
    const PlayerInitData* p = gmVsMelee_GetVsData()->start.players;
    PcNetTeam team;
    int mate = -1;
    if (loadSavedTeam(&team)) {
        return team;
    }
    memset(&team, 0, sizeof team);
    for (int i = 1; i < 4; i++) {
        if (p[i].slot_type != Gm_PKind_NA && p[i].team == p[0].team) {
            mate = i;
            break;
        }
    }
    team.fighter[0].ckind = p[0].ckind;
    team.fighter[0].color = p[0].color;
    team.fighter[1].ckind = mate >= 0 ? p[mate].ckind : -1;
    team.fighter[1].color = mate >= 0 ? p[mate].color : 0;
    team.fighter[1].cpu_level = mate >= 0 ? p[mate].cpu_level : 9;
    team.point = mate >= 0 && TagAssist_IsPortPoint(mate) ? 1 : 0;
    return team;
}

/* Character names for the lobby's team lines, in CharacterKind order. */
static const char* const ckind_name[CKind_Playable_Count] = {
    "Captain Falcon", "Donkey Kong", "Fox",        "Mr. Game & Watch", "Kirby",
    "Bowser",         "Link",        "Luigi",      "Mario",            "Marth",
    "Mewtwo",         "Ness",        "Peach",      "Pikachu",          "Ice Climbers",
    "Jigglypuff",     "Samus",       "Yoshi",      "Zelda",            "Sheik",
    "Falco",          "Young Link",  "Dr. Mario",  "Roy",              "Pichu",
    "Ganondorf",
};

/* "Your team: Fox (point) + Falco CPU": both fighters, which starts on
 * point, and whether the partner is a CPU or a second player on the same
 * machine ("(Couch)", a couch duo). */
static void teamLine(char* out, size_t size, const char* label, const PcNetTeam* team)
{
    const char* name[2];
    for (int i = 0; i < 2; i++) {
        int ck = team->fighter[i].ckind;
        name[i] = ck >= 0 && ck < CKind_Playable_Count ? ckind_name[ck] : NULL;
    }
    if (name[0] == NULL) {
        out[0] = '\0';
        return;
    }
    if (name[1] == NULL) {
        snprintf(out, size, "%s: %s", label, name[0]);
        return;
    }
    snprintf(out, size, "%s: %s%s + %s%s%s", label, name[0], team->point == 0 ? " (point)" : "",
             name[1], team->point == 1 ? " (point)" : "",
             team->fighter[1].human ? " (Couch)" : " CPU");
}

/* Every search starts here, so the netcode always knows whether this side
 * is joining a Matchmaking game (and with which team) before it connects. */
static void startMatch(enum PcNetMatchMode mode, const char* code)
{
    if (mode == PC_MATCH_UNRANKED && matchmadeMode()) {
        PcNetTeam team = matchmadeLocalTeam();
        pc_net_set_matchmade(true, &team);
    } else {
        pc_net_set_matchmade(false, NULL);
    }
    pc_net_match_start(mode, code);
}

/* Internal stage ids of the Melee competitive legal stages, the same list
 * Ranked strikes from (net_rank_session.c): Fountain of Dreams, Pokemon
 * Stadium, Yoshi's Story, Dream Land, Battlefield, Final Destination. */
static const u8 matchmade_stages[] = { 2, 3, 8, 28, 31, 32 };

/* Build the whole match from the two handshake teams, identically on both
 * machines: ports by pc_net_game_port (player 0, the host, on 1+2 in red;
 * player 1 on 3+4 in blue), point from each team's choice, each fighter in
 * its team color's costume the way a vanilla team battle dresses it, and a
 * stage picked from the shared seed. */
static void matchmadeBuild(u32 seed)
{
    StartMeleeData* start = &online_vs.start;
    start->rules.is_teams = 1;
    for (int i = 0; i < GM_MAX_PLAYERS; i++) {
        start->players[i].slot_type = Gm_PKind_NA;
    }
    for (int m = 0; m < 2; m++) {
        const PcNetTeam* team = pc_net_team(m);
        for (int slot = 0; slot < 2; slot++) {
            const PcNetTeamFighter* f = &team->fighter[slot];
            int port = pc_net_game_port(m, slot);
            PlayerInitData* p = &start->players[port];
            p->ckind = f->ckind;
            p->color = m == 0 ? gm_80169264((u8) f->ckind) : gm_801692BC((u8) f->ckind);
            p->sub_color = 0;
            p->slot_type = f->human ? Gm_PKind_Human : Gm_PKind_Cpu;
            if (!f->human) {
                p->cpu_level = f->cpu_level;
            }
            p->team = (u8) m;
            TagAssist_CssSyncPortTeam(port, (u8) m);
        }
        TagAssist_SetExplicitPoint((u8) m, pc_net_game_port(m, team->point));
    }
    {
        u32 h = seed * 2654435761u;
        start->rules.stkind = matchmade_stages[(h >> 16) % ARRAY_SIZE(matchmade_stages)];
    }
    /* What gmVsMelee_ExitCss / ExitSss would have queued: the fighters'
     * and the stage's sound banks. */
    {
        u64 mask = 0;
        for (int i = 0; i < GM_MAX_PLAYERS; i++) {
            mask |= lbAudioAx_80026E84(start->players[i].ckind);
        }
        lbAudioAx_80026F2C(20);
        lbAudioAx_8002702C(4, mask);
        lbAudioAx_80027168();
        lbAudioAx_80026F2C(24);
        lbAudioAx_8002702C(8, lbAudioAx_80026EBC(start->rules.stkind));
        lbAudioAx_80027168();
    }
    /* And the preload cache the CSS fills every frame (fighters, costumes)
     * and the SSS commits on exit (stage). Without it the VS scene loads
     * against the lobby's stale cache and dies before its first frame. */
    {
        PreloadedGameModeState* cache;
        lbDvd_SetupVsPreloadCache();
        cache = lbDvd_GetPreloadCacheScene();
        for (int i = 0; i < GM_MAX_PLAYERS; i++) {
            const PlayerInitData* p = &start->players[i];
            bool used = p->slot_type == Gm_PKind_Human || p->slot_type == Gm_PKind_Cpu;
            cache->game_cache.entries[i].char_id = used ? p->ckind : ChKind_None;
            cache->game_cache.entries[i].color = used ? p->color : 0;
        }
        cache->game_cache.stkind = start->rules.stkind;
        lbDvd_80018254();
    }
    pc_log_line("online: matchmade match on stage %d: P1 %d/%d P2 %d/%d P3 %d/%d P4 %d/%d",
                start->rules.stkind, start->players[0].ckind, start->players[0].color,
                start->players[1].ckind, start->players[1].color, start->players[2].ckind,
                start->players[2].color, start->players[3].ckind, start->players[3].color);
}

/* Direct Connect code entry. The friend's code is the one piece of online
 * state the player has to type, and the F1 field is only editable before the
 * lobby consumes it, so the lobby edits it in place. The page opens on the
 * saved code with START ready to connect; X starts editing, where stick or
 * D-pad left/right picks a slot (it blinks), up/down cycles the character,
 * and X or B finish. An empty code hosts our own code, which is what a
 * friend types.
 * ponytail: 17 fixed slots instead of a keyboard; codes are 17 chars max. */
#define DIRECT_CODE_SLOTS 17
static const char direct_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#";
static char direct_entry[DIRECT_CODE_SLOTS + 1];
static int direct_cursor;
static bool direct_editing;   /* on the code page */
static bool direct_code_edit; /* X pressed: the D-pad changes the code */
static int direct_blink;
static char direct_error[ONLINE_LOBBY_MSG_LEN];

/* Reconnect to the last opponent as a Matchmaking (fixed teams, random
 * stage) Direct session: the original host hosts its code, the other side
 * dials it, and the last peer endpoint is tried at once. */
static void startRematch(void)
{
    PcNetTeam team = matchmadeLocalTeam();
    pc_net_set_matchmade(true, &team);
    pc_net_match_rematch_hint();
    pc_log_line("lobby: rematch, %s %s", rematch_host ? "hosting" : "dialing", rematch_code);
    pc_net_match_start(PC_MATCH_DIRECT, rematch_host ? NULL : rematch_code);
}

/* TEAM SELECT from the lobby, then a search or a rematch. */
static void afterMatchTeamSelect(bool then_rematch)
{
    online_kind = ONLINE_KIND_TEAM_SELECT;
    team_select_then_search = !then_rematch;
    team_select_then_rematch = then_rematch;
    TagAssist_EnterForcedOn();
    gm_InitVsMode(&online_vs);
    for (int i = 0; i < GM_MAX_PLAYERS; ++i) {
        online_vs.start.players[i].slot_type = Gm_PKind_NA;
    }
    teamSelectPrefill();
    gm_SetNextGameModeStateId(state_css);
    gm_801A4B60();
}

/* What this side does once the session is over: a rematch pick that got no
 * rematch searches with its own choice's team, as the matchmake picks do. */
static void afterMatchFollow(int pick)
{
    after_match = false;
    switch (pick) {
    case AM_SAME_REMATCH:
    case AM_SAME_SEARCH:
        online_kind = ONLINE_KIND_UNRANKED;
        startMatch(PC_MATCH_UNRANKED, NULL);
        break;
    case AM_CHANGE_REMATCH:
    case AM_CHANGE_SEARCH:
        afterMatchTeamSelect(false);
        break;
    default:
        pc_net_match_stop();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        break;
    }
}

/* End the session our own way: our disconnect marks the peer as gone, and
 * GM_ONLINE ends any non-lobby scene while it does (gmscene.c), which would
 * close TEAM SELECT on its first frame. */
static void afterMatchEndSession(void)
{
    am_live = false;
    pc_net_match_idle();
    pc_net_peer_status_clear();
}

static void afterMatchExecute(int me)
{
    int mine = am_pick[me];
    int outcome = am_outcome;
    am_outcome = AM_OUT_NONE;
    pc_log_line("after match: carrying out %s (our pick %d)",
                outcome == AM_OUT_LEAVE ? "no rematch" : "rematch with a team change", mine);
    afterMatchEndSession();
    if (outcome == AM_OUT_LEAVE) {
        if (mine >= 0) {
            afterMatchFollow(mine);
        } else {
            am_cursor[me] = AM_SAME_SEARCH;
        }
        return;
    }
    after_match = false;
    if (mine == AM_CHANGE_REMATCH) {
        afterMatchTeamSelect(true);
    } else {
        online_kind = ONLINE_KIND_DIRECT;
        rematch_direct = true;
        startRematch();
    }
}

static void afterMatchFrame(OnlineLobbyView* view)
{
    static const char* const label[4] = { "SAME TEAM - REMATCH", "CHANGE TEAM - REMATCH",
                                          "SAME TEAM - MATCHMAKE",
                                          "CHANGE TEAM - MATCHMAKE" };
    int me = pc_net_match_is_host() ? 0 : 1;
    int them = 1 - me;

    /* Our team is the host's (red, team 0) or the guest's (blue, team 1). */
    view->title = am_result == AM_RESULT_NONE ? "MATCH OVER" :
                  am_result == me             ? "You won!" :
                                                "You lost.";
    view->phase = LOBBY_PHASE_FOUND;
    view->menu_count = 4;
    for (int i = 0; i < 4; i++) {
        view->menu[i] = label[i];
    }

    if (am_outcome != AM_OUT_NONE) {
        /* Decided: wait out the hold (or the peer's goodbye), then go. */
        view->phase = LOBBY_PHASE_CONNECTING;
        for (int i = 0; i < 4; i++) {
            view->menu_tag[i] = am_pick[me] == i ? "YOU" : am_pick[them] == i ? "OPPONENT" : "";
        }
        view->menu_cursor = am_pick[me];
        snprintf(view->message, sizeof view->message, "%s",
                 am_outcome == AM_OUT_LEAVE ? "No rematch" : "Rematch with a new team...");
        if (pc_net_frame() >= am_leave_frame || !pc_net_active() ||
            pc_net_peer_status() != PC_NET_PEER_OK) {
            afterMatchExecute(me);
        }
        return;
    }

    if (am_live && (!pc_net_active() || pc_net_peer_status() != PC_NET_PEER_OK)) {
        /* The opponent quit on its own (not a synced pick). */
        int mine = am_pick[me];
        pc_log_line("after match: opponent left (our pick %d)", mine);
        afterMatchEndSession();
        if (mine >= 0) {
            afterMatchFollow(mine);
            return;
        }
        am_cursor[me] = AM_SAME_SEARCH;
    }

    if (!am_live) {
        /* Only this side is left: the two matchmake options, or B. */
        u64 rep = gm_801A36C0(PAD_MAX_CONTROLLERS);
        u64 trg = gm_GetButtonsTriggered(PAD_MAX_CONTROLLERS);
        view->menu_tag[AM_SAME_REMATCH] = view->menu_tag[AM_CHANGE_REMATCH] = "-";
        view->menu_cursor = am_cursor[me];
        view->phase = LOBBY_PHASE_ERROR;
        snprintf(view->message, sizeof view->message, "Opponent left - no rematch");
        view->hint = "D-PAD: choose    A: confirm    B: menu";
        if (rep & (PAD_ANY_UP | PAD_ANY_DOWN)) {
            am_cursor[me] = am_cursor[me] == AM_SAME_SEARCH ? AM_CHANGE_SEARCH : AM_SAME_SEARCH;
            sfxMove();
        } else if (trg & HSD_PAD_A) {
            sfxForward();
            afterMatchFollow(am_cursor[me]);
        } else if (trg & HSD_PAD_B) {
            sfxBack();
            afterMatchFollow(AM_MENU);
        }
        return;
    }

    /* Both players' picks from the synced pads: same on both machines. */
    for (int m = 0; m < 2; m++) {
        u8 port = (u8) pc_net_game_port(m, 0);
        u64 rep = gm_801A36C0(port);
        u64 trg = gm_GetButtonsTriggered(port);
        int before = am_pick[m];
        if (am_pick[m] < 0) {
            if (rep & PAD_ANY_UP) {
                am_cursor[m] = (am_cursor[m] + 3) % 4;
                if (m == me) sfxMove();
            } else if (rep & PAD_ANY_DOWN) {
                am_cursor[m] = (am_cursor[m] + 1) % 4;
                if (m == me) sfxMove();
            } else if (trg & HSD_PAD_A) {
                am_pick[m] = am_cursor[m];
            } else if (trg & HSD_PAD_B) {
                am_pick[m] = AM_MENU;
            }
        } else if (trg & HSD_PAD_B) {
            am_pick[m] = -1;
        }
        if (am_pick[m] != before) {
            pc_log_line("after match: %s picks %d", m == 0 ? "host" : "guest", am_pick[m]);
            if (m == me) {
                if (am_pick[m] >= 0 && am_pick[m] != AM_MENU) sfxForward();
                else sfxBack();
            }
        }
    }

    for (int i = 0; i < 4; i++) {
        bool mine = am_pick[me] == i, theirs = am_pick[them] == i;
        view->menu_tag[i] = mine && theirs ? "BOTH" : mine ? "YOU" : theirs ? "OPPONENT" : "";
    }
    view->menu_cursor = am_pick[me] >= 0 ? am_pick[me] : am_cursor[me];
    snprintf(view->message, sizeof view->message, "%s",
             am_pick[me] < 0 ? "Rematch only if both players pick a rematch" :
             am_pick[them] < 0 ? "Waiting for your opponent..." : "");
    view->hint = am_pick[me] < 0 ? "D-PAD: choose    A: confirm    B: menu" :
                                   "B: change your pick";

    int a = am_pick[0], b = am_pick[1];
    if (a >= AM_SAME_SEARCH || b >= AM_SAME_SEARCH) {
        /* Someone leaves: no rematch. Both sides end the session on this
         * frame; each goes on with its own pick. */
        pc_log_line("after match: no rematch (host %d, guest %d)", a, b);
        am_outcome = AM_OUT_LEAVE;
        am_leave_frame = pc_net_frame() + AM_HOLD_FRAMES;
        return;
    }
    if (a < 0 || b < 0) {
        return;
    }
    if (a == AM_SAME_REMATCH && b == AM_SAME_REMATCH) {
        /* Same teams, same session: straight back in on a new stage. */
        u32 seed = pc_net_match_seed() + ++am_game * 0x9E3779B9u;
        pc_log_line("after match: rematch %u in session, seed %u", am_game, seed);
        after_match = false;
        *HSD_RandSeedPtr = seed;
        matchmadeBuild(seed);
        gm_SetNextGameModeStateId(state_vs);
        gm_801A4B60();
        return;
    }
    /* A team changes: end the session, change it offline, reconnect. */
    rematch_host = me == 0;
    snprintf(rematch_code, sizeof rematch_code, "%s",
             rematch_host ? pc_net_match_local_code() : pc_net_match_opponent_code());
    pc_log_line("after match: rematch with a team change (host %d, guest %d)", a, b);
    am_outcome = AM_OUT_TEAM_REMATCH;
    am_leave_frame = pc_net_frame() + AM_HOLD_FRAMES;
}

static void directEntryBegin(void)
{
    snprintf(direct_entry, sizeof direct_entry, "%s", pc_get_net_target());
    direct_cursor = (int) strlen(direct_entry);
    if (direct_cursor >= DIRECT_CODE_SLOTS) {
        direct_cursor = DIRECT_CODE_SLOTS - 1;
    }
    direct_editing = true;
    direct_code_edit = false;
    direct_error[0] = '\0';
    pc_log_line("lobby: direct connect code entry, prefilled '%s'", direct_entry);
}

/* Slots past the first blank stay blank, so the code is always contiguous. */
static void directEntrySet(int slot, char c)
{
    size_t len = strlen(direct_entry);
    if (c == '\0') {
        memset(direct_entry + slot, 0, sizeof direct_entry - (size_t) slot);
        return;
    }
    while (len < (size_t) slot) {
        direct_entry[len++] = direct_alphabet[0];
    }
    direct_entry[slot] = c;
    if ((size_t) slot >= len) {
        direct_entry[slot + 1] = '\0';
    }
}

static void directEntryCycle(int step)
{
    /* The wheel is the alphabet plus one blank, so a slot can be cleared. */
    const int n = (int) sizeof direct_alphabet; /* includes the blank */
    char current = direct_entry[direct_cursor];
    const char* at = current ? strchr(direct_alphabet, current) : NULL;
    int index = at ? (int) (at - direct_alphabet) : n - 1;
    index = (index + step + n) % n;
    directEntrySet(direct_cursor, index == n - 1 ? '\0' : direct_alphabet[index]);
}
#endif

void gm_Scene_OnlineLobby_OnEnter(UNUSED void* unused)
{
    mnOnlineLobby_Create();
#ifdef TARGET_PC
    direct_editing = false;
    if (online_kind == ONLINE_KIND_TEAM_SELECT) {
        /* offline: the first frame goes straight on to the CSS */
    } else if (online_kind == ONLINE_KIND_PROFILE) {
        profileRefresh();
    } else if (after_match) {
        /* the after-match choice runs in OnFrame */
    } else if (internetLobby() && online_kind == ONLINE_KIND_DIRECT) {
        /* Ask for the code first: starting on a stale launcher pref is how
         * two players both ended up hosting their own codes forever. */
        if (rematch_direct) {
            startRematch();
        } else {
            directEntryBegin();
        }
    } else if (internetLobby() && !awaiting_rank_result &&
               (online_kind != ONLINE_KIND_RANKED ||
                                  pc_net_match_publication(NULL) == 0)) {
        if (pc_net_peer_status() == PC_NET_PEER_OK) {
            startMatch(online_kind == ONLINE_KIND_UNRANKED ? PC_MATCH_UNRANKED :
                               PC_MATCH_RANKED, NULL);
        }
    } else if (!internetLobby()) {
        pc_net_match_stop(); /* free the port the online menu's DHT node holds */
        pc_net_set_matchmade(false, NULL); /* LAN keeps the regular MeleeVS CSS */
        pc_lan_start();
    }
#endif
}

void gm_Scene_OnlineLobby_OnExit(UNUSED void* unused)
{
    mnOnlineLobby_Destroy();
}

#ifdef TARGET_PC
static const char* const peer_word[] = { "", "Peer left",
                                         "Connection timed out", "Desync",
                                         "Incompatible version",
                                         "Could not resume" };

static void lobbyCopyName(char* dst, const char* src)
{
    snprintf(dst, ONLINE_LOBBY_NAME_LEN, "%s", src);
}

/* Peers on our protocol and build: the only ones counted or started with. */
static int lobbyCompatible(const PcLanPeer* peers, int n)
{
    int i, nc = 0;
    for (i = 0; i < n; i++) {
        nc += peers[i].compatible;
    }
    return nc;
}

/* Fill the view from the LAN state; logs the status line when it changes. */
static void lobbyFillView(OnlineLobbyView* view, int state, const char* why,
                          const PcLanPeer* peers, int n)
{
    /* Indexed by pc_net_quality() 0..3 and pc_net_peer_status() 0..5;
     * anything outside stays blank. */
    static const char* const link_word[] = { "stable", "warning", "stalling",
                                             "reconnecting" };
    static char last_status[ONLINE_LOBBY_MSG_LEN];
    char status[ONLINE_LOBBY_MSG_LEN];
    bool connected = state == 1 || state == 2;
    bool host = connected && pc_lan_is_host();
    int ping = -1, delay;
    unsigned rollbacks;
    int quality, reason;
    int nc = lobbyCompatible(peers, n);
    int i;

    memset(view, 0, sizeof *view);
    view->title =
        online_kind == ONLINE_KIND_DIRECT ? "DIRECT CONNECT" : "LAN PLAY";
    if (connected && !pc_net_stats(&ping, &delay, &rollbacks)) {
        ping = -1;
    }
    quality = connected ? pc_net_quality() : -1;
    if (quality >= 0 && quality < (int) ARRAY_SIZE(link_word)) {
        view->link = link_word[quality];
    }

    lobbyCopyName(view->players[0].name, pc_lan_local_name());
    view->players[0].ping_ms = -1;
    view->players[0].is_host = host;
    view->players[0].is_local = true;
    view->player_count = 1;
    for (i = 0; i < n && view->player_count < ONLINE_LOBBY_MAX_PLAYERS; i++) {
        OnlineLobbyPlayer* p = &view->players[view->player_count++];
        lobbyCopyName(p->name, peers[i].name);
        p->is_host = peers[i].host;
        p->incompatible = !peers[i].compatible;
        /* The session peer: the host we joined, or our first peer as host. */
        p->ping_ms = connected && (peers[i].host || (host && i == 0)) ? ping : -1;
    }

    switch (state) {
    case 0:
        view->phase = nc == 0 ? LOBBY_PHASE_SEARCHING : LOBBY_PHASE_FOUND;
        if (nc == 0) {
            snprintf(status, sizeof status, "LAN: searching...%s",
                     pc_lan_full() ? " - Lobby full" : "");
        } else {
            snprintf(status, sizeof status, "%d players found - press START%s",
                     nc + 1, pc_lan_full() ? " - Lobby full" : "");
        }
        break;
    case 1:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Connecting...");
        break;
    case 4:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Ready - waiting for host...");
        break;
    case 2:
        view->phase = LOBBY_PHASE_STARTING;
        view->countdown_frames = pc_lan_start_frame() - pc_net_frame();
        if (view->countdown_frames < 0) {
            view->countdown_frames = 0;
        }
        snprintf(status, sizeof status, "Starting...");
        break;
    default:
        view->phase = LOBBY_PHASE_ERROR;
        reason = pc_net_peer_status();
        if (reason <= 0 || reason >= (int) ARRAY_SIZE(peer_word)) {
            reason = 0;
        }
        /* Still in the lobby: Start elects again, and a peer's proposal is
         * still joined (net_lan.c), so leaving to retry is never needed. */
        snprintf(status, sizeof status, "Failed: %s%s%s%s",
                 why != NULL ? why : "unknown error", reason ? " - " : "",
                 peer_word[reason], nc ? " - START: retry" : "");
        break;
    }
    memcpy(view->message, status, sizeof view->message);
    if (strcmp(status, last_status) != 0) {
        memcpy(last_status, status, sizeof last_status);
        pc_log_line("lobby: %s", status);
    }
}
#endif

void gm_Scene_OnlineLobby_OnFrame(void)
{
#ifdef TARGET_PC
    PcLanPeer peers[PC_LAN_MAX_PEERS];
    OnlineLobbyView view;
    const char* why = NULL;
    int state;
    int n;
    u64 input = gm_GetButtonsTriggered(PAD_MAX_CONTROLLERS);
    bool keep_lobby = false; /* B was used on this page, not to leave it */

    if (online_kind == ONLINE_KIND_TEAM_SELECT) {
        gm_801A4B60(); /* on to the CSS (state_css) */
        return;
    }
    if (online_kind == ONLINE_KIND_PROFILE || internetLobby()) {
        bool choosing = after_match;
        memset(&view, 0, sizeof view);
        view.title = online_kind == ONLINE_KIND_PROFILE ? "PROFILE" :
                     online_kind == ONLINE_KIND_UNRANKED ?
                         (TagAssist_IsTagBattleOn() ? "MATCHMAKING" : "UNRANKED") :
                     online_kind == ONLINE_KIND_RANKED ? "RANKED" :
                     rematch_direct ? "REMATCH" : "DIRECT CONNECT";
        view.player_count = 1;
        view.players[0].is_local = true;
        view.players[0].ping_ms = -1;
        lobbyCopyName(view.players[0].name, pc_net_match_local_code());
        if (online_kind == ONLINE_KIND_PROFILE) {
            view.phase = LOBBY_PHASE_FOUND;
            snprintf(view.message, sizeof view.message, "%s", profile_message);
        } else if (choosing) {
            afterMatchFrame(&view);
        } else if (direct_editing) {
            u64 repeat = gm_801A36C0(PAD_MAX_CONTROLLERS);
            /* Bootstrap while the code is being typed, not after START, and
             * start publishing our direct record. */
            pc_net_match_prepublish();
            bool edited = false;
            if (!direct_code_edit) {
                if (input & HSD_PAD_X) {
                    direct_code_edit = true;
                    direct_blink = 0;
                    direct_cursor = (int) strlen(direct_entry);
                    if (direct_cursor >= DIRECT_CODE_SLOTS) {
                        direct_cursor = DIRECT_CODE_SLOTS - 1;
                    }
                    sfxForward();
                }
            } else if (input & (HSD_PAD_X | HSD_PAD_B | PAD_CANCEL)) {
                /* Done editing; B here must not also leave the page. */
                direct_code_edit = false;
                keep_lobby = true;
                sfxForward();
            } else if (repeat & PAD_ANY_LEFT) {
                /* Stop at the ends, and at most one slot past the last
                 * character, so the cursor never sits out in blank space. */
                if (direct_cursor > 0) {
                    direct_cursor--;
                }
                edited = true;
            } else if (repeat & PAD_ANY_RIGHT) {
                if (direct_cursor < (int) strlen(direct_entry) &&
                    direct_cursor < DIRECT_CODE_SLOTS - 1) {
                    direct_cursor++;
                }
                edited = true;
            } else if (repeat & PAD_ANY_UP) {
                directEntryCycle(1);
                edited = true;
            } else if (repeat & PAD_ANY_DOWN) {
                directEntryCycle(-1);
                edited = true;
            }
            if (edited) {
                sfxMove();
                direct_blink = 0; /* show the slot at once after a move */
                direct_error[0] = '\0'; /* the rejected code is being changed */
            }
            view.phase = LOBBY_PHASE_FOUND;
            if (direct_error[0]) {
                snprintf(view.message, sizeof view.message, "%s", direct_error);
            } else if (direct_code_edit) {
                /* The selected slot blinks: its character (or a blank past
                 * the end) alternates with '_'. */
                char shown[DIRECT_CODE_SLOTS + 1];
                int len = (int) strlen(direct_entry);
                snprintf(shown, sizeof shown, "%s", direct_entry);
                if (direct_cursor >= len) {
                    memset(shown + len, ' ', (size_t) (direct_cursor - len + 1));
                    shown[direct_cursor + 1] = '\0';
                }
                if ((direct_blink++ / 20) % 2 == 0) {
                    shown[direct_cursor] = '_';
                }
                snprintf(view.message, sizeof view.message, "Friend's code: %s", shown);
            } else {
                snprintf(view.message, sizeof view.message, "Friend's code: %s",
                         direct_entry[0] ? direct_entry : "none");
            }
            view.hint = direct_code_edit ? "D-PAD: move and change    X: done    START: connect" :
                        direct_entry[0]  ? "START: connect    X: edit code    B: back" :
                                           "START: host your code    X: edit code    B: back";
            if (input & HSD_PAD_START) {
                if (direct_entry[0] && !pc_identity_code_valid(direct_entry)) {
                    sfxBack();
                    /* Held until the code changes: a one-frame message is
                     * invisible, and the player needs to know why nothing
                     * happened. */
                    snprintf(direct_error, sizeof direct_error,
                             "%s is not a connect code (NAME#AB2CDE3F)", direct_entry);
                    snprintf(view.message, sizeof view.message, "%s", direct_error);
                    pc_log_line("lobby: direct connect rejected '%s'", direct_entry);
                } else {
                    sfxForward();
                    direct_editing = false;
                    direct_code_edit = false;
                    direct_error[0] = '\0';
                    pc_set_net_target(direct_entry);
                    pc_log_line("lobby: direct connect %s '%s'",
                                direct_entry[0] ? "dialing" : "hosting as",
                                direct_entry[0] ? direct_entry : pc_net_match_local_code());
                    startMatch(PC_MATCH_DIRECT,
                                       direct_entry[0] ? direct_entry : NULL);
                }
            }
        } else if (awaiting_rank_result && pc_net_match_publication(NULL) == 0) {
            pc_net_poll();
            pc_rank_session_poll();
            int result = pc_rank_session_state(&why);
            if (result == PC_RANK_SESSION_SAVED) {
                pc_net_match_publish_rank();
            }
            view.phase = result == PC_RANK_SESSION_FAILED ? LOBBY_PHASE_ERROR : LOBBY_PHASE_CONNECTING;
            snprintf(view.message, sizeof view.message, "%s",
                     why ? why : "Finishing signed result...");
            if (result == PC_RANK_SESSION_FAILED)
                snprintf(view.message, sizeof view.message, "%.54s - START: retry",
                         why ? why : "Set could not be rated");
            if (result == PC_RANK_SESSION_FAILED && (input & HSD_PAD_START)) {
                awaiting_rank_result = false;
                startMatch(PC_MATCH_RANKED, NULL);
            }
        } else if (online_kind == ONLINE_KIND_RANKED && pc_net_match_publication(NULL) != 0) {
            pc_net_match_poll_publication();
            int publication = pc_net_match_publication(&why);
            view.phase = publication < 0 ? LOBBY_PHASE_ERROR :
                         publication == 2 ? LOBBY_PHASE_FOUND : LOBBY_PHASE_CONNECTING;
            snprintf(view.message, sizeof view.message, "%s",
                     publication == 2 ? "Rating saved and published. START: next set" :
                     publication < 0 ? "Rating saved locally. START: retry publication" :
                     "Rating saved. Publishing...");
            if ((input & HSD_PAD_START) && publication != 1) {
                if (publication < 0) pc_net_match_publish_rank();
                else {
                    awaiting_rank_result = false;
                    startMatch(PC_MATCH_RANKED, NULL);
                }
            }
        } else {
            pc_net_match_poll();
            state = pc_net_match_state(&why);
            int reason = pc_net_peer_status();
            view.phase = state == PC_MATCH_READY ? LOBBY_PHASE_STARTING :
                         (state == PC_MATCH_FAIL || reason != PC_NET_PEER_OK) ? LOBBY_PHASE_ERROR :
                         state == PC_MATCH_CONNECT ? LOBBY_PHASE_CONNECTING : LOBBY_PHASE_SEARCHING;
            if (reason != PC_NET_PEER_OK && reason < (int) ARRAY_SIZE(peer_word)) {
                snprintf(view.message, sizeof view.message, "%s - START: search", peer_word[reason]);
            } else if (rematch_direct && state == PC_MATCH_SEARCH) {
                /* The same opponent is coming back, perhaps via TEAM SELECT. */
                snprintf(view.message, sizeof view.message, "Waiting for opponent...");
            } else if (!why && state == PC_MATCH_SEARCH && pc_rdv_nat() == PC_RDV_NAT_STRICT) {
                /* The pairing server saw this router change ports per
                 * destination: say why no match may ever connect. */
                snprintf(view.message, sizeof view.message,
                         "Searching... Strict NAT: matches may fail");
            } else {
                snprintf(view.message, sizeof view.message, "%s", why ? why : "Searching for an opponent...");
            }
            const char* peer = pc_net_match_opponent_code();
            if (peer && peer[0]) {
                view.player_count = 2;
                lobbyCopyName(view.players[1].name, peer);
                view.players[1].ping_ms = -1;
            }
            if (state == PC_MATCH_READY && pc_net_frame() >= pc_net_match_start_frame()) {
                *HSD_RandSeedPtr = pc_net_match_seed();
                if (pc_net_matchmade()) {
                    /* Matchmaking: no CSS/SSS, straight into the match. */
                    matchmadeBuild(pc_net_match_seed());
                    am_game = 0;
                    rematch_direct = false;
                    gm_SetNextGameModeStateId(state_vs);
                    pc_log_line("lobby: entering matchmade VS at frame %d, seed %u", pc_net_frame(),
                                pc_net_match_seed());
                } else {
                    pc_log_line("lobby: entering CSS at frame %d, seed %u", pc_net_frame(),
                                pc_net_match_seed());
                }
                gm_801A4B60();
            }
            if ((input & HSD_PAD_START) && (state == PC_MATCH_FAIL || reason != PC_NET_PEER_OK)) {
                pc_net_peer_status_clear();
                /* Retry the mode we are actually in: a direct session used to
                 * restart as public matchmaking, dropping the friend's code. */
                if (online_kind == ONLINE_KIND_DIRECT && rematch_direct) {
                    startRematch();
                } else if (online_kind == ONLINE_KIND_DIRECT) {
                    directEntryBegin();
                } else {
                    startMatch(online_kind == ONLINE_KIND_RANKED ? PC_MATCH_RANKED :
                                       PC_MATCH_UNRANKED, NULL);
                }
            }
        }
        if (matchmadeMode()) {
            /* What this machine queues with (saved TEAM SELECT team, or
             * the CSS fallback), and the opponent's once matched. */
            PcNetTeam mine = matchmadeLocalTeam();
            teamLine(view.team[0], sizeof view.team[0], "Your team", &mine);
            const PcNetTeam* theirs =
                pc_net_matchmade() ? pc_net_team(pc_net_match_is_host() ? 1 : 0) : NULL;
            if (theirs != NULL) {
                teamLine(view.team[1], sizeof view.team[1], "Opponent", theirs);
            }
        }
        mnOnlineLobby_Update(&view);
        if (!choosing && !keep_lobby && (input & (HSD_PAD_B | PAD_CANCEL))) {
            sfxBack();
            rematch_direct = false;
            pc_net_peer_status_clear();
            pc_net_match_stop();
            gm_ChangeGameModeAfterCurrentScene(GM_MENU);
            gm_801A4B60();
        }
        return;
    }
    pc_lan_poll();
    state = pc_lan_state(&why);
    n = pc_lan_peers(peers, PC_LAN_MAX_PEERS);
    lobbyFillView(&view, state, why, peers, n);
    mnOnlineLobby_Update(&view);

    if (state == 2) {
        /* Both peers tick in lockstep once connected, so leaving on the
         * agreed frame puts the CSS on the same synced frame everywhere. */
        if (pc_net_frame() >= pc_lan_start_frame()) {
            *HSD_RandSeedPtr = pc_lan_seed();
            pc_log_line("lobby: entering CSS at frame %d, seed %u",
                        pc_net_frame(), pc_lan_seed());
            gm_801A4B60();
        }
        return;
    }
    if (input & (HSD_PAD_B | PAD_CANCEL)) {
        sfxBack();
        pc_net_peer_status_clear();
        pc_lan_stop();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
    } else if ((input & HSD_PAD_START) && (state == 0 || state == 3) &&
               lobbyCompatible(peers, n)) {
        sfxForward();
        pc_net_peer_status_clear(); /* a retry from 3 is not the last session's */
        pc_lan_start_match();
    }
#endif
}
