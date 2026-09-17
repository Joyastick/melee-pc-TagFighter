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

/// Whether "Tag Battle" is currently toggled on from the CSS rules screen
/// (see TagAssist_ToggleTagBattle). Off by default, so an un-toggled match
/// plays as ordinary Melee -- every other TagAssist_* hook below no-ops
/// while this is false.
bool TagAssist_IsTagBattleOn(void);

/// Flips the Tag Battle toggle. Call from the CSS rules-screen input
/// handler, mirroring how Team Battle's own is_teams flag is flipped.
void TagAssist_ToggleTagBattle(void);

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
bool TagAssist_IsPortPoint(int port);

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

#endif
