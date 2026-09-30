#include "mnonline.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include <melee/gm/gmonlinemode.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mod/tag_assist.h>
#include <sysdolphin/baselib/gobj.h>
#include <stdio.h>
#ifdef TARGET_PC
#include "pc/net_match.h"
#endif

/* Every page of MELEE VS is a menu kind of its own (forward.h), all drawn by
 * the same code: a kind change is what makes the panel play its exit and
 * enter animations, so the rows of one page never sit under the next's.
 *
 *   MAIN MENU > MELEE VS   MENU_KIND_MV_ROOT     LOCAL, ONLINE, CREDITS
 *                          MENU_KIND_MV_ONLINE   LAN PLAY, DIRECT CONNECT, PARTY,
 *                                                TEAM SELECT, MATCHMAKING
 *                          MENU_KIND_MV_PARTY    the Duo Party page
 *                          MENU_KIND_MV_CREDITS  a few lines of text
 *   VS MODE > ONLINE       MENU_KIND_ONLINE      the stock rows (LAN, Direct,
 *                                                Ranked, Unranked, Profile)
 */
enum { MV_LOCAL, MV_ONLINE, MV_CREDITS };
enum { MVO_LAN, MVO_DIRECT, MVO_PARTY, MVO_TEAM_SELECT, MVO_MATCHMAKING };
enum { PARTY_PARTNER, PARTY_YOU, PARTY_MATE, PARTY_POINT, PARTY_SEARCH };

/* <= 44 chars: that is what fits the bottom bar at mnmain.c's font size */
static const char* const vs_online_labels[] = {
    "LAN PLAY", "DIRECT CONNECT", "RANKED", "UNRANKED", "PROFILE",
};
static const char* const vs_online_descriptions[] = {
    "Play another player on your local network.",
    "Connect to a friend using a connect code.",
    "Play a rated best-of-three set.",
    "Find an opponent over the internet.",
    "View your player identity and connect code.",
};

static const char* const mv_root_labels[] = { "LOCAL", "ONLINE", "CREDITS" };
static const char* const mv_root_descriptions[] = {
    "Play locally, same as MELEE VS today.",
    "Play MELEE VS over LAN or the internet.",
    "Who made this.",
};

/* MELEE VS online is scoped to LAN + Direct + Party + Matchmaking (which is
 * ONLINE_KIND_UNRANKED under a player-facing MeleeVS name). TEAM SELECT picks
 * the fixed team Matchmaking queues with. */
static const char* const mv_online_labels[] = {
    "LAN PLAY", "DIRECT CONNECT", "PARTY", "TEAM SELECT", "MATCHMAKING",
};
static const char* const mv_online_descriptions[] = {
    "Play another player on your local network.",
    "Connect to a friend using a connect code.",
    "Team up with a friend and queue together.",
    "Pick the team you take into Matchmaking.",
    "Find an opponent online for a Tag Battle.",
};

/* The credits page is plain text rows; the cursor can rest on any of them. */
static const char* const credits_labels[] = {
    "MELEE VS",
    "PC PORT: 999SIAN",
    "DECOMP: DOLDECOMP",
    "GRAPHICS: AURORA",
    "THANKS FOR PLAYING",
};
static const char* const credits_descriptions[] = {
    "Tag Fighter, a 2v2 tag mode by Joyastick.",
    "melee-pc, the PC port this is built on.",
    "The doldecomp Melee decompilation project.",
    "Aurora, the GameCube graphics layer on PC.",
    "See the README for the rest.",
};

static bool sFromLocal;      /* LOCAL was launched from the MELEE VS page */
static int sLaunchKind;      /* the menu kind an online scene was launched from */
static unsigned sGeneration; /* bumped when a row's text changes (see the header) */

static const char* notice;

/* The Party page's rows are text made from the saved team, rebuilt as it is
 * asked for. */
static char party_text[5][40];
static const char* const party_descriptions[] = {
    "Link with a teammate online. Select again to leave the party.",
    "Your fighter. A opens TEAM SELECT.",
    "The fighter your partner picked (MATE).",
    "Who starts on point. You and your partner can both swap it.",
    "Search Matchmaking as a duo with your partner.",
};

