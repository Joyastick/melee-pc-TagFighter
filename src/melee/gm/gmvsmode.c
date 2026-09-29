#include "gmvsmode.h"

#include <stdlib.h>
#include <string.h>
#include <melee/lb/forward.h>

#include "forward.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmmovieend.h"
#include "gmresult.h"
#include "gmvsmelee.h"
#include "types.h"
#include <melee/if/if_2FD9.h>
#include <melee/lb/types.h>
#include <melee/mn/types.h>
#include <melee/mod/tag_assist.h>

/* 1B13B8 */ static void onEnterDebugVs(GameModeState*);
/* 1B14A0 */ static void onEnterCss(GameModeState*);
/* 1B14DC */ static void onExitCss(GameModeState*);
/* 1B1514 */ static void onEnterSss(GameModeState*);
/* 1B154C */ static void onExitSss(GameModeState*);
/* 1B1588 */ static void onEnterVs(GameModeState*);
/* 1B15C8 */ static void onExitVs(GameModeState*);
/* 1B1648 */ static void onEnterSuddenDeath(GameModeState*);
/* 1B1688 */ static void onExitSuddenDeath(GameModeState*);
/* 1B16A8 */ static void onEnterResults(GameModeState*);
/* 1B16C8 */ static void onExitResults(GameModeState*);

