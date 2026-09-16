#include "tag_assist.h"

#include <math.h>

#include <Runtime/platform.h>
#include <dolphin/os.h>
#include <melee/cm/camera.h>
#include <melee/cm/types.h>
#include <melee/ef/eflib.h>
#include <melee/ef/types.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftanim.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/if/textdraw.h>
#include <melee/if/textlib.h>
#include <melee/mp/forward.h>
#include <melee/mp/mpcoll.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/random.h>

#include <melee/ft/kinds/ftCommon/forward.h>

#include <melee/ft/kinds/ftCaptain/forward.h>
#include <melee/ft/kinds/ftDonkey/forward.h>
#include <melee/ft/kinds/ftFox/forward.h>
#include <melee/ft/kinds/ftGameWatch/forward.h>
#include <melee/ft/kinds/ftKoopa/forward.h>
#include <melee/ft/kinds/ftMario/forward.h>
#include <melee/ft/kinds/ftPopo/forward.h>
#include <melee/ft/kinds/ftZelda/forward.h>

/// v3 design (assist-call only -- tagging deliberately out of scope for now):
///
/// Ports are fixed roles, not swappable: Port 1 & Port 2 are always the
/// human "point" characters; Port 3 & Port 4 are always their "assist"
/// characters. Team A = Port 1 + Port 3, Team B = Port 2 + Port 4. Both
/// point and assist are REAL, already-existing player-slot Fighters from
/// the game's own normal spawn pipeline (assist ports set to CPU in CSS) --
/// nothing is ever spawned or freed by this module, so each has its own
/// independent stock/percent for free via the normal MatchPlayerData
/// system, with no risk of the flat/collapsed-model bugs an ad-hoc
/// Fighter_Create() mid-match produced in an earlier version.
///
/// Benching (v2 -> v3 fix): earlier versions only set rendering/collision
/// flags (invisible, x2219_b1 intangibility, etc.) on the assist. That
/// held for exactly one frame: the assist's own CPU AI (ftCo_800B3900,
/// ftCo_0A01.c) kept running every frame regardless, deciding to walk/
/// attack/whatever, and each state transition's own Enter-function cleanup
/// reset those flags back to normal -- "goes invisible for a second, then
/// back to being a normal CPU." The fix is fp->x221F_b3 = 1, which skips
/// the CPU AI think-call entirely (Fighter_8006ABA0, fighter.c) -- no
/// decisions are made, so nothing can reset anything. Combined with
/// fp->cpu.kind = CpuKind_5 (disables CPU control outright, per
/// ftCo_IsCpuControlled) this makes a genuinely inert, do-nothing fighter,
/// not just a fighter we're racing against its own AI.
///
/// Explicitly out of scope for this pass:
///  - Tagging (swapping which of point/assist is "active") -- come back to
///    this once assist-calling alone is solid.
///  - A dedicated menu/game-mode entry -- this still rides on a normal
///    4-player VS match; surfacing it as its own mode is a separate,
///    larger menu/scene-table change for later.
///  - Full roster coverage beyond TagAssist_GetSpecialNState's current
///    handful of characters.

/// D-Pad Down calls in your assist. Note this can also fire alongside
/// retail's own down-taunt if that's bound to the same input in a given
/// state -- known v1 overlap, revisit if that's a problem in practice.
#define TAG_ASSIST_PRESSED HSD_PAD_DPADDOWN

/// How long a called assist stays out before auto-benching.
/// 180 = 3 seconds at 60fps.
#define ASSIST_DURATION_FRAMES 180

/// Frames to let a freshly-spawned assist run completely untouched before
/// we freeze it for the first time.
///
/// This used to be 60 (~1s), cut to 5 out of concern it let the assist get
/// grabbed/physically linked to the point character before ever being
/// frozen -- symptom quoted at the time: "not affected by gravity, stuck
/// riding the point character's momentum" on the first call.
///
/// That's exactly the bug that came back at 5 frames, and increasing the
/// grace period before the first *call* (TAG_ASSIST_FIRST_CALL_GRACE_FRAMES)
/// didn't help at all -- proving the corruption happens at the first
/// *bench*, not the first call; waiting longer afterward can't fix state
/// that was already frozen mid-transition. The actual distinguishing
/// factor isn't point-character proximity (CSS spawn points are normally
/// well separated, and this module's own 10.0f re-spawn offset for
/// *later* calls is fine): every match starts with an intro/countdown
/// sequence (fighters descend onto the stage, "3, 2, 1, GO") that a real
/// mid-match death/respawn never repeats. 5 frames freezes the assist
/// while that one-time intro is still running, every match, without
/// fail -- corrupting something the intro never gets to finish, which a
/// later real respawn (a completely different code path) simply doesn't
/// share. 180 frames (3s) comfortably outlasts the intro.
#define INITIAL_SETTLE_FRAMES 180

