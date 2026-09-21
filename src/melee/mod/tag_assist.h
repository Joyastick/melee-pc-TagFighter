#ifndef MELEE_MOD_TAG_ASSIST_H
#define MELEE_MOD_TAG_ASSIST_H

/**
 * First-pass "assist call" / tag prototype for the 2v2 tag-fighter mod.
 *
 * This is new gameplay code, not decompiled retail code: it has no
 * corresponding address in the original binary, so it is built as a
 * NonMatching object appended to the link (see configure.py). Building it
 * intentionally makes the output main.dol differ from retail, which is
 * expected once real gameplay changes exist.
 *
 * v1 scope (see the local design notes (not tracked in git) for the full design and TODOs):
 *  - One human-controlled "point" fighter per slot, plus at most one
 *    "assist" fighter per slot that the point character can call in.
 *  - Calling an assist spawns the slot's configured partner character,
 *    forces it directly into its own Neutral Special action state, and
 *    despawns it after a fixed duration (once its current animation ends).
 *  - While the assist is on screen, a second input swaps which of the two
 *    entities is the "active" (human-controlled) one for that slot -- the
 *    other becomes the new assist and is subject to the same duration.
 *  - The benched entity for a slot always receives neutral input, decoupled
 *    from the retail CPU/human input-routing tables (Player_8003248C and
 *    friends), so its behavior is fully owned by this module instead of the
 *    real CPU AI.
 *
 * Explicitly NOT handled yet (left for the next pass):
 *  - Per-character assist moves beyond Neutral Special, and full roster
 *    coverage (TagAssist_GetSpecialNState only maps a handful of
 *    characters right now).
 *  - Stocks/percent/HUD/results-screen integration (MatchPlayerData).
 *  - Duo mode (a second physical controller for the partner character).
 */

#include <melee/ft/forward.h>
#include <melee/pl/forward.h>

/// Whether "Tag Battle" is currently on (see TagAssist_EnterForcedOn). Off
/// by default, so a plain VS Mode CSS visit plays as ordinary Melee --
/// every other TagAssist_* hook below no-ops while this is false. The only
/// way in is the main menu's "TAG BATTLE" entry; there's no in-CSS toggle.
bool TagAssist_IsTagBattleOn(void);

/// Bitmask (HSD_PAD_* from sysdolphin/baselib/controller.h) of whichever
/// single extra GCC button the F1 menu's "MeleeVS: Tag Bind" setting
/// currently maps alongside D-Pad Down for calling/tagging, or 0 if that
/// setting is "Off" (D-Pad Down only). ftCo_Jump_GetInput reads this to stop
/// treating the same button as a jump input whenever it's X or Y.
u32 TagAssist_ExtraBindMask(void);

/// Turns Tag Battle on and arms TagAssist_ConsumeAutoPopulate for the CSS
/// setup that follows. Call from the main menu's "TAG BATTLE" entry, before
/// jumping into VS mode's CSS -- goes straight to a ready 2v2 instead of the
/// usual press-Start-per-door setup.
void TagAssist_EnterForcedOn(void);

/// Turns Tag Battle back off. Call once at CSS entry for every path that
/// ISN'T the main menu's "TAG BATTLE" entry, so a stale on-flag from an
/// earlier match doesn't leak into a plain VS Mode CSS visit -- CSS itself
/// has no toggle to turn this off anymore.
void TagAssist_LeaveTagBattle(void);

/// One-shot: true the first time this is called after TagAssist_EnterForcedOn,
/// false every time after (until the next TagAssist_EnterForcedOn). The CSS
/// setup code calls this exactly once, right as it opens the doors, so
/// backing out to the rules screen and back into CSS afterward doesn't keep
/// re-stomping choices the player already made.
bool TagAssist_ConsumeAutoPopulate(void);

/// Mirrors a CSS door's currently-selected team color (0 = Red, 1 = Blue,
/// 2 = Green) into this module, so the real gameplay pairing (who's on
/// whose team) can be read from the player's own CSS choice instead of a
/// fixed port layout. Call every CSS frame, for every non-empty door, while
/// Tag Battle is on -- the mirrored values stay put once CSS's own per-frame
/// updates stop and the match actually begins.
void TagAssist_CssSyncPortTeam(int port, unsigned char team_color);

/// Claims "point" for `port`'s team (Red or Blue), bumping whichever other
/// port was previously on that team down to "assist". Call when a player
/// presses Z while hovering their door's team button in the CSS. No-ops if
/// `team_color` isn't Red/Blue (2 = Green is never a valid Tag Battle team).
void TagAssist_SetExplicitPoint(unsigned char team_color, int port);

/// True if `port` is currently its Red/Blue team's point character (for the
/// CSS's flashing indicator, and internally for TagAssist_OnFighterInputFrame's
/// own role lookup). False for a port with no explicit team assignment yet or
/// one on Green -- Tag Battle can't start until every port is Red or Blue.
///
/// This is the fixed CSS-time assignment -- it does NOT track live in-match
/// tagging. Once a match is running, use TagAssist_IsPortCurrentlyPoint
/// instead for "who's point right now".
bool TagAssist_IsPortPoint(int port);

