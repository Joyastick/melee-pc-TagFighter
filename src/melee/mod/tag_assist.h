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
 *  - Team-select UI; partner pairing is a hardcoded table
 *    (kPartnerKindForSlot) standing in for real CSS data.
 *  - Stocks/percent/HUD/results-screen integration (MatchPlayerData).
 *  - Duo mode (a second physical controller for the partner character).
 */

#include <melee/ft/forward.h>

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