typedef struct TeamState {
    Fighter_GObj* point;  ///< Port 1 or Port 2: always human, never touched
    Fighter_GObj* assist; ///< Port 3 or Port 4: CPU in CSS, benched by
                           ///< default, this module owns its behavior
    bool initialized;     ///< true once both members have been seen
    bool benched_once;    ///< true once the initial (post-settle) bench has
                           ///< actually been applied
    u32 settle_timer;     ///< frames left before the initial bench (0 once
                           ///< benched_once is true)
    bool assist_out;      ///< true while the assist has been called out
    u32 assist_timer;     ///< frames left before assist_out auto-benches
    u32 despawn_grace;    ///< hard-cap frames left to wait on
                           ///< ftAnim_IsFramesRemaining before re-benching
                           ///< unconditionally once assist_timer hits 0
    u32 ready_frame;      ///< sFrameCounter value at which the first-ever
                           ///< TryCallAssist is allowed to proceed -- see
                           ///< TAG_ASSIST_FIRST_CALL_GRACE_FRAMES
    u32 last_seen_frame;  ///< sFrameCounter value as of the most recent
                           ///< TagAssist_OnFighterInputFrame call for
                           ///< *either* member of this team -- see
                           ///< TAG_ASSIST_STALE_FRAMES
} TeamState;

/// If the assist gets KO'd for real while called out, forcing our bench
/// (x221F_b3=1) the instant assist_timer expires can land mid-way through
/// the real death/respawn action-state sequence -- Fighter_procUpdate
/// early-returns on x221F_b3, which can halt whatever later step in that
/// sequence re-arms the percent/stock HUD digit for this port. Waiting for
/// the current animation to finish (capped, in case it never reports
/// done) avoids interrupting that sequence.
///
/// Confirmed via a real bug: this used to be 20 (a third of a second) --
/// nowhere near long enough for an actual death->respawn cycle (fall as a
/// star, land, get up), which normally runs over a second. A real KO
/// while assist_out's timer had already run out got benched mid-respawn,
/// and its percent HUD digit never got reset even though the character
/// itself did respawn. Bumped to a full 3 seconds -- long enough for any
/// real respawn to finish, short enough that a genuinely stuck
/// ftAnim_IsFramesRemaining (idle loop) doesn't hold the assist forever.
#define ASSIST_DESPAWN_GRACE_FRAMES 180

/// Team A = Port 1 (point) + Port 3 (assist); Team B = Port 2 (point) +
/// Port 4 (assist). In 0-indexed player_id terms: team = player_id % 2,
/// role = player_id / 2 (0 = point, 1 = assist).
static TeamState sTeams[2];

/// Incremented once per TagAssist_DrawStatusOverlay call (i.e. once per
/// retail frame, unconditionally, every scene -- see gmscene.c). Used
/// together with TeamState::last_seen_frame to tell "this team's fighters
/// are still being simulated" apart from "the match ended (or a reset
/// happened) and these are now dangling Fighter_GObj pointers into freed
/// memory" -- see TAG_ASSIST_STALE_FRAMES for why this exists at all.
static u32 sFrameCounter;

/// How many frames of silence from TagAssist_OnFighterInputFrame (i.e. no
/// port on this team got a fighter-input-frame call at all) before the
/// overlay stops trusting sTeams[i].assist/point and treats them as stale.
/// Root-caused from a real crash: leaving a match (results screen, or a
/// hard reset) stops fighters from being simulated -- and therefore stops
/// TagAssist_OnFighterInputFrame from ever being called for them again --
/// but this module kept the last-known Fighter_GObj* around indefinitely,
/// and the overlay (which runs every frame in every scene, unconditionally)
/// kept dereferencing it. A hard reset frees the backing memory outright
/// (crash); an ordinary scene change merely recycles the GObj pool slot
/// (silently wrong data, not a crash) -- either way the pointer was never
/// safe to keep trusting once nothing is updating it. 2 frames is enough
/// margin for ordinary per-frame call-order jitter without masking a real
/// staleness for very long.
#define TAG_ASSIST_STALE_FRAMES 2

/// Frames to wait after a team is first seen before EVER allowing the
/// first TryCallAssist to go through. 300 = 5 seconds at 60fps.
///
/// Root cause of a real bug: a freshly-spawned fighter's own spawn-in
/// sequence keeps running internally in the background across several
/// action-state transitions even while this module holds it frozen (see
/// the "Reassert the bench state EVERY frame" comment below -- each such
/// transition resets x221F_b3, which is why we have to keep re-freezing
/// it every frame instead of once). If the very first call's
/// Fighter_ChangeMotionState into the assist's Neutral Special races
/// against that still-in-progress background sequence, the two
/// transitions can stomp on each other -- observed as the called assist
/// drifting with self_vel exactly zero at the moment of the call (ruled
/// out via TagAssist_TryCallAssist's own OSReport dump) but with gravity
/// and ground collision simply not applying for the rest of that one
/// move, until it eventually dies off-stage. A real death/respawn always
/// fixed it afterward because that cycle runs to completion, unlike the
/// interrupted spawn-in. INITIAL_SETTLE_FRAMES only controls the first
/// *bench*, not the first *call* -- 5 frames is nowhere near enough for a
/// spawn sequence to finish, so this is a separate, much longer gate
/// specifically on the first call.
#define TAG_ASSIST_FIRST_CALL_GRACE_FRAMES 300