/// True if `port` is the port whose fighter is CURRENTLY playing the point
/// role for its team, tracking every live tag swap (TagAssist_TryTag),
/// point-elimination promotion, and revival -- unlike TagAssist_IsPortPoint,
/// which only reflects the original CSS-time assignment and never changes
/// once the match starts. Falls back to TagAssist_IsPortPoint before the
/// team's TeamState has finished initializing (both fighters not spawned in
/// yet). False if Tag Battle is off, `port` isn't on a valid Red/Blue team,
/// or `port` is 4 or higher.
bool TagAssist_IsPortCurrentlyPoint(int port);

/// True while `port`'s team currently has an assist called out and on
/// screen (see TagAssist_TryCallAssist / TagAssist_UpdateTimer). False if
/// Tag Battle is off, `port` isn't on a valid Red/Blue team, or no assist
/// is out for that team right now.
bool TagAssist_IsAssistOut(int port);

/// Frames left before `port`'s team's currently-called assist auto-benches
/// (the ASSIST_DURATION_FRAMES countdown TagAssist_UpdateTimer runs down
/// every frame). Only meaningful while TagAssist_IsAssistOut(port) is true;
/// returns 0 otherwise.
u32 TagAssist_GetAssistFramesLeft(int port);

/// How many more times the point character can tag with `port`'s team's
/// currently-called assist before TAG_MAX_TAGS_PER_CALL blocks further tag
/// input. Only meaningful while TagAssist_IsAssistOut(port) is true;
/// returns 0 otherwise.
u8 TagAssist_GetTagsRemaining(int port);

/// Call once per frame for every live fighter, from the same per-frame input
/// pass that populates fp->input (Fighter_Spaghetti_8006AD10). Handles
/// assist-call input, forced-move playback, auto-despawn, tag-swap input,
/// and neutral-input gating for whichever entity isn't currently active.
void TagAssist_OnFighterInputFrame(Fighter_GObj* gobj);

/// Advances this module's internal frame counter. Call once per frame from a
/// scene-independent hook (see gmscene.c) -- drives the first-call grace
/// period gating in TagAssist_TryCallAssist.
void TagAssist_Tick(void);

/// Clears every fighter pointer this module has cached. Call as soon as a
/// hardware reset (LRA+Start) is detected, before the engine actually tears
/// anything down -- unlike an ordinary scene change (menu/CSS/match, which
/// just recycles GObj pool slots and leaves a stale pointer merely wrong but
/// still mapped), a reset frees the backing memory pools outright, so a
/// stale Fighter_GObj* read afterward is a genuine use-after-free.
void TagAssist_OnReset(void);

/// Reverts every human+CPU team's control-role swap back to the original
/// CSS-configured pairing (the human plays the CSS-designated point port,
/// CPU AI plays the CSS-designated assist port) -- no-ops for a human+human
/// team, or if Tag Battle was never on. Call once, as the very first thing
/// in onExitVs (gmvsmode.c), before the match-end transition runs at all.
///
/// Without this, ending a match while tagged into an originally-CPU slot
/// soft-locks the results screen: gm_DefaultVSGetPauser-style port/slot
/// lookups and the results screen's own "wait for Start" gate
/// (fn_80177920/fn_801791E4, gmresultplayer.c) both key off state this mod
/// only ever meant to be swapped WHILE a match is live, never left swapped
/// across the transition to results. See also
/// TagAssist_GetOriginalPkindForMatchEnd -- reverting player_slots[] alone
/// isn't enough, since the results screen actually reads an already-taken
/// snapshot (MatchEnd.player_standings[].pkind) that isn't automatically
/// re-derived at match end.
void TagAssist_RevertControlRolesForMatchEnd(void);

/// Returns the CSS-original Gm_PKind (Human or Cpu) for `player_id` (a
/// player SLOT index, same meaning as Fighter->player_id and the `i` index
/// into MatchEnd.player_standings[]) if it's currently part of a human+CPU
/// team this mod has been swapping via tagging, or Gm_PKind_NA if this slot
/// isn't managed by this mod at all (a human+human team, an unpaired port,
/// or Tag Battle off). Call from onExitVs (gmvsmode.c) right after
/// TagAssist_RevertControlRolesForMatchEnd, to directly patch
/// MatchExitInfo->match_end.player_standings[player_id].pkind for any slot
/// this returns a real value for -- that field is a snapshot taken during
/// the match's own last live frame(s), not re-derived from player_slots[]
/// at match end, so reverting player_slots[] alone arrives too late to fix
/// what the results screen actually reads.
Gm_PKind TagAssist_GetOriginalPkindForMatchEnd(int player_id);

#endif