GameModeState gm_Mode_Vs_States[] = {
    {
        gmVsMode_State_Css,
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
        gmVsMode_State_Sss,
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
        gmVsMode_State_Vs,
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
        gmVsMode_State_SuddenDeath,
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
        gmVsMode_State_Results,
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
    {
        gmVsMode_State_Approach,
        lbDvdPreload_2,
        0,
        gm_ModeState_Approach_OnEnter,
        NULL,
        {
            GS_APPROACH,
            &gmVsMelee_ApproachData,
            &gmVsMelee_ApproachData,
        },
    },
    {
        gmVsMode_State_ApproachVs,
        lbDvdPreload_2,
        0,
        gm_ModeState_ApproachVs_OnEnter,
        gm_ModeState_ApproachVs_OnExit,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        gmVsMode_State_Prize,
        lbDvdPreload_2,
        0,
        gm_ModeState_Prize_OnEnter,
        gm_ModeState_Prize_OnExit,
        {
            GS_PRIZE_INTERFACE,
            &if_Scene_Prize_EnterData,
            NULL,
        },
    },
    { GM_GAMEMODESTATE_TERMINATE },
};

enum {
    state_debug_vs = 1,
    state_debug_results = 3,
};

GameModeState gm_Mode_DebugVs_States[] = {
    {
        state_debug_vs,
        lbDvdPreload_2,
        0,
        onEnterDebugVs,
        NULL,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        state_debug_results,
        lbDvdPreload_2,
        0,
        onEnterResults,
        NULL,
        {
            GS_RESULTS,
            &gmVsMelee_ResultsEnterData,
            NULL,
        },
    },
    { GM_GAMEMODESTATE_TERMINATE },
};

void onEnterDebugVs(GameModeState* state)
{
    StartMeleeData* start = gm_GetGameModeStateEnterData(state);
    ssize_t i;

    gm_SetupRulesDefaults(&start->rules);
    start->rules.stkind = St_Kind_Last;
    start->rules.item_freq = -1;
    start->rules.sd_penalty = -1;
    start->rules.match_kind = MatchKind_Time;

    for (i = 0; i < Gm_Player_NumMax; i++) {
        gm_SetupPlayerDefaults(&start->players[i]);
        start->players[i].stocks = 0;
        start->players[i].cpu_kind = 4;
    }

    start->players[0].ckind = CKind_Link;
    start->players[1].ckind = CKind_Mario;
    start->players[2].ckind = CKind_Link;
    start->players[3].ckind = CKind_Link;

    start->players[0].slot_type = Gm_PKind_Human;
    start->players[1].slot_type = Gm_PKind_Human;
    start->players[2].slot_type = Gm_PKind_NA;
    start->players[3].slot_type = Gm_PKind_NA;
#ifdef TARGET_PC
    /* MELEE_DEBUG_VS_STAGE=<StKind>: the debug match on one stage instead of
     * the last-used one, so the harness can reach a stage's mid-match loads
     * (3 is Pokemon Stadium, whose transformations load from disc). */
    if (getenv("MELEE_DEBUG_VS_STAGE") != NULL) {
        int st = atoi(getenv("MELEE_DEBUG_VS_STAGE"));
        if (st > St_Kind_Test && st < St_Kind_Last) {
            start->rules.stkind = (StKind) st;
        }
    }
    /* MELEE_DEBUG_VS_PLAYERS=<3|4>: humans on ports 3 and 4 too, for the
     * 3-4 machine netplay test (tools/net_group_test.py). */
    if (getenv("MELEE_DEBUG_VS_PLAYERS") != NULL) {
        int humans = atoi(getenv("MELEE_DEBUG_VS_PLAYERS"));
        static const CharacterKind extra[2] = { CKind_Fox, CKind_Donkey };
        for (i = 2; i < humans && i < Gm_Player_NumMax; i++) {
            start->players[i].ckind = extra[i - 2];
            start->players[i].slot_type = Gm_PKind_Human;
        }
    }
    if (getenv("MELEE_DEBUG_VS") != NULL && strcmp(getenv("MELEE_DEBUG_VS"), "cpu") == 0) {
        start->players[1].slot_type = Gm_PKind_Cpu;
    } else if (getenv("MELEE_DEBUG_VS") != NULL && strcmp(getenv("MELEE_DEBUG_VS"), "cpu4") == 0) {
        /* Four CPUs fighting each other: the worst-case scene for a
         * frame-time gate, with no input needed to keep it busy. */
        static const CharacterKind kinds[4] = {
            CKind_Link, CKind_Mario, CKind_Fox, CKind_Donkey,
        };
        for (i = 0; i < 4; i++) {
            start->players[i].ckind = kinds[i];
            start->players[i].slot_type = Gm_PKind_Cpu;
        }
    }
    // MELEE_DEBUG_VS=tag: same Link vs Mario shortcut, but with Tag Battle
    // forced on and Link given a CPU assist partner (port 2) so a single
    // keyboard can drive point/assist tag-swapping - lets synctest/
    // determinism drives exercise tag/call input resimulation without
    // walking the real CSS, same reasoning as the "cpu" variant above.
    // TagAssist_CssSyncPortTeam mirrors what the last CSS frame would have
    // left (its own doc comment: "the mirrored values stay put once CSS's
    // own per-frame updates stop"), needed here since this path never runs
    // CSS at all, so TagAssist's own sPortTeamColor defaults (all zero)
    // would otherwise group all three live ports into one team.
    // MELEE_DEBUG_VS=tag2v2: the same, as a full 2v2 (Link+Fox vs Mario+Donkey,
    // a CPU assist behind each human point) for the two-machine harness
    // (tools/net_pair_test.py --tag). Stocks stay large and the match untimed,
    // so it runs as long as the harness wants.
    if (getenv("MELEE_DEBUG_VS") != NULL &&
        (strcmp(getenv("MELEE_DEBUG_VS"), "tag") == 0 ||
         strcmp(getenv("MELEE_DEBUG_VS"), "tag2v2") == 0))
    {
        bool two_v_two = strcmp(getenv("MELEE_DEBUG_VS"), "tag2v2") == 0;
        start->players[2].ckind = CKind_Fox;
        start->players[2].slot_type = Gm_PKind_Cpu;
        start->players[2].cpu_kind = 4;
        // Every debug-VS variant zeroes stocks above (fine for the plain/cpu
        // shortcuts, which run MatchKind_Time and never check stocks) - but
        // TagAssist_CheckPointElimination reads stocks unconditionally, so
        // 0 reads as "already out of lives" and promotes the assist to solo
        // point within the first couple of frames, before any real
        // gameplay. Give the three live players real stocks so the team
        // actually stays a team.
        start->players[0].stocks = two_v_two ? 99 : 4;
        start->players[1].stocks = two_v_two ? 99 : 4;
        start->players[2].stocks = two_v_two ? 99 : 4;
        if (two_v_two) {
            start->players[3].ckind = CKind_Donkey;
            start->players[3].slot_type = Gm_PKind_Cpu;
            start->players[3].cpu_kind = 4;
            start->players[3].stocks = 99;
        }
        start->rules.is_teams = 1; // same forcing mncharsel.c does entering from the main menu
        TagAssist_EnterForcedOn();
        {
            /* MELEE_DEBUG_VS_PLAYERS=3|4 (tools/net_group_test.py --tag): one human
             * per machine on the port of its machine number, teams 0,1 vs 2,3
             * (the group layout, net_group.h). With 3 machines port 3 stays the
             * CPU assist behind the lone human of team B. */
            int humans = getenv("MELEE_DEBUG_VS_PLAYERS") != NULL ?
                             atoi(getenv("MELEE_DEBUG_VS_PLAYERS")) :
                             0;
            if (two_v_two && humans > 2) {
                start->players[2].ckind = CKind_Fox;
                start->players[2].slot_type = Gm_PKind_Human;
                if (humans > 3) {
                    start->players[3].slot_type = Gm_PKind_Human;
                }
                TagAssist_CssSyncPortTeam(0, 0);
                TagAssist_CssSyncPortTeam(1, 0);
                TagAssist_CssSyncPortTeam(2, 1);
                TagAssist_CssSyncPortTeam(3, 1);
                goto teams_synced;
            }
        }
        TagAssist_CssSyncPortTeam(0, 0);
        TagAssist_CssSyncPortTeam(2, 0);
        TagAssist_CssSyncPortTeam(1, 1);
        if (two_v_two) {
            TagAssist_CssSyncPortTeam(3, 1);
        }
    teams_synced:;
    }
    /* MELEE_DEBUG_VS_STOCKS=<n>: a stock match instead of an untimed time
     * one, so a run can end on GAME! with stocks the replay (src/pc/slp.c)
     * must carry. */
    if (getenv("MELEE_DEBUG_VS_STOCKS") != NULL) {
        int stocks = atoi(getenv("MELEE_DEBUG_VS_STOCKS"));
        if (stocks > 0 && stocks < 100) {
            start->rules.match_kind = MatchKind_Stock;
            start->rules.is_stock = true;
            for (i = 0; i < Gm_Player_NumMax; i++) {
                start->players[i].stocks = stocks;
            }
        }
    }
#endif

    start->players[0].rumble_enabled = false;
    start->players[1].rumble_enabled = false;
    start->players[2].rumble_enabled = false;
    start->players[3].rumble_enabled = false;

    gm_LoadAnnouncer();
}

void onEnterCss(GameModeState* state)
{
    gmVsMelee_EnterCss(state, gmVsMelee_GetVsData(), VS_MELEE);
}

void onExitCss(GameModeState* state)
{
    gmVsMelee_ExitCss(state, gmVsMelee_GetVsData());
}

void onEnterSss(GameModeState* state)
{
    gmVsMelee_EnterSss(state, gmVsMelee_GetVsData());
}

void onExitSss(GameModeState* state)
{
    gmVsMelee_ExitSss(state, gmVsMelee_GetVsData(), gmVsMode_State_Css);
}

void onEnterVs(GameModeState* state)
{
    gmVsMelee_EnterVs(state, gmVsMelee_GetVsData(), NULL, NULL);
}

void onExitVs(GameModeState* state)
{
    MatchExitInfo* mei;
    ssize_t i;

    // Tag Fighter: undo any human+CPU control-role swap left over from
    // tagging before the match-end transition runs at all -- see
    // TagAssist_RevertControlRolesForMatchEnd's own comment for why this
    // needs to happen here specifically (leaving it swapped soft-locks the
    // results screen's own "wait for Start" gate). player_slots[] alone
    // isn't enough since MatchEnd.player_standings[].pkind below is
    // already a stale snapshot by this point -- patched directly further
    // down instead.
    TagAssist_RevertControlRolesForMatchEnd();

    gmVsMelee_ExitVs(state, gmVsMode_State_Results,
                     gmVsMode_State_SuddenDeath);
    mei = gm_GetGameModeStateExitData(state);
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
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

    // Tag Fighter: forget this match's point/assist GObj pointers now that
    // the VS match is actually ending (natural end, Sudden Death handoff,
    // or an LRA+Start retry -- onExitVs fires for all of them). Confirmed
    // via a real crash: TagAssist_Tick's own per-frame elimination check
    // (TagAssist_CheckPointElimination) runs unconditionally every scene,
    // not just during a live VS match, so on the very next frame after
    // this scene exits it would otherwise keep dereferencing team->point/
    // assist -- pointers into GObj pool slots this scene transition is
    // free to recycle for something else entirely (same "stale
    // Fighter_GObj*" hazard TagAssist_OnReset's own comment already
    // describes for a hardware reset, just triggered by an ordinary scene
    // change instead). Reusing TagAssist_OnReset here is safe: the next
    // match's fighters re-initialize everything from scratch via
    // TagAssist_HandleNewMatch regardless, the same way a genuinely new
    // match after a hardware reset already does.
    TagAssist_OnReset();
}

void onEnterSuddenDeath(GameModeState* state)
{
    gmVsMelee_EnterSuddenDeath(state, gmVsMelee_GetVsData(), NULL, NULL);
}

void onExitSuddenDeath(GameModeState* state)
{
    gmVsMelee_ExitSuddenDeath(state);
}

void onEnterResults(GameModeState* state)
{
    gmVsMelee_EnterResults(state);
}

void onExitResults(GameModeState* state)
{
    gmVsMelee_ExitResults(state, gmVsMelee_GetVsData(), gmVsMode_State_Css);
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}