/// Returns the Neutral Special action-state ID for a character, or -1 if
/// this character isn't wired up for assists yet. Extending roster coverage
/// is just adding more cases here using that character's own ftXx_MS_*
/// enum (see e.g. src/melee/ft/kinds/ftMario/forward.h).
static FtMotionId TagAssist_GetSpecialNState(FighterKind kind)
{
    switch (kind) {
    case Ft_Kind_Mario:
        return ftMr_MS_SpecialN;
    case Ft_Kind_Fox:
        return ftFx_MS_SpecialNStart;
    case Ft_Kind_Captain:
        return ftCa_MS_SpecialN;
    case Ft_Kind_Zelda:
        return ftZd_MS_SpecialN;
    case Ft_Kind_Donkey:
        return ftDk_MS_SpecialN;
    case Ft_Kind_Koopa:
        return ftKp_MS_SpecialN;
    case Ft_Kind_GameWatch:
        return ftGw_MS_SpecialN;
    case Ft_Kind_Popo:
        return ftPp_MS_SpecialN;
    default:
        return -1;
    }
}

/// Puts the assist into a genuinely inert dormant state: no CPU AI
/// decision-making at all (x221F_b3, which skips Fighter_8006ABA0's call
/// into ftCo_800B3900 outright -- see the module comment for why this,
/// not just rendering flags, is what actually makes it stick), CPU control
/// disabled outright as a second layer (cpu.kind = CpuKind_5), invisible,
/// intangible (x2219_b1 -- skips hurtboxes/collision AND
/// ftCo_800D3158's blast-zone/KO check, so this can never cost a stock),
/// and excluded from camera framing. The GObj/Fighter stays fully alive.
static void TagAssist_SetBenched(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    fp->x221F_b3 = 1;
    fp->cpu.kind = CpuKind_5;
    fp->invisible = true;
    fp->x2219_b1 = 1;
    fp->x221E_b1 = 1;
    fp->x221E_b2 = 1;
    fp->x221F_b1 = 1;
    fp->allow_interrupt = false;
    // Hold a sane scale while benched too (see the matching reset in
    // TagAssist_Unbench) -- invisible while benched so not visually
    // needed, but keeps it correct/settled by the time we unbench instead
    // of depending on that single reset alone.
    fp->x34_scale.x = 1.0f;
    fp->x34_scale.y = 1.0f;
    fp->x34_scale.z = 1.0f;
    if (fp->x890_cameraBox != NULL) {
        Camera_80028F5C(fp->x890_cameraBox, CmSubjectState_Inactive);
    }
}