static bool is_mv(int kind)
{
    return kind == MENU_KIND_MV_ROOT || kind == MENU_KIND_MV_ONLINE ||
           kind == MENU_KIND_MV_PARTY || kind == MENU_KIND_MV_CREDITS;
}

/* Rows the kind has (mnmain.c's table holds the same numbers). */
static int row_count(MenuKind kind)
{
    switch (kind) {
    case MENU_KIND_MV_ROOT:
        return (int) ARRAY_SIZE(mv_root_labels);
    case MENU_KIND_MV_ONLINE:
        return (int) ARRAY_SIZE(mv_online_labels);
    case MENU_KIND_MV_PARTY:
        return 5;
    case MENU_KIND_MV_CREDITS:
        return (int) ARRAY_SIZE(credits_labels);
    case MENU_KIND_ONLINE:
        return (int) ARRAY_SIZE(vs_online_labels);
    default:
        return 0;
    }
}

/* A party is two players on two machines, so there is no CPU assist here. The
 * rest of the page waits on PARTNER: a link made from it (the party lobby,
 * net_match.c) shows who the partner is, their fighter, and who is point. */
static void build_party_text(void)
{
    char you[24], mate[24];
    bool mate_human;
    int point;
    bool saved = gmOnline_SavedTeamText(you, mate, sizeof you, &mate_human, &point);
    char who[10];
    int mate_fighter = -1;
    bool mate_point = false;
    bool linked = false;
#ifdef TARGET_PC
    pc_net_party_set_pick(gmOnline_SavedFighter());
    linked = pc_net_party_partner(who, &mate_fighter, &mate_point);
#endif
    if (linked) {
        snprintf(party_text[PARTY_PARTNER], sizeof party_text[0], "WITH: %s", who);
        snprintf(party_text[PARTY_MATE], sizeof party_text[0], "MATE: %s",
                 gmOnline_FighterName(mate_fighter));
        snprintf(party_text[PARTY_POINT], sizeof party_text[0], "POINT: %s",
                 mate_point ? "MATE" : "YOU");
    } else {
        snprintf(party_text[PARTY_PARTNER], sizeof party_text[0], "PARTNER: NONE");
        snprintf(party_text[PARTY_MATE], sizeof party_text[0], "MATE: -");
        snprintf(party_text[PARTY_POINT], sizeof party_text[0], "POINT: -");
    }
    snprintf(party_text[PARTY_YOU], sizeof party_text[0], "YOU: %s", saved ? you : "NOT SET");
    snprintf(party_text[PARTY_SEARCH], sizeof party_text[0], "FIND MATCH");
}

bool mnOnline_ReturnFromLocal(int* kind, int* selection)
{
    if (!sFromLocal) {
        return false;
    }
    sFromLocal = false;
    *kind = MENU_KIND_MV_ROOT;
    *selection = MV_LOCAL;
    return true;
}

void mnOnline_ReturnMenu(int online_kind, int* kind, int* selection)
{
    /* Back on the page the online scene was launched from, on the row that
     * launched it. */
    *kind = is_mv(sLaunchKind) ? sLaunchKind : MENU_KIND_ONLINE;
    switch (*kind) {
    case MENU_KIND_MV_ONLINE:
        *selection = online_kind == ONLINE_KIND_DIRECT      ? MVO_DIRECT :
                     online_kind == ONLINE_KIND_TEAM_SELECT ? MVO_TEAM_SELECT :
                     online_kind == ONLINE_KIND_UNRANKED    ? MVO_MATCHMAKING :
                                                              MVO_LAN;
        break;
    case MENU_KIND_MV_PARTY:
        *selection = online_kind == ONLINE_KIND_UNRANKED ? PARTY_SEARCH :
                     online_kind == ONLINE_KIND_DIRECT   ? PARTY_PARTNER :
                                                           PARTY_YOU;
        break;
    default:
        *selection = online_kind == ONLINE_KIND_DIRECT ? SEL_ONLINE_DIRECT : SEL_ONLINE_LAN;
        break;
    }
}