/// Reverses TagAssist_SetBenched and repositions the assist next to
/// `nearGobj` (the point character), facing the same way.
///
/// A raw fp->cur_pos write isn't enough: a real fighter carries physics
/// state that only a real respawn normally resets. Skipping that produced
/// two bugs -- (1) stale velocity from before the freeze (or accumulated
/// during it) launches the fighter forward the instant physics resumes,
/// rendering flat/collapsed on top of that, and (2) after several
/// call/re-bench cycles the fighter stops moving to the new position at
/// all, because Fighter_procUpdate's grounded-physics update drags
/// fp->cur_pos every frame by whatever platform fp->coll_data.floor.index
/// last pointed at, regardless of what we write to cur_pos. Both are fixed
/// by mirroring what Fighter_UnkProcessDeath_80068354 actually does on a
/// real respawn: zero velocity (ftCommon_8007E2FC) and force a fresh
/// ground/floor re-detection (ground_or_air = GA_Air, gr_vel = 0,
/// coll_data.x130_flags |= CollData_X130_Locked to briefly lock out ECB
/// processing) instead of trusting stale coll_data.
static void TagAssist_Unbench(Fighter_GObj* gobj, Fighter_GObj* nearGobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    Fighter* nearFp = GET_FIGHTER(nearGobj);

    ftCommon_8007E2FC(gobj);
    // Ground the assist directly onto the point character's own current
    // floor when it has one, rather than always forcing GA_Air and
    // relying on collision re-detecting a floor within the very same
    // frame. Root-caused via frame-by-frame logging of a real slide: a
    // forced-airborne assist sat at self_vel.y == 0.0 (no gravity) for
    // exactly ~60 frames before gravity ever kicked back in -- the
    // signature of a move's own action-state script (Falcon Punch's
    // forward-glide phase, here) running its scripted physics assuming a
    // normal grounded context, which never expects to still be airborne
    // that long. Re-detection normally wins that same-frame race
    // (invisibly, which is why every later call "just worked"), but nothing
    // guarantees it does, and on this capture it didn't. The point
    // character is right there and, in every normal case, already
    // standing on a real floor -- copying it outright removes the race
    // entirely instead of gambling on winning it.
    if (nearFp->ground_or_air == GA_Ground && nearFp->coll_data.floor.index >= 0) {
        fp->ground_or_air = GA_Ground;
    } else {
        fp->ground_or_air = GA_Air;
    }
    fp->gr_vel = 0.0f;

    // Defensive scale reset: ftCommon_GetModelScale(fp) = fp->x34_scale.y *
    // fp->co_attrs.model_scaling -- x34_scale.y directly drives the actual
    // rendered/collision scale (via Fighter_UpdateModelScale ->
    // HSD_JObjSetScale). The intermittent "flat" model is consistent with
    // this getting caught mid-transition by some brief non-1.0 scale
    // window (e.g. a landing-squash effect) right as we freeze the assist
    // for benching, then never correcting itself since a frozen (x221F_b3)
    // fighter's own animation/state machine never gets the chance to
    // finish reverting it. Forcing 1:1:1 here and re-pushing it to the
    // joint transform guarantees a sane scale every time the assist wakes
    // up, regardless of what it was frozen mid-way through.
    fp->x34_scale.x = 1.0f;
    fp->x34_scale.y = 1.0f;
    fp->x34_scale.z = 1.0f;
    Fighter_UpdateModelScale(gobj);
    // NOTE: deliberately NOT setting coll_data.x130_flags |=
    // CollData_X130_Locked here anymore -- it was never cleared afterward
    // (retail always pairs it with a frame-counted ecb_lock timer that
    // expires it; we had no such timer), so it permanently froze this
    // fighter's environment-collision processing from the first call
    // onward. That's the likely cause of "died and did not go back to
    // normal" -- blast-zone/death handling probably depends on ECB
    // processing actually running.

    fp->cur_pos = nearFp->cur_pos;
    // Deliberately close (tighter than ftcommon.c's grounded anti-overlap
    // push threshold -- ftCommon_8007E0E4/xF8_playerNudgeVel -- for
    // wide-ECB characters like Captain Falcon): a wider offset (150.0f)
    // reliably cleared it, but read as spawning the assist too far from
    // the point character. At 10.0f, expect the same "something is
    // pushing it apart" slide to come back for wide-ECB characters once
    // the assist lands next to the point character and both are grounded
    // -- that's this same per-frame push, not a new bug, and not fixable
    // from the spawn offset alone at this distance. If that's not
    // acceptable, the real fix is on the OTHER side of the tradeoff:
    // suppress/clamp xF8_playerNudgeVel for the assist specifically
    // (e.g. intangibility already skips hurtboxes -- extending that or an
    // equivalent flag to this push check) rather than backing off the
    // spawn distance.
    fp->cur_pos.x += nearFp->facing_dir * 10.0f;
    fp->facing_dir = nearFp->facing_dir;
    HSD_JObjSetTranslate(GET_JOBJ(gobj), &fp->cur_pos);

    // The actual fix for "stuck on the last platform it ever landed on":
    // mpColl_80044628_Floor casts a SEGMENT from coll_data.prev_pos to
    // cur_pos every frame to detect the floor -- a raw cur_pos write
    // leaves prev_pos/last_pos stale, so that segment still runs from the
    // OLD platform through the new position and keeps re-acquiring the
    // old floor. mpColl_80043680 is retail's own teleport helper (Warp
    // Star uses exactly this, ftCo_WarpStar.c) -- it collapses
    // prev_pos/last_pos onto the new cur_pos so next frame's floor check
    // starts clean instead of dragging from where it used to be.
    mpColl_80043680(&fp->coll_data, &fp->cur_pos);
    if (fp->ground_or_air == GA_Ground) {
        // Known-good floor, copied outright -- see the ground_or_air
        // assignment above for why this replaces the old "always -1,
        // let collision re-detect it" approach.
        fp->coll_data.floor = nearFp->coll_data.floor;
    } else {
        fp->coll_data.floor.index = -1;
    }

    // Two more sources of "inherits the point character's momentum":
    // (1) xF8_playerNudgeVel is the anti-overlap push force between nearby
    // grounded fighters -- it accumulates from proximity to the point
    // character before we ever freeze this fighter, is normally cleared
    // once per frame but ONLY when intangible/grounded checks pass (which
    // fail the whole time we're frozen), and ftCommon_8007E2FC above does
    // NOT zero it. So it sits frozen, unapplied, then replays in full the
    // instant Fighter_procUpdate resumes on unbench. (2) while frozen,
    // Fighter_Spaghetti_8006AD10 skips the ENTIRE input-population block,
    // so fp->input.lstick/held_buttons are stale from the instant we froze
    // it -- some Neutral Specials read stick direction on entry, so a
    // stale "was moving right" reads as if the player still is.
    fp->xF8_playerNudgeVel.x = 0.0f;
    fp->xF8_playerNudgeVel.y = 0.0f;
    fp->input.lstick[0].x = 0.0f;
    fp->input.lstick[0].y = 0.0f;
    fp->input.lstick[1].x = 0.0f;
    fp->input.lstick[1].y = 0.0f;
    fp->input.lstick[2].x = 0.0f;
    fp->input.lstick[2].y = 0.0f;
    fp->input.cstick[0].x = 0.0f;
    fp->input.cstick[0].y = 0.0f;
    fp->input.cstick[1].x = 0.0f;
    fp->input.cstick[1].y = 0.0f;
    fp->input.cstick[2].x = 0.0f;
    fp->input.cstick[2].y = 0.0f;
    fp->input.held_buttons[0] = 0;
    fp->input.held_buttons[1] = 0;
    fp->input.held_buttons[2] = 0;
    fp->input.pressed_buttons = 0;
    fp->input.released_buttons = 0;

    fp->x221F_b3 = 0;
    fp->invisible = false;
    fp->x2219_b1 = 0;
    fp->x221E_b1 = 0;
    fp->x221E_b2 = 0;
    fp->x221F_b1 = 0;
    if (fp->x890_cameraBox != NULL) {
        Camera_80028F5C(fp->x890_cameraBox, CmSubjectState_Auto);
    }
}

static void TagAssist_TryCallAssist(TeamState* team)
{
    Fighter* pointFp = GET_FIGHTER(team->point);
    Fighter* assistFp = GET_FIGHTER(team->assist);
    FtMotionId specialN;

    if (team->assist_out) {
        return; // already out
    }
    if (!(pointFp->input.pressed_buttons & TAG_ASSIST_PRESSED)) {
        return;
    }
    if (sFrameCounter < team->ready_frame) {
        return; // see TAG_ASSIST_FIRST_CALL_GRACE_FRAMES
    }
    if (pointFp->ground_or_air != GA_Ground) {
        // Every supported character's assist move is called via its
        // GROUNDED Neutral Special motion ID (TagAssist_GetSpecialNState)
        // -- there's no aerial-variant lookup yet, and at least one
        // character's grounded move script doesn't handle actually being
        // airborne gracefully (observed: Falcon Punch's forward-glide
        // phase held self_vel.y at 0 well past its intended duration
        // when forced into the air, since a real move-triggered call is
        // the one case TagAssist_Unbench can't safely ground -- neither
        // fighter has a real floor to copy). Simplest safe fix for now:
        // don't allow calling the assist while the point character isn't
        // grounded, rather than risk it on every character until aerial
        // variants are actually wired up.
        return;
    }

    specialN = TagAssist_GetSpecialNState(assistFp->kind);
    if (specialN < 0) {
        return; // this character isn't wired up for assists yet
    }

    // Diagnostic for the per-character sliding-on-call bug: two attempts
    // at a timing fix (first-call grace period, then a longer first-bench
    // settle window) both failed to change the outcome, and the one
    // working case observed so far was a fighter that inherited an
    // ALREADY-benched state from a previous match's fighter object
    // occupying the same memory (assist spawned in already invisible) --
    // i.e. this isn't about *when* we freeze it, it's about some field
    // this module never touches that differs between a truly-fresh
    // spawn and a reused/already-initialized one. Dump the full
    // collision/ECB state BEFORE Unbench touches anything, so a broken
    // (fresh-spawn) capture can be diffed field-by-field against a
    // working (reused-memory) capture instead of guessing further.
    OSReport("TagAssist: PRE-UNBENCH kind=%d goa=%d floor_idx=%d "
             "x130=%08X env=%08X prev_env=%08X x221D_b5=%d x2219_b1=%d "
             "pos=%.2f,%.2f,%.2f prev_pos=%.2f,%.2f,%.2f "
             "last_pos=%.2f,%.2f,%.2f\n",
             (int) assistFp->kind, (int) assistFp->ground_or_air,
             assistFp->coll_data.floor.index, assistFp->coll_data.x130_flags,
             assistFp->coll_data.env_flags, assistFp->coll_data.prev_env_flags,
             (int) assistFp->x221D_b5, (int) assistFp->x2219_b1,
             assistFp->cur_pos.x, assistFp->cur_pos.y, assistFp->cur_pos.z,
             assistFp->coll_data.prev_pos.x, assistFp->coll_data.prev_pos.y,
             assistFp->coll_data.prev_pos.z, assistFp->coll_data.last_pos.x,
             assistFp->coll_data.last_pos.y, assistFp->coll_data.last_pos.z);

    TagAssist_Unbench(team->assist, team->point);
    Fighter_ChangeMotionState(team->assist, specialN, 0, 0.0f, 1.0f, 0.0f,
                              NULL);

    // Same fields, right as the new action state takes over, so we can
    // also see what Unbench actually changed vs. left alone.
    OSReport("TagAssist: POST-UNBENCH kind=%d sv=%.3f,%.3f gr=%.3f goa=%d "
             "scale=%.2f,%.2f,%.2f floor_idx=%d x130=%08X env=%08X "
             "x221D_b5=%d x2219_b1=%d\n",
             (int) assistFp->kind, assistFp->self_vel.x, assistFp->self_vel.y,
             assistFp->gr_vel, (int) assistFp->ground_or_air,
             assistFp->x34_scale.x, assistFp->x34_scale.y,
             assistFp->x34_scale.z, assistFp->coll_data.floor.index,
             assistFp->coll_data.x130_flags, assistFp->coll_data.env_flags,
             (int) assistFp->x221D_b5, (int) assistFp->x2219_b1);

    team->assist_out = true;
    team->assist_timer = ASSIST_DURATION_FRAMES;
    team->despawn_grace = ASSIST_DESPAWN_GRACE_FRAMES;
}