unsigned mnOnline_LabelGeneration(void)
{
    return sGeneration;
}

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_MELEEVS) {
        return "MELEE VS";
    }
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "ONLINE";
    }
    if (selection < 0 || selection >= row_count(kind)) {
        return NULL;
    }
    switch (kind) {
    case MENU_KIND_MV_ROOT:
        return mv_root_labels[selection];
    case MENU_KIND_MV_ONLINE:
        return mv_online_labels[selection];
    case MENU_KIND_MV_PARTY:
        build_party_text();
        return party_text[selection];
    case MENU_KIND_MV_CREDITS:
        return credits_labels[selection];
    default:
        return vs_online_labels[selection];
    }
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_MELEEVS) {
        return "MeleeVS - 2v2 Tag Fighter Mode";
    }
    if (kind == MENU_KIND_VS && selection == SEL_VS_ONLINE) {
        return "Play against other players over the network.";
    }
    if (selection < 0 || selection >= row_count(kind)) {
        return NULL;
    }
    switch (kind) {
    case MENU_KIND_MV_ROOT:
        return mv_root_descriptions[selection];
    case MENU_KIND_MV_ONLINE:
        return mv_online_descriptions[selection];
    case MENU_KIND_MV_PARTY:
        return party_descriptions[selection];
    case MENU_KIND_MV_CREDITS:
        return credits_descriptions[selection];
    default:
        return vs_online_descriptions[selection];
    }
}

const char* mnOnline_Title(MenuKind kind)
{
    switch (kind) {
    case MENU_KIND_MV_ROOT:
        return "MELEE VS";
    case MENU_KIND_MV_ONLINE:
        return "ONLINE";
    case MENU_KIND_MV_PARTY:
        return "PARTY";
    case MENU_KIND_MV_CREDITS:
        return "CREDITS";
    default:
        return NULL;
    }
}

const char* mnOnline_TakeNotice(void)
{
    const char* s = notice;
    notice = NULL;
    return s;
}

static void enterOnline(OnlineKind kind)
{
    MenuExitData* data = gm_GetCurrentSceneExitData();
    sfxForward();
    sLaunchKind = mn_804A04F0.cur_menu;
    gmOnline_SetKind(kind);
    data->pending_mode = GM_ONLINE;
    gm_801A4B60();
}

/* Every MELEE VS online entry starts with Tag Battle forced on. */
static void tagSetup(void)
{
    TagAssist_EnterForcedOn();
    TagAssist_ApplyDefaultRules();
}

static void enterMatchmaking(void)
{
    tagSetup();
    /* No saved team yet: pick one first, then search. */
    if (!gmOnline_HasSavedTeam()) {
        gmOnline_SetTeamSelectThenSearch(true);
        enterOnline(ONLINE_KIND_TEAM_SELECT);
        return;
    }
    enterOnline(ONLINE_KIND_UNRANKED);
}

static void enterTeamSelect(void)
{
    tagSetup();
    gmOnline_SetTeamSelectThenSearch(false);
    enterOnline(ONLINE_KIND_TEAM_SELECT);
}

/// Move to another MELEE VS page; transition 1 goes in, 3 goes back.
static void goPage(MenuKind kind, int selection, int transition)
{
    mn_804A04F0.entering_menu = transition == 1;
    mn_80229894(kind, selection, transition);
}