/// gfx_ids 22 and 24, captured live via MELEE_EF_LOG=1 firing together on
/// the exact frame Zelda's Down-B transform triggered (after standing
/// completely idle for 7s first, ruling out any spawn/landing effect as
/// contamination this time). Two earlier captures each turned out to be
/// polluted by something else in-frame with the transform -- gfx_id 5
/// visibly looked like a jump-dust puff, and a {2, 24} pairing visibly
/// looked like ground-landing wind, not a sparkle burst; 24 shows up in
/// both of those AND here, so it's likely a generic ground-impact dust
/// riding along rather than something transform-specific, kept here on
/// the assumption it's still part of the intended look (drop it if the
/// visual still reads as "landing wind" rather than sparkle -- 22 alone
/// would be the next thing to try). Both ids are < 1000, i.e.
/// gfx_id/1000 == 0 in efLib_Create's efAsync_DatEntries[gfx_id / 1000]
/// bank lookup -- the shared/common effect bank that's always loaded, not
/// a per-character one gated on which fighters happen to be in this
/// match. Safe to spawn regardless of whether Zelda is even one of the 8
/// characters this mod supports as an assist.
static const u32 kDespawnEffectGfxIds[2] = { 22, 24 };

/// Star/sparkle burst (Zelda's transform effect, see kDespawnEffectGfxIds)
/// at `gobj`'s current position. Each spawns with its own baked-in
/// animation/lifetime from its EF_EffectDesc, same as retail's own call
/// would -- no custom update callback or params needed. Purely cosmetic:
/// no gameplay effect, just a visible marker for the moment the assist
/// actually leaves.
static void TagAssist_SpawnDespawnEffect(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    Vec3 pos = fp->cur_pos;
    int i;

    for (i = 0; i < 2; i++) {
        efLib_Create_Attach_Pos(kDespawnEffectGfxIds[i], gobj, &pos);
    }
}

/// Diagnostic for the "slides on first call, fine after a real respawn"
/// report: dumps exactly what the grounded anti-overlap push
/// (ftCommon_8007E0E4/xF8_playerNudgeVel) and floor state look like while
/// the assist is actually out, throttled to every 10 frames so a 3-second
/// call produces a readable handful of lines instead of ~180. Comparing a
/// first-ever call against a call made after the assist has died and
/// respawned for real once should show whether coll_data.floor.index (or
/// the nudge push itself) actually differs between the two -- i.e. whether
/// TagAssist_Unbench is missing some init that a real spawn-in normally
/// does before a fighter's collision state is trustworthy.
static void TagAssist_LogCollisionState(Fighter_GObj* gobj, TeamState* team)
{
    Fighter* fp = GET_FIGHTER(gobj);
    // Frames elapsed since THIS call started, derived from the existing
    // countdown rather than a free-running counter -- a free-running one
    // would carry over between separate calls (and separate teams) and
    // stop lining up with "how long has this specific call been out".
    u32 frame = ASSIST_DURATION_FRAMES - team->assist_timer;
    // Every frame for the first 40 (0.66s) -- enough to catch exactly
    // which frame introduces bad velocity/position, since the call-time
    // snapshot alone (self_vel/gr_vel = 0) isn't the frame where the
    // slide actually appears -- then fall back to every 10th so a full
    // 3-second call doesn't spam the console for its whole duration.
    if (frame >= 40 && (frame % 10) != 0) {
        return;
    }
    OSReport("TagAssist: out f=%u sv=%.3f,%.3f gr=%.3f nudge=%.3f,%.3f "
             "floor_idx=%d goa=%d pos=%.2f,%.2f,%.2f\n",
             frame, fp->self_vel.x, fp->self_vel.y, fp->gr_vel,
             fp->xF8_playerNudgeVel.x, fp->xF8_playerNudgeVel.y,
             fp->coll_data.floor.index, (int) fp->ground_or_air,
             fp->cur_pos.x, fp->cur_pos.y, fp->cur_pos.z);
}

static void TagAssist_UpdateTimer(TeamState* team)
{
    if (!team->assist_out) {
        return;
    }
    TagAssist_LogCollisionState(team->assist, team);
    if (team->assist_timer > 0) {
        team->assist_timer--;
        return;
    }
    // Don't force the bench mid-animation -- in particular, don't
    // interrupt a real death/respawn sequence if the assist got KO'd
    // while called out. Capped so a state that never reports "done"
    // can't stall this forever.
    if (ftAnim_IsFramesRemaining(team->assist) && team->despawn_grace > 0) {
        team->despawn_grace--;
        return;
    }
    TagAssist_SpawnDespawnEffect(team->assist);
    TagAssist_SetBenched(team->assist);
    team->assist_out = false;
}

/// Diagnostic only: records every distinct fp->player_id this hook has ever
/// seen, so TagAssist_DrawStatusOverlay can show it.
static s8 sSeenPlayerIds[4] = { -1, -1, -1, -1 };

static void TagAssist_TrackPlayerId(u8 pid)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (sSeenPlayerIds[i] == (s8) pid) {
            return;
        }
        if (sSeenPlayerIds[i] == -1) {
            sSeenPlayerIds[i] = (s8) pid;
            return;
        }
    }
}

/// Detects a new match starting: our module-level TeamState is a plain C
/// static that lives for the whole process, but every match creates fresh
/// Fighter_GObj instances -- without this, a second match in the same
/// Dolphin session inherits the previous match's stale pointers/flags
/// (initialized=true with dead GObj pointers, an assist_out/timer left
/// over from however the last match ended, etc.), which is exactly what
/// produced the "next match started with the assist moving around, then
/// randomly went invisible" symptom. Resets THIS team's state the moment
/// either role's gobj changes out from under it; converges correctly
/// regardless of which of point/assist's frame call notices first (see the
/// inline comments below).
static void TagAssist_HandleNewMatch(TeamState* team, int roleIdx,
                                     Fighter_GObj* gobj)
{
    if (roleIdx == 0) {
        if (team->point != NULL && team->point != gobj) {
            team->initialized = false;
            team->benched_once = false;
            team->settle_timer = 0;
            team->assist_out = false;
            team->assist_timer = 0;
            team->assist = NULL; // let the assist's own call re-set this
        }
        team->point = gobj;
    } else {
        if (team->assist != NULL && team->assist != gobj) {
            team->initialized = false;
            team->benched_once = false;
            team->settle_timer = 0;
            team->assist_out = false;
            team->assist_timer = 0;
            team->point = NULL; // let the point's own call re-set this
        }
        team->assist = gobj;
    }
}

void TagAssist_OnFighterInputFrame(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    int teamIdx = fp->player_id % 2;
    int roleIdx = fp->player_id / 2; // 0 = point, 1 = assist
    TeamState* team;

    TagAssist_TrackPlayerId(fp->player_id);

    if (roleIdx >= 2) {
        return; // only 4 ports (2 teams of point+assist) are handled
    }
    team = &sTeams[teamIdx];
    TagAssist_HandleNewMatch(team, roleIdx, gobj);
    team->last_seen_frame = sFrameCounter;

    if (!team->initialized) {
        if (team->point == NULL || team->assist == NULL) {
            return; // waiting on both point and assist to (re)spawn
        }
        team->initialized = true;
        team->settle_timer = INITIAL_SETTLE_FRAMES;
        team->ready_frame = sFrameCounter + TAG_ASSIST_FIRST_CALL_GRACE_FRAMES;
    }

    if (gobj == team->assist) {
        if (!team->benched_once) {
            // Let a freshly-spawned assist run completely untouched for
            // INITIAL_SETTLE_FRAMES before ever freezing it -- see that
            // macro's comment.
            if (team->settle_timer > 0) {
                team->settle_timer--;
                return;
            }
            // Root cause of the sliding-on-first-call bug, found by
            // comparing two PRE-UNBENCH dumps within the same match that
            // were byte-for-byte identical (position, floor, ecb flags --
            // everything this module could read) yet one call slid and
            // the other didn't: it was never about *state data*, it's
            // about which action state the assist is transitioning FROM.
            // TagAssist_SetBenched only twiddles flags -- it never puts
            // the fighter in a real, fully-resolved action state, so the
            // very first freeze locks in whatever ad-hoc/interrupted
            // state its own spawn-in sequence happened to be mid-way
            // through. The first TryCallAssist then transitions directly
            // from that malformed state into the assist move, which is
            // what actually broke. Every later call works because by
            // then the fighter has already completed one full, clean
            // action-state cycle (the move itself, or a real
            // death/respawn) to transition from instead. Forcing a
            // known-good baseline state here -- before ever freezing it
            // for the first time -- means every subsequent call always
            // transitions from something real. ftCo_MS_Wait is a
            // fighter-generic idle state (used the same way elsewhere,
            // e.g. ftCo_800C7220.c), not X-specific, so this is safe
            // for every supported character.
            Fighter_ChangeMotionState(gobj, ftCo_MS_Wait, 0, 0.0f, 1.0f,
                                      0.0f, NULL);
            team->benched_once = true;
        }
        // Reassert the bench state EVERY frame while not called out,
        // rather than once: Fighter_ChangeMotionState resets x221F_b3 (and
        // friends) as part of any state transition, including ones the
        // assist's own spawn-in sequence triggers internally in the
        // background after our first bench call -- a one-time write loses
        // that race unpredictably (sometimes it wins, sometimes the assist
        // ends up "woken back up" as a normal-moving CPU). Continuously
        // holding the freeze here means any such reset gets corrected the
        // very next frame instead of silently sticking.
        if (!team->assist_out) {
            TagAssist_SetBenched(team->assist);
        }
        return;
    }

    if (gobj != team->point) {
        return;
    }

    TagAssist_TryCallAssist(team);
    TagAssist_UpdateTimer(team);
}

/// Always-on-screen confirmation that this build (not vanilla retail) is
/// what's actually running -- visible from the title screen onward, so you
/// don't need to start a match to tell the mod loaded. Hooked from
/// gmscene.c's scene-independent per-frame loop.
static DevText* sStatusText;
static char sStatusBuf[0x200];