static void confirmMeleeVs(void)
{
    switch (mn_804A04F0.cur_menu) {
    case MENU_KIND_MV_ROOT:
        switch (mn_804A04F0.hovered_selection) {
        case MV_LOCAL: {
            MenuExitData* data;
            sfxForward();
#ifdef TARGET_PC
            pc_net_match_stop();
#endif
            sFromLocal = true;
            TagAssist_EnterForcedOn();
            TagAssist_ApplyDefaultRules();
            data = gm_GetCurrentSceneExitData();
            data->pending_mode = GM_VS;
            gm_801A4B60();
            break;
        }
        case MV_ONLINE:
            sfxForward();
            goPage(MENU_KIND_MV_ONLINE, MVO_LAN, 1);
            break;
        case MV_CREDITS:
            sfxForward();
            goPage(MENU_KIND_MV_CREDITS, 0, 1);
            break;
        }
        break;
    case MENU_KIND_MV_ONLINE:
        switch (mn_804A04F0.hovered_selection) {
        case MVO_LAN:
            tagSetup();
            enterOnline(ONLINE_KIND_LAN);
            break;
        case MVO_DIRECT:
            tagSetup();
            enterOnline(ONLINE_KIND_DIRECT);
            break;
        case MVO_PARTY:
            sfxForward();
            goPage(MENU_KIND_MV_PARTY, PARTY_YOU, 1);
            break;
        case MVO_TEAM_SELECT:
            enterTeamSelect();
            break;
        case MVO_MATCHMAKING:
            enterMatchmaking();
            break;
        }
        break;
    case MENU_KIND_MV_PARTY:
        switch (mn_804A04F0.hovered_selection) {
        case PARTY_PARTNER:
#ifdef TARGET_PC
            if (pc_net_party_linked()) {
                sfxBack();
                pc_net_party_leave();
                sGeneration++;
                notice = "Left the party.";
                break;
            }
#endif
            tagSetup();
            gmOnline_SetPartyLink(true);
            enterOnline(ONLINE_KIND_DIRECT);
            break;
        case PARTY_YOU:
            enterTeamSelect();
            break;
        case PARTY_POINT:
            sfxForward();
#ifdef TARGET_PC
            if (pc_net_party_linked()) {
                pc_net_party_toggle_point();
                sGeneration++;
                break;
            }
#endif
            notice = "Link a partner first.";
            break;
        default: /* partner fighter, search */
            sfxForward();
#ifdef TARGET_PC
            if (pc_net_party_linked()) {
                notice = "Searching as a party arrives next.";
                break;
            }
#endif
            notice = "Link a partner first.";
            break;
        }
        break;
    default: /* credits: nothing to press */
        break;
    }
}

static void confirmVanilla(void)
{
    switch (mn_804A04F0.hovered_selection) {
    case SEL_ONLINE_LAN:
        enterOnline(ONLINE_KIND_LAN);
        break;
    case SEL_ONLINE_DIRECT:
        enterOnline(ONLINE_KIND_DIRECT);
        break;
    case SEL_ONLINE_UNRANKED:
        enterOnline(ONLINE_KIND_UNRANKED);
        break;
    case SEL_ONLINE_RANKED:
        enterOnline(ONLINE_KIND_RANKED);
        break;
    case SEL_ONLINE_PROFILE:
        enterOnline(ONLINE_KIND_PROFILE);
        break;
    default:
        break;
    }
}

/// @brief Online menu think, after mn_8022D594; every page of MELEE VS and
/// the stock VS MODE > ONLINE run it, told apart by mn_804A04F0.cur_menu.
void mnOnline_Think(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);
    int count = row_count(mn_804A04F0.cur_menu);

#ifdef TARGET_PC
    /* Open and bootstrap the DHT node while the player is still choosing, so
     * Direct/Unranked/Ranked start searching with a populated routing table.
     * The lobby takes it over (or closes it for LAN) on entry. */
    pc_net_match_warm();
#endif

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        if (mn_804A04F0.cur_menu == MENU_KIND_ONLINE) {
            confirmVanilla();
        } else {
            confirmMeleeVs();
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
#ifdef TARGET_PC
        if (mn_804A04F0.cur_menu != MENU_KIND_MV_PARTY) {
            pc_net_match_stop(); /* nothing polls it outside this menu */
        }
#endif
        switch (mn_804A04F0.cur_menu) {
        case MENU_KIND_MV_ONLINE:
            goPage(MENU_KIND_MV_ROOT, MV_ONLINE, 3);
            break;
        case MENU_KIND_MV_PARTY:
            goPage(MENU_KIND_MV_ONLINE, MVO_PARTY, 3);
            break;
        case MENU_KIND_MV_CREDITS:
            goPage(MENU_KIND_MV_ROOT, MV_CREDITS, 3);
            break;
        case MENU_KIND_MV_ROOT:
            mn_804A04F0.entering_menu = 0;
            mn_80229894(MENU_KIND_MAIN, SEL_MAIN_MELEEVS, 3);
            break;
        default:
            mn_804A04F0.entering_menu = 0;
            mn_80229894(MENU_KIND_VS, SEL_VS_ONLINE, 3);
            break;
        }
    } else if (buttons & MenuInput_Up) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + count - 1) % count;
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + 1) % count;
    }
}