static HSD_GObj* sStatusTextOwner;

/// DevText_Create(id, ...) silently returns NULL if `id` is already claimed
/// by another live DevText (see textlib.c's find_by_id) -- it does NOT
/// overwrite or queue behind the existing owner. dbanim.c's animation-info
/// debug overlay (gated behind DbLevel, off by default) also claims id 7;
/// picking something no other module uses avoids ever silently losing this
/// slot to it.
#define TAG_ASSIST_DEVTEXT_ID 100

void TagAssist_DrawStatusOverlay(void)
{
    HSD_GObj* curOwner = DevText_GetGObj();
    static u32 sDiagFrames = 0;

    sFrameCounter++;

    // Self-healing instead of "create once": the overlay has been observed
    // to vanish at scene transitions (CSS -> intro, intro -> match), which
    // is consistent with DevText's underlying driving GObj (curOwner) being
    // torn down and recreated by that transition, orphaning a cached
    // sStatusText that still looks non-NULL to us but no longer renders.
    // Recreating whenever the owner GObj changes handles that regardless
    // of exactly which transition is responsible.
    if (sStatusText == NULL || curOwner != sStatusTextOwner) {
        GXColor bg = { 0x00, 0x00, 0x00, 0xC0 };
        GXColor fg = { 0x40, 0xFF, 0x40, 0xFF };
        // Positioned mid-screen, away from the real match HUD (percent/
        // stock icons along the bottom, timer top-center).
        sStatusText =
            DevText_Create(TAG_ASSIST_DEVTEXT_ID, 100, 90, 42, 5, sStatusBuf);
        OSReport("TagAssist: overlay (re)create attempt, owner=%p -> %p\n",
                 (void*) curOwner, (void*) sStatusText);
        if (sStatusText == NULL) {
            return;
        }
        DevText_Show(curOwner, sStatusText);
        DevText_HideCursor(sStatusText);
        DevText_SetBGColor(sStatusText, bg);
        DevText_SetTextColor(sStatusText, fg);
        DevText_SetScale(sStatusText, 12.0f, 16.0f);
        sStatusTextOwner = curOwner;
    }

    // Cheap "is this hook even still running" heartbeat -- once every ~2s
    // at 60fps -- independent of whether DevText itself is visibly
    // rendering, so a real match can be checked against the console instead
    // of only against what's on screen.
    if ((sDiagFrames++ % 120) == 0) {
        OSReport("TagAssist: overlay heartbeat, owner=%p text=%p\n",
                 (void*) curOwner, (void*) sStatusText);
    }

    DevText_SetCursorXY(sStatusText, 0, 0);
    DevText_Printf(sStatusText, "tAi%do%d tBi%do%d", (int) sTeams[0].initialized,
                   (int) sTeams[0].assist_out, (int) sTeams[1].initialized,
                   (int) sTeams[1].assist_out);

    // sTeams[0].assist is a raw Fighter_GObj* that TagAssist_OnFighterInputFrame
    // stops refreshing as soon as its fighters stop being simulated (match
    // end -> results screen, or a reset) -- nothing else invalidates it, so
    // it can easily be a dangling pointer into freed memory by the time this
    // unconditional, every-scene, every-frame draw call gets to it. Root
    // cause of a real crash (TagAssist_DrawStatusOverlay reading through a
    // freed assist fighter right after leaving a match); see
    // TAG_ASSIST_STALE_FRAMES.
    if (sTeams[0].assist != NULL &&
        sFrameCounter - sTeams[0].last_seen_frame <= TAG_ASSIST_STALE_FRAMES)
    {
        Fighter* aFp = GET_FIGHTER(sTeams[0].assist);
        // Diagnostic for the "push force on first call" bug: velocities
        // as milli-units via %d in case this printf doesn't support %f.
        DevText_Printf(sStatusText, "\nsv%d,%d gr%d nu%d,%d",
                       (int) (aFp->self_vel.x * 1000.0f),
                       (int) (aFp->self_vel.y * 1000.0f),
                       (int) (aFp->gr_vel * 1000.0f),
                       (int) (aFp->xF8_playerNudgeVel.x * 1000.0f),
                       (int) (aFp->xF8_playerNudgeVel.y * 1000.0f));
    }
}

void TagAssist_OnReset(void)
{
    int i;
    OSReport("TagAssist: reset detected, dropping cached fighter pointers\n");
    for (i = 0; i < 2; i++) {
        sTeams[i].point = NULL;
        sTeams[i].assist = NULL;
        sTeams[i].initialized = false;
        sTeams[i].benched_once = false;
        sTeams[i].settle_timer = 0;
        sTeams[i].assist_out = false;
        sTeams[i].assist_timer = 0;
        sTeams[i].despawn_grace = 0;
    }
    // The reset also invalidates whatever memory backed the DevText pool
    // entry itself, not just the fighters -- force a clean recreate rather
    // than let the overlay keep writing through a pointer into freed
    // memory on the very first post-reset frame.
    sStatusText = NULL;
    sStatusTextOwner = NULL;
}
