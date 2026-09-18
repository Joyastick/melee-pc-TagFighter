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
#include <melee/ft/ft_0877.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/mp/forward.h>
#include <melee/mp/mpcoll.h>
#include <melee/mp/mplib.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/random.h>

#include <melee/ft/kinds/ftCommon/forward.h>

/// Each of these declares that character's move-Enter function(s) --
/// see TagAssist_GetAssistMoveEnter for why we call those directly instead
/// of a bare Fighter_ChangeMotionState.
#include <melee/ft/kinds/ftCaptain/ftcaptainspecialn.h>
#include <melee/ft/kinds/ftCaptain/ftcaptainspeciallw.h>
#include <melee/ft/kinds/ftDonkey/ftdonkey.h>
#include <melee/ft/kinds/ftDonkey/ftdonkeyspecialn.h>
#include <melee/ft/kinds/ftFox/ftfoxspecialn.h>
#include <melee/ft/kinds/ftGameWatch/ftgamewatchspecialn.h>
#include <melee/ft/kinds/ftKirby/ftkirby.h>
#include <melee/ft/kinds/ftKoopa/ftkoopaspecialhi.h>
#include <melee/ft/kinds/ftKoopa/ftkoopaspecialn.h>
#include <melee/ft/kinds/ftLink/ftlinkspecialhi.h>
#include <melee/ft/kinds/ftLink/ftlinkspecialn.h>
#include <melee/ft/kinds/ftLink/ftlinkspecials.h>
#include <melee/ft/kinds/ftLuigi/ftluigispeciallw.h>
#include <melee/ft/kinds/ftLuigi/ftluigispecialn.h>
#include <melee/ft/kinds/ftMario/ftmariospeciallw.h>
#include <melee/ft/kinds/ftMario/ftmariospecialn.h>
#include <melee/ft/kinds/ftMars/ftmarsspecialhi.h>
#include <melee/ft/kinds/ftMars/ftmarsspecialn.h>
#include <melee/ft/kinds/ftMewtwo/ftmewtwospecialn.h>
#include <melee/ft/kinds/ftMewtwo/ftmewtwospecials.h>
#include <melee/ft/kinds/ftNess/ftnessspecialn.h>
#include <melee/ft/kinds/ftNess/ftnessspecials.h>
#include <melee/ft/kinds/ftPeach/ftpeachspecialn.h>
#include <melee/ft/kinds/ftPikachu/ftpikachuspeciallw.h>
#include <melee/ft/kinds/ftPikachu/ftpikachuspecialn.h>
#include <melee/ft/kinds/ftPopo/ftpopospeciallw.h>
#include <melee/ft/kinds/ftPopo/ftpopospecialn.h>
#include <melee/ft/kinds/ftPurin/ftpurinspecialn.h>
#include <melee/ft/kinds/ftPurin/ftpurinspecials.h>
#include <melee/ft/kinds/ftSamus/ftsamusspecialn.h>
#include <melee/ft/kinds/ftSamus/ftsamusspecials.h>
#include <melee/ft/kinds/ftSeak/ftseakspecialn.h>
#include <melee/ft/kinds/ftYoshi/ftyoshispecialhi.h>
#include <melee/ft/kinds/ftYoshi/ftyoshispecialn.h>
#include <melee/ft/kinds/ftZelda/ftzeldaspecialn.h>

/// v4 design (adds tagging -- full point/assist role swap):
///
/// Point's D-Pad Down while the assist is already out swaps which of the two
/// is "point" (TagAssist_TryTag) -- unlike a call, this never forces a move
/// Enter or repositions anyone, since both fighters are already mid-match
/// wherever they actually are; it's a pure role handoff. The newly-demoted
/// fighter keeps fighting, visible and active, subject to the exact same
/// ASSIST_DURATION_FRAMES cameo timer TagAssist_UpdateTimer already runs for
/// an ordinary call (reused as-is, not a new mechanism).
///
/// Who can trigger a call in the first place now depends on whether the
/// assist is CPU or a second real player (TagAssist_InitControlRoles,
/// captured once when the team's roster settles): a CPU assist is still
/// called in by point's own input, same as before; a real second player
/// calls themself in on their own input instead -- point's job is only ever
/// to decide whether to tag once that assist is out, never to call in
/// another real player.
///
/// For a human+CPU team, tagging also has to move which physical controller
/// drives which entity, since "point" can now be the fighter CSS originally
/// configured as CPU (TagAssist_ApplyControlRoles): the newly-point fighter
/// gets fp->cpu.kind forced to CpuKind_5 (defeats ftCo_IsCpuControlled the
/// same way TagAssist_SetBenched already does, just for input routing this
/// time, not benching) plus its own fp->x618_player_id repointed at the
/// team's one real controller port; the newly-assist fighter gets its own
/// player_slots[].pkind flipped to Gm_PKind_Cpu (Player_SetSlottype) and a
/// real fp->cpu.kind restored, so retail's own Fighter_Spaghetti_8006AD10
/// picks it up as genuinely CPU-driven from the very next frame, no
/// per-frame copying needed. A human+human team needs none of this --
/// fp->x618_player_id and player_slots[].pkind never move away from each
/// player's own real port/Human designation in the first place, so each
/// player already always drives their own fighter regardless of role.
///
/// Team pairing and point/assist roles are read from the CSS's own Red/Blue
/// team-color selection and its per-team point-character choice (see
/// TagAssist_IsTagBattleOn / TagAssist_IsPortPoint and their CSS-side call
/// sites in mncharsel.c) -- no longer a fixed Port1+3/Port2+4 layout. Both
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
///  - A dedicated menu/game-mode entry -- this still rides on a normal
///    4-player VS match; surfacing it as its own mode is a separate,
///    larger menu/scene-table change for later.
///  - Any character whose assist move doesn't yet have an `_Enter`
///    function wired into TagAssist_GetAssistMoveEnter -- unwired kinds
///    return NULL there and simply can't be called out.

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
/// This used to need to be long enough (180f/3s) to outlast the match's
/// intro/countdown sequence ("3, 2, 1, GO"), because benching mid-intro
/// appeared to corrupt state the intro never got to finish. That symptom
/// turned out to actually be the same physics-inheritance bug
/// TagAssist_Unbench now fixes directly -- forcing a known-good
/// ftCo_MS_Wait state before the first freeze, zeroing stale velocity/
/// nudge/input, and re-grounding via mpColl_80043680 instead of trusting
/// stale coll_data (see TagAssist_Unbench's comments). With that fix in
/// place there's nothing left for a settle delay to protect against, so
/// this is 0 -- the assist benches (goes invisible) on the very first
/// frame it's seen.
#define INITIAL_SETTLE_FRAMES 0

/// Minimum frames after a call before point's next D-Pad Down is honored as
/// a tag instead of just being ignored -- without this, point could call an
/// assist and tag on the very next frame, converting the cameo into a full
/// swap before it's ever actually visible on screen. Purely a gate on the
/// TAG *input*: doesn't touch assist_timer/despawn_grace at all, so once a
/// tag does go through, the newly-demoted fighter still rides out the same
/// full ASSIST_DURATION_FRAMES cameo window as ever before benching --
/// nothing here shortens that.
#define TAG_MIN_CALL_TO_TAG_FRAMES 15

typedef struct TeamState {
    Fighter_GObj* port_gobj[2]; ///< [0] = whichever gobj currently sits at
                                 ///< this team's CSS-designated point PORT,
                                 ///< [1] = its assist port -- fixed physical
                                 ///< identity (TagAssist_IsPortPoint's
                                 ///< static, CSS-time mapping), used only by
                                 ///< TagAssist_HandleNewMatch to detect a
                                 ///< genuinely new match (a port's gobj
                                 ///< pointer actually changed). A tag never
                                 ///< touches this -- only point/assist below.
    Fighter_GObj* point;  ///< Whichever of port_gobj[0]/[1] is CURRENTLY
                           ///< playing "point" -- starts equal to
                           ///< port_gobj[0], and TagAssist_TryTag is the
                           ///< only thing that ever reassigns it afterward.
    Fighter_GObj* assist; ///< The other one -- benched by default, this
                           ///< module owns its behavior while it isn't
                           ///< "point".
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
    u32 tag_ready_frame;  ///< sFrameCounter value at which TagAssist_TryTag's
                           ///< input starts being honored -- set to
                           ///< sFrameCounter + TAG_MIN_CALL_TO_TAG_FRAMES on
                           ///< every successful call (not just the first).
    bool tagged_this_call; ///< true once TagAssist_TryTag has already
                            ///< succeeded once for the CURRENT call -- reset
                            ///< to false on every successful call, checked
                            ///< (and set) by TagAssist_TryTag so only one tag
                            ///< is allowed per assist call, not a repeated
                            ///< back-and-forth within the same cameo window.
    bool is_cpu_team;      ///< true if this team is one human point + one CPU
                            ///< assist, as opposed to two real players --
                            ///< captured once in TagAssist_InitControlRoles
                            ///< and never touched again (player_id/pkind
                            ///< identity doesn't move between GObjs, only
                            ///< which GObj is currently "point" does). Drives
                            ///< TagAssist_TryCallAssist's trigger source and
                            ///< whether TagAssist_ApplyControlRoles does
                            ///< anything at all.
    u8 human_pad_port;      ///< The real controller port (fp->x618_player_id)
                             ///< of whichever fighter started out human,
                             ///< captured before any tag ever touches it.
                             ///< Reapplied to whichever fighter is currently
                             ///< "point" so the same physical controller
                             ///< always drives point, even after point
                             ///< becomes the originally-CPU fighter.
    u8 cpu_pad_port;        ///< The ORIGINAL fp->x618_player_id of whichever
                             ///< fighter started out CPU, captured before any
                             ///< tag ever touches it. Reapplied to whichever
                             ///< fighter is currently "assist" so a fighter
                             ///< that has previously played point (and so had
                             ///< its own x618_player_id repointed at
                             ///< human_pad_port) doesn't keep reading the
                             ///< human's real controller as a fallback if
                             ///< ftCo_IsCpuControlled ever misreads it as not
                             ///< CPU-controlled for any reason -- belt and
                             ///< suspenders alongside the CpuKind_5 collision
                             ///< guard below.
    CpuKind saved_cpu_kind; ///< The original AI profile
                             ///< (fp->cpu.kind) of whichever fighter started
                             ///< out as the CPU assist, captured before any
                             ///< tag ever touches it (that fighter's own
                             ///< cpu.kind gets forced to CpuKind_5 whenever
                             ///< it's playing point -- see
                             ///< TagAssist_ApplyControlRoles). Reapplied to
                             ///< whichever fighter currently needs to act as
                             ///< real CPU so it always gets a legitimate
                             ///< profile, not whatever garbage an
                             ///< originally-human fighter's own never-used
                             ///< cpu.kind field happens to hold. Never
                             ///< actually CpuKind_5 -- TagAssist_InitControlRoles
                             ///< substitutes a fallback if the real captured
                             ///< value happens to collide with that sentinel
                             ///< (see its own comment).
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

/// sTeams[0] = the Red team, sTeams[1] = Blue -- which two ports belong to
/// each is no longer fixed by port number, but read from the CSS's own
/// Red/Blue team-color selection (see sPortTeamColor below).
static TeamState sTeams[2];
static bool sTagBattleOn = false;

/// Mirrors each port's CSS-selected team color (0 = Red, 1 = Blue, 2 =
/// Green) once per CSS frame (TagAssist_CssSyncPortTeam) -- frozen at
/// whatever it last was once the match begins and CSS's own per-frame
/// updates stop. Unindexed/empty ports are left however they last were;
/// TagAssist_OnFighterInputFrame only reads slots with a live fp anyway.
static u8 sPortTeamColor[4];

/// The port a player explicitly chose (via Z on the CSS team button) as
/// each team's point character; index 0 = Red, index 1 = Blue. -1 means no
/// explicit choice yet, in which case the lower port number on that team is
/// the default point (see TagAssist_IsPortPoint).
static s8 sExplicitPointPort[2] = { -1, -1 };

/// Incremented once per frame, unconditionally, every scene -- see
/// TagAssist_Tick and its call site in gmscene.c. Drives
/// TAG_ASSIST_FIRST_CALL_GRACE_FRAMES gating.
static u32 sFrameCounter;

/// Frames to wait after a team is first seen before EVER allowing the
/// first TryCallAssist to go through.
///
/// This used to guard against the called assist drifting off-stage with
/// self_vel stuck at zero on its first call -- a race between
/// Fighter_ChangeMotionState into the assist's Neutral Special and the
/// assist's own still-in-progress spawn-in sequence. That's now fixed at
/// the root in TagAssist_Unbench (explicit re-grounding, zeroed nudge
/// velocity/stale input, mpColl_80043680 teleport instead of trusting
/// stale coll_data) rather than by outwaiting the race, so the first call
/// no longer needs to be held back. 0 = allowed as soon as the team is
/// initialized (still subject to TagAssist_OnFighterInputFrame's own
/// point/assist-both-present gating).
#define TAG_ASSIST_FIRST_CALL_GRACE_FRAMES 0

/// A move's own Enter function -- e.g. ftMr_SpecialN_Enter, or any other
/// character's `_Enter(Fighter_GObj*)` for whichever single move that
/// character is wired to. Same signature for a Neutral/Side/Up/Down
/// Special, a smash, or a tilt, so this lookup isn't tied to "Neutral
/// Special" at all -- it's just whatever move each case below names.
typedef void (*TagAssistMoveFn)(Fighter_GObj* gobj);

/// Returns the assist move's own Enter function for a character, or NULL
/// if this character isn't wired up for assists yet.
///
/// This calls the move's real Enter function instead of driving
/// Fighter_ChangeMotionState to the move's motion-state ID directly (the
/// old approach). Retail never enters a move that way either -- every
/// move's Enter function does its own setup on top of the state change:
/// resetting cmd_vars, zeroing stale velocity, and critically, wiring up
/// whatever callback the move's animation-event script actually calls to
/// produce its effect. E.g. ftMr_SpecialN_Enter ends with
/// `fp->accessory4_cb = ftMr_SpecialN_ItemFireSpawn` -- skip that (as the
/// direct-ChangeMotionState approach did) and the animation plays but the
/// fireball callback is never wired up, so nothing spawns. Confirmed root
/// cause of "Mario/G&W's assist projectile doesn't come out": ftGw_SpecialN_Enter
/// has the identical pattern for Chef's sausages. Any move with its own
/// `_Enter` fits this lookup the same way, which is also what makes
/// picking something other than Neutral Special for a given character
/// (a smash, a tilt, a different special) just a different function
/// reference here, not a different mechanism.
///
/// Extending roster coverage is adding a case using that character's own
/// `ftXx_Yyyy_Enter` (declared in that character's own kinds/ftXx/*.h,
/// included above) for whichever move that character is wired to -- not
/// necessarily Neutral Special (see docs/tag_assist_roster.csv for the
/// current character -> move mapping; keep that file in sync with this
/// switch). A few clone characters reuse their base character's Enter
/// function directly because retail shares it, which itself branches
/// internally on fp->kind for whatever differs (item kind, effect color,
/// etc.) when it needs to -- e.g. ftFox_SpecialN_InitializeState's
/// fp->kind check for Fox vs Falco's blaster article, or
/// ftCaptain_SpecialN_CreateWindEffect's case Ft_Kind_Ganon -- and simply
/// has nothing left to differ (a bare Fighter_ChangeMotionState to a
/// shared motion-state ID) when it doesn't, since each character's own
/// separately-authored animation/hitbox data for that same state ID is
/// what actually carries the difference (e.g. ftMr_SpecialLw_Enter for
/// Mario/Dr. Mario's Tornado, or ftMs_SpecialHi_Enter for Marth/Roy's Up
/// Special -- neither has any fp->kind branch at all). Ness's Enter
/// functions are named SpecialNStart/SpecialHiStart/SpecialLwStart (each
/// charges or holds before releasing), not the plain Special_ used
/// elsewhere -- still the same shape.
///
/// Two moves here (Up Smash, Down Smash) aren't tied to a Neutral/Side/Up
/// /Down Special at all: see TagAssist_Common_AttackHi4_Enter and
/// TagAssist_Common_AttackLw4_Enter below.

/// Up Smash and Down Smash's assist entries (used by Zelda, Sheik, and Fox
/// for Up Smash; Peach for Down Smash).
///
/// Neither smash has a character-specific override for any of these four
/// (nor for almost anyone -- Ness is the one exception in retail for
/// both): every other character enters them through the exact same
/// generic, shared function -- `doEnter` in ftCo_AttackHi4.c /
/// ftCo_AttackLw4.c respectively. Those symbols happen to have external
/// linkage (missing a `static` retail's own build didn't need, since
/// decomp matching only cares about the compiled bytes, not the linkage)
/// but aren't declared in either file's own header -- they're not meant
/// to be called from outside their file. Rather than reach into a
/// private, incidentally-external symbol by guessing its exact
/// (unprefixed, collision-prone) name, these mirror their 3 lines
/// directly: lock interrupts, force the shared motion state, and hand off
/// to the standard animation-driven charge/release/IASA handling every
/// smash already uses. Keeps this entirely mod-owned instead of touching
/// decomp-matched common code for a mod-specific need.
static void TagAssist_Common_AttackHi4_Enter(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    fp->allow_interrupt = false;
    Fighter_ChangeMotionState(gobj, ftCo_MS_AttackHi4, Ft_MF_None, 0, 1, 0,
                              NULL);
    ftAnim_8006EBA4(gobj);
}

static void TagAssist_Common_AttackLw4_Enter(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    fp->allow_interrupt = false;
    Fighter_ChangeMotionState(gobj, ftCo_MS_AttackLw4, Ft_MF_None, 0, 1, 0,
                              NULL);
    ftAnim_8006EBA4(gobj);
}

/// Donkey Kong's assist move: Giant Punch, thrown instantly uncharged
/// instead of standing there winding up forever.
///
/// Giant Punch normally needs two separate Neutral Special inputs: the
/// first starts the windup (ftDk_MS_SpecialNStart -> ...Loop, gaining a
/// charge level each loop iteration), and a second B press is what
/// actually releases it -- ftDk_SpecialNLoop_IASA is what reads that
/// second press and transitions to the real punch (ftDk_MS_SpecialN).
/// Calling ftDk_SpecialN_Enter directly, like every other move's assist
/// entry, only ever does the first half: nothing ever delivers that
/// second press for a CPU-frozen assist, so it just stands there winding
/// up for the assist's whole time out and the punch never actually
/// throws. Confirmed: this is the "DK just charges and never punches"
/// bug.
///
/// This skips straight to the release state (ftDk_MS_SpecialN) instead,
/// replicating the same per-move setup ftDk_SpecialN_Enter itself does
/// (cmd_vars/velocity/damage-callback init -- see that function and its
/// file-local setCallbacks helper in ftdonkeyspecialn.c) plus exactly
/// what ftDk_SpecialNLoop_IASA does on that second B press. Charge
/// (fp->mv.dk.specialn.xC) is forced to 0 -- an assist call always starts
/// from fp->u.dk.x222C == 0 (no real windup time was ever spent), so
/// there's no charge to preserve either way. Net effect: an immediate,
/// uncharged (weakest) Giant Punch instead of a windup that never
/// releases -- the assist equivalent of tapping Neutral Special twice
/// instantly.
static void TagAssist_Dk_SpecialN_Enter(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);

    Fighter_ChangeMotionState(gobj, ftDk_MS_SpecialN, 0, 0, 1, 0, NULL);
    fp->mv.dk.specialn.xC = 0;
    fp->u.dk.x222C = 0;
    Fighter_ClearCmdVars(fp);
    fp->mv.dk.specialn.x0 = 0;
    fp->mv.dk.specialn.x4 = 0;
    fp->mv.dk.specialn.x14 = -1;
    fp->mv.dk.specialn.x10 = -1;
    ftCommon_8007D7FC(fp);
    fp->self_vel.y = 0;

    // Mirrors setCallbacks(gobj) in ftdonkeyspecialn.c (file-local, not
    // callable from here).
    fp->take_dmg_cb = ftDk_Init_8010D774;
    fp->death2_cb = ftDk_Init_8010D774;
    fp->take_dmg_2_cb = ftDk_SpecialN_DestroyAllEffects;
    Fighter_SetEffectHitlagCallbacks(fp);

    ftAnim_8006EBA4(gobj);
}

static TagAssistMoveFn TagAssist_GetAssistMoveEnter(FighterKind kind)
{
    switch (kind) {
    case Ft_Kind_Mario:
        return ftMr_SpecialN_Enter; // Neutral Special (Fireball)
    case Ft_Kind_DrMario:
        return ftMr_SpecialLw_Enter; // Down Special (Tornado) -- no fp->kind branch, identical for both
    case Ft_Kind_Fox:
        return TagAssist_Common_AttackHi4_Enter; // Up Smash
    case Ft_Kind_Falco:
        return ftFx_SpecialN_Enter; // Neutral Special (Blaster)
    case Ft_Kind_Captain:
        return ftCa_SpecialN_Enter; // Neutral Special (Falcon Punch)
    case Ft_Kind_Ganon:
        return ftCa_SpecialLw_Enter; // Down Special (Wizard's Foot) -- branches on fp->kind internally
    case Ft_Kind_Zelda:
        return TagAssist_Common_AttackHi4_Enter; // Up Smash
    case Ft_Kind_Seak:
        return TagAssist_Common_AttackHi4_Enter; // Up Smash
    case Ft_Kind_Donkey:
        return TagAssist_Dk_SpecialN_Enter; // instant uncharged Giant Punch -- see above
    case Ft_Kind_Koopa:
        return ftKp_SpecialHi_Enter; // Up Special (Whirling Fortress)
    case Ft_Kind_GameWatch:
        return ftGw_SpecialN_Enter; // Neutral Special (Chef)
    case Ft_Kind_Popo:
        return ftPp_SpecialLw_Enter; // Down Special (Blizzard)
    case Ft_Kind_Luigi:
        return ftLg_SpecialLw_Enter; // Down Special (Luigi Cyclone)
    case Ft_Kind_Mars:
        return ftMs_SpecialN_Enter; // Neutral Special (Shield Breaker)
    case Ft_Kind_Emblem:
        return ftMs_SpecialHi_Enter; // Up Special (Blazer) -- no fp->kind branch, identical for both
    case Ft_Kind_Yoshi:
        return ftYs_SpecialHi_Enter; // Up Special (Egg Throw)
    case Ft_Kind_Mewtwo:
        return ftMt_SpecialS_Enter; // Side Special (Confusion)
    case Ft_Kind_Peach:
        return TagAssist_Common_AttackLw4_Enter; // Down Smash
    case Ft_Kind_Samus:
        return ftSs_SpecialS_Enter; // Side Special (Missile)
    case Ft_Kind_Pikachu:
        return ftPk_SpecialLw_Enter; // Down Special (Thunder)
    case Ft_Kind_Pichu:
        return ftPk_SpecialN_Enter; // Neutral Special (Thunder Jolt)
    case Ft_Kind_Purin:
        return ftPr_SpecialS_Enter; // Side Special (Pound)
    case Ft_Kind_Kirby:
        return ftKb_SpecialS_Enter; // Side Special (Hammer Flip)
    case Ft_Kind_Link:
        return ftLk_SpecialHi_Enter; // Up Special (Spin Attack)
    case Ft_Kind_CLink:
        return ftLk_SpecialS_Enter; // Side Special (Boomerang) -- no fp->kind branch, identical for both
    case Ft_Kind_Ness:
        return ftNs_SpecialS_Enter; // Side Special (PK Fire)
    default:
        return NULL;
    }
}

/// v1 aerial assist table: every character currently just reuses their
/// grounded assist move -- this is a real, separate lookup rather than
/// TagAssist_TryCallAssist's airborne path just calling
/// TagAssist_GetAssistMoveEnter directly, so a character can later get its
/// own dedicated aerial-specific Enter function (e.g. an actual aerial
/// special/attack, instead of the grounded move entered at wherever the
/// ground-below raycast landed) without touching the grounded table at all.
static TagAssistMoveFn TagAssist_GetAssistMoveEnterAerial(FighterKind kind)
{
    return TagAssist_GetAssistMoveEnter(kind);
}

/// Distance (in-game units) to search straight down from a point for the
/// stage's own floor geometry when the point character calls an assist
/// while airborne -- large enough to span the full vertical extent any real
/// stage actually uses, so a hit means "there's a real floor down there
/// somewhere," full stop, not merely "somewhere nearby." Deliberately not
/// clamped any tighter than that -- the assist is allowed to land a full
/// stage height below an airborne point character; see
/// TagAssist_TryCallAssist.
#define TAG_ASSIST_AIR_RAYCAST_DISTANCE 100000.0f

/// Casts a straight line down from `pos` looking for the stage's own floor
/// collision geometry, and returns the hit position/surface if found.
///
/// Uses mpCheckFloor directly -- the same low-level segment-vs-floor-line
/// primitive mpColl_80044628_Floor (mpcoll.c) is itself built on -- instead
/// of routing through a full CollData/ECB setup and the wall/ceiling/squeeze
/// resolution loop that goes with it (see mpColl_80046904). All that's
/// needed here is "is there a floor down there, and if so where," not any
/// of that loop's other side effects.
///
/// The stage's own collision lines are defined in 2D (X/Y) only -- there's
/// no meaningful Z-depth collision -- so mpCheckFloor always writes 0 into
/// out_ground_pos->z; this fills it back in from `pos->z` afterward so the
/// result is directly usable as a real world position.
///
/// Returns false (out_ground_pos/out_floor untouched) if nothing is found
/// within TAG_ASSIST_AIR_RAYCAST_DISTANCE -- e.g. the point character is out
/// over a gap or blast zone with no floor below them at all.
static bool TagAssist_FindGroundBelow(const Vec3* pos, Vec3* out_ground_pos,
                                      SurfaceData* out_floor)
{
    if (!mpCheckFloor(pos->x, pos->y, pos->x,
                      pos->y - TAG_ASSIST_AIR_RAYCAST_DISTANCE, 0.0f,
                      out_ground_pos, &out_floor->index, &out_floor->flags,
                      &out_floor->normal, -1, -1, -1, NULL, NULL))
    {
        return false;
    }
    out_ground_pos->z = pos->z;
    return true;
}

/// If `gobj` is Popo (Ice Climbers' leader), returns Nana's own separate
/// Fighter_GObj -- NULL for every other character, and for Popo if Nana
/// somehow isn't present.
///
/// Ice Climbers' assist role is really TWO independent Fighter_GObj
/// instances running their own CPU AI: Popo (what TeamState.assist
/// actually points to) and Nana, a dependent sub-fighter who shares
/// Popo's own fp->player_id rather than having one of her own (retail's
/// design -- every one of Nana's own move-scripts looks Popo up via
/// Player_GetEntityAtIndex(nana_fp->player_id, 0), e.g.
/// ftnanaspecials.c). Benching/unbenching Popo alone does nothing to
/// Nana's own x221F_b3/cpu.kind/invisible flags -- confirmed root cause of
/// "Ice Climbers assist: Popo benches fine, Nana keeps running as a
/// normal CPU." index 1 (vs. Nana's own index-0 lookup for Popo) is
/// retail's existing pairing for the non-transforming (Popo, Nana) case,
/// the same player_entity/transformed[] mechanism Zelda/Sheik use for
/// their transform instead.
static Fighter_GObj* TagAssist_GetIceClimberPartner(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    if (fp->kind != Ft_Kind_Popo) {
        return NULL;
    }
    return Player_GetEntityAtIndex(fp->player_id, 1);
}

/// Puts the assist into a genuinely inert dormant state: no CPU AI
/// decision-making at all (x221F_b3, which skips Fighter_8006ABA0's call
/// into ftCo_800B3900 outright -- see the module comment for why this,
/// not just rendering flags, is what actually makes it stick), CPU control
/// disabled outright as a second layer (cpu.kind = CpuKind_5), invisible,
/// intangible (x2219_b1 -- skips hurtboxes/collision AND
/// ftCo_800D3158's blast-zone/KO check, so this can never cost a stock),
/// and excluded from camera framing. The GObj/Fighter stays fully alive.
///
/// Also benches Nana when `gobj` is Popo -- see
/// TagAssist_GetIceClimberPartner. The recursive call is on Nana, whose
/// kind is Ft_Kind_Nana, so TagAssist_GetIceClimberPartner returns NULL
/// for her and this doesn't recurse further.
static void TagAssist_SetBenched(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    Fighter_GObj* partner = TagAssist_GetIceClimberPartner(gobj);
    if (partner != NULL) {
        TagAssist_SetBenched(partner);
    }
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
/// `nearGobj` (the point character), facing the same way -- or, when
/// `groundPos`/`groundFloor` are given (non-NULL), directly onto that
/// already-confirmed floor position instead (the airborne-point-character
/// path: see TagAssist_FindGroundBelow and TagAssist_TryCallAssist). In that
/// case the assist can legitimately end up a full stage height away from
/// `nearGobj` -- there's no adjacency guarantee once the point character is
/// airborne, just "the ground directly below them, however far down that
/// is."
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
///
/// Also unbenches Nana when `gobj` is Popo -- see
/// TagAssist_GetIceClimberPartner. She's placed at the same `nearGobj`
/// -relative spot Popo is (briefly overlapping him) rather than
/// positioned relative to Popo specifically: her own AI immediately
/// starts closing whatever distance remains, exactly like retail's
/// existing Ice Climbers separation/reunion behavior, so this settles
/// within a frame or two rather than staying visibly wrong.
static void TagAssist_Unbench(Fighter_GObj* gobj, Fighter_GObj* nearGobj,
                              const Vec3* groundPos,
                              const SurfaceData* groundFloor)
{
    Fighter* fp = GET_FIGHTER(gobj);
    Fighter* nearFp = GET_FIGHTER(nearGobj);
    Fighter_GObj* partner = TagAssist_GetIceClimberPartner(gobj);
    if (partner != NULL) {
        TagAssist_Unbench(partner, nearGobj, groundPos, groundFloor);
    }

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
    if (groundPos != NULL) {
        // Airborne-point path: the caller already confirmed a real floor at
        // groundPos via TagAssist_FindGroundBelow, so there's nothing left
        // to race here the way the grounded path below has to.
        fp->ground_or_air = GA_Ground;
    } else if (nearFp->ground_or_air == GA_Ground && nearFp->coll_data.floor.index >= 0) {
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

    if (groundPos != NULL) {
        fp->cur_pos = *groundPos;
    } else {
        fp->cur_pos = nearFp->cur_pos;
        // Deliberately close (tighter than ftcommon.c's grounded
        // anti-overlap push threshold -- ftCommon_8007E0E4/
        // xF8_playerNudgeVel -- for wide-ECB characters like Captain
        // Falcon): a wider offset (150.0f) reliably cleared it, but read
        // as spawning the assist too far from the point character. At
        // 10.0f, expect the same "something is pushing it apart" slide to
        // come back for wide-ECB characters once the assist lands next to
        // the point character and both are grounded -- that's this same
        // per-frame push, not a new bug, and not fixable from the spawn
        // offset alone at this distance. If that's not acceptable, the
        // real fix is on the OTHER side of the tradeoff: suppress/clamp
        // xF8_playerNudgeVel for the assist specifically (e.g.
        // intangibility already skips hurtboxes -- extending that or an
        // equivalent flag to this push check) rather than backing off the
        // spawn distance.
        //
        // Not applicable to the groundPos case above: that spot is
        // wherever straight down from the point character actually is,
        // not necessarily anywhere near nearFp->cur_pos, so there's no
        // "next to them" overlap to nudge away from in the first place.
        fp->cur_pos.x += nearFp->facing_dir * 10.0f;
    }
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
    if (groundPos != NULL) {
        // Floor found by TagAssist_FindGroundBelow's own raycast, not
        // nearFp's -- nearFp is airborne in this path, so it has no floor
        // of its own worth copying.
        fp->coll_data.floor = *groundFloor;
    } else if (fp->ground_or_air == GA_Ground) {
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
    TagAssistMoveFn enterFn;
    bool pointGrounded;
    bool triggerPressed;
    Vec3 groundPos;
    SurfaceData groundFloor;

    if (team->assist_out) {
        return; // already out
    }
    if (team->is_cpu_team) {
        // CPU partner: point decides when to call it in, same as ever.
        triggerPressed = (pointFp->input.pressed_buttons & TAG_ASSIST_PRESSED) != 0;
    } else {
        // Real second player: only their own button calls them in. Can't
        // read assistFp->input for this -- TagAssist_SetBenched's freeze
        // (fp->x221F_b3) makes Fighter_Spaghetti_8006AD10 skip its entire
        // input-population block for a benched fighter (the same `if
        // (!fp->x221F_b3)` gate that wraps the call into this module's own
        // per-frame hook), so a benched player's real button press never
        // reaches assistFp->input at all. Read the raw controller state
        // directly instead -- a plain "currently held" check (not
        // edge-detected) is fine, since the assist_out guard above already
        // makes this a one-shot trigger no matter how many frames the
        // button stays held.
        triggerPressed = (HSD_PadGameStatus[assistFp->x618_player_id].button &
                          TAG_ASSIST_PRESSED) != 0;
    }
    if (!triggerPressed) {
        return;
    }
    if (sFrameCounter < team->ready_frame) {
        return; // see TAG_ASSIST_FIRST_CALL_GRACE_FRAMES
    }

    pointGrounded = pointFp->ground_or_air == GA_Ground;
    // Every wired move (grounded and, for now, aerial -- see
    // TagAssist_GetAssistMoveEnterAerial) is entered assuming a real floor
    // under the assist; at least one grounded move script doesn't handle
    // actually being airborne gracefully (observed: Falcon Punch's
    // forward-glide phase held self_vel.y at 0 well past its intended
    // duration when forced into the air). When the point character is
    // airborne, find a real floor directly below them first -- if there
    // isn't one (out over a gap/blast zone), don't call the assist at all
    // rather than risk entering a grounded-assuming move with nothing under
    // it.
    if (!pointGrounded &&
        !TagAssist_FindGroundBelow(&pointFp->cur_pos, &groundPos, &groundFloor))
    {
        return; // no floor found below the airborne point character
    }

    enterFn = pointGrounded ? TagAssist_GetAssistMoveEnter(assistFp->kind)
                            : TagAssist_GetAssistMoveEnterAerial(assistFp->kind);
    if (enterFn == NULL) {
        return; // this character isn't wired up for assists yet
    }

    if (pointGrounded) {
        TagAssist_Unbench(team->assist, team->point, NULL, NULL);
    } else {
        // Ground found by the raycast above, however far below the
        // (airborne) point character it actually is -- see
        // TagAssist_Unbench's groundPos handling.
        TagAssist_Unbench(team->assist, team->point, &groundPos, &groundFloor);
    }
    enterFn(team->assist);

    team->assist_out = true;
    team->assist_timer = ASSIST_DURATION_FRAMES;
    team->despawn_grace = ASSIST_DESPAWN_GRACE_FRAMES;
    team->tag_ready_frame = sFrameCounter + TAG_MIN_CALL_TO_TAG_FRAMES;
    team->tagged_this_call = false;
    OSReport("[TagAssist] call: assist kind=%d player_id=%d is_cpu_team=%d "
             "tag_ready_frame=%u\n",
             assistFp->kind, assistFp->player_id, team->is_cpu_team,
             team->tag_ready_frame);
}

/// gfx_ids 22 and 24. NOT confirmed to be Zelda's sparkle/glimmer transform
/// effect -- confirmed, via MELEE_EF_LOG_REPEAT gobj-attributed captures,
/// that this pairing is actually wrong on that front: 22 never spawns
/// during Zelda's Down-B at all, and 24 also spawns on unrelated gobjs
/// (likely generic ground-impact dust, not transform-specific). A
/// gfx_id-2-alone attempt was tried next (the one id that DID spawn
/// exclusively on Zelda's own gobj during the transform) but read as too
/// subtle in practice (closer to a double-jump puff than a sparkle burst).
/// Back on {22, 24} for now on the user's own call after comparing both in
/// game -- it's more visually prominent even though neither has been
/// confirmed to actually BE the transform sparkle. Revisit if a real
/// capture of the sparkle turns up later (see MELEE_EF_LOG_REPEAT in
/// eflib.c, added specifically to help re-attempt this). Both ids are <
/// 1000, i.e. gfx_id/1000 == 0 in efLib_Create's
/// efAsync_DatEntries[gfx_id / 1000] bank lookup -- the shared/common
/// effect bank that's always loaded, not a per-character one gated on
/// which fighters happen to be in this match. Safe to spawn regardless of
/// whether Zelda is even one of the characters this mod supports as an
/// assist.
static const u32 kDespawnEffectGfxIds[2] = { 22, 24 };

/// Visual burst (see kDespawnEffectGfxIds's own comment on why this isn't
/// confirmed to be Zelda's actual transform sparkle) at `gobj`'s current
/// position. Each spawns with its own baked-in animation/lifetime from its
/// EF_EffectDesc, same as retail's own call would -- no custom update
/// callback or params needed. Purely cosmetic: no gameplay effect, just a
/// visible marker for the moment the assist actually leaves.
static void TagAssist_SpawnDespawnEffect(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    Vec3 pos = fp->cur_pos;
    int i;

    for (i = 0; i < 2; i++) {
        efLib_Create_Attach_Pos(kDespawnEffectGfxIds[i], gobj, &pos);
    }
}

/// Fighter-side generic hit-impact "thwack" (weak/mid/strong severity
/// table, ftColl_803C0C40 = {141, 142, 143} in ftcoll.c, played via
/// ft_PlaySFX same as any normal attack's hit sound) -- picked as the tag
/// cue after confirming the actual Fan/Harisen item hit sound the request
/// was for isn't reachable this way at all: it's resolved through a
/// completely different audio path (lbColl_80005BB0/raw AX handles baked
/// into the item's own .dat animcmd data, with no C-level id), not
/// ft_PlaySFX's fighter-side sound table. Mid severity (142): always
/// resident in the fighter-common bank regardless of which items/
/// characters are in the match, and audibly close enough to a hit-based
/// cue to read as "something just connected" without being tied to any
/// one character's own voice/effect bank.
#define TAG_EFFECT_SFX_ID 142

/// Same sparkle burst as TagAssist_SpawnDespawnEffect, reused as-is (same
/// confirmed-safe, shared-bank gfx ids -- see kDespawnEffectGfxIds) at BOTH
/// fighters' positions the moment a tag actually swaps them, so the moment
/// reads clearly on screen regardless of which two characters are involved
/// or which one the camera happens to be favoring. Called from
/// TagAssist_TryTag right after the swap, so `newPoint`/`newAssist` are
/// already at their real, current positions -- no repositioning happens on
/// a tag (unlike a call), so this is purely a visual marker, no physics
/// implications. Plays TAG_EFFECT_SFX_ID once (not twice) -- attributed to
/// the new point, but audible either way since it's not spatialized by
/// distance the way the sparkle's position is.
static void TagAssist_SpawnTagEffect(Fighter_GObj* newPoint, Fighter_GObj* newAssist)
{
    TagAssist_SpawnDespawnEffect(newPoint);
    TagAssist_SpawnDespawnEffect(newAssist);
    ft_PlaySFX(GET_FIGHTER(newPoint), TAG_EFFECT_SFX_ID, 127, 64);
}

/// True while `gobj`'s fighter is anywhere in the common death->respawn
/// chain (falling as a star, landing, waking up, walking onto the
/// newly-spawned platform) -- ftCo_MS_DeadDown..ftCo_MS_RebirthWait are the
/// character-agnostic motion IDs retail chains through for every KO,
/// contiguous in ftCommon's own MotionState enum (ftCo_MS_Wait, the first
/// truly "done" state, immediately follows RebirthWait).
///
/// Root cause of "rebenched with no percent digit": TagAssist_UpdateTimer
/// used to gate solely on ftAnim_IsFramesRemaining, which -- at each
/// sub-state boundary inside this chain -- can read "no frames remaining"
/// for exactly one frame after the old sub-state's animation finishes but
/// before the next sub-state's Enter function has run and set new anim
/// data. If that one-frame gap landed on the same frame this module
/// checked, it read as "the assist is done, safe to bench" and force-froze
/// (x221F_b3) the fighter mid-sequence -- which, per Fighter_procUpdate's
/// early-return on that flag, halts whatever later step in the chain would
/// have re-armed this port's percent/stock HUD digit, even though the
/// fighter itself keeps existing in a normal, alive state. Checking the
/// actual current motion_id instead of animation-frame bookkeeping closes
/// that gap: it stays true across every sub-state transition in the chain,
/// with no boundary to race.
static bool TagAssist_IsInDeathSequence(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    return fp->motion_id >= ftCo_MS_DeadDown && fp->motion_id <= ftCo_MS_RebirthWait;
}

/// True once the assist's death/respawn sequence has reached the "angel
/// platform" specifically -- ftCo_MS_Rebirth (riding the platform down) or
/// ftCo_MS_RebirthWait (standing on it, about to drop through and become
/// a normal, controllable fighter again) -- as opposed to anywhere earlier
/// in TagAssist_IsInDeathSequence's wider range (still falling/flying as a
/// star, not yet confirmed landed on the platform).
///
/// This distinction matters because Fighter_UnkProcessDeath_80068354 --
/// which does the actual percent/HP reset for the new stock
/// (fp->dmg.x1830_percent = Player_GetDamage(...) inside
/// Fighter_UnkInitReset_80067C98) -- runs unconditionally at the very
/// start of ftCo_800D4FF4 (ft_0D4D.c), BEFORE the fighter is ever put into
/// ftCo_MS_Rebirth. So by the time motion_id reaches Rebirth, that reset
/// has already happened; freezing here can't leave the stale-percent bug
/// TagAssist_IsInDeathSequence's wider range was created to avoid (that
/// bug was freezing during the earlier DeadDown..DeadUpFall* substates,
/// BEFORE ftCo_800D4FF4 has ever run).
static bool TagAssist_HasReachedRebirth(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    return fp->motion_id == ftCo_MS_Rebirth || fp->motion_id == ftCo_MS_RebirthWait;
}

static void TagAssist_UpdateTimer(TeamState* team)
{
    if (!team->assist_out) {
        return;
    }

    // A real KO overrides the normal timer/animation-based despawn
    // entirely, checked every frame regardless of assist_timer -- a death
    // doesn't pause assist_timer, so waiting for assist_timer to run out
    // on its own could take up to its full remaining duration, during
    // which the assist finishes respawning, walks off the platform, and
    // starts fighting as a normal, un-benched CPU well before this
    // function ever re-checked anything. Bench the instant they're
    // confirmed safely on the platform instead of letting them ever climb
    // down off it. Confirmed root cause of "assist walks off the angel
    // platform and fights like a normal CPU."
    if (TagAssist_HasReachedRebirth(team->assist)) {
        TagAssist_SpawnDespawnEffect(team->assist);
        TagAssist_SetBenched(team->assist);
        team->assist_out = false;
        return;
    }

    if (team->assist_timer > 0) {
        team->assist_timer--;
        return;
    }
    // Don't force the bench mid-animation -- in particular, don't
    // interrupt a real death/respawn sequence if the assist got KO'd
    // while called out (TagAssist_HasReachedRebirth above already handles
    // that case as soon as it's safe; this is for the ordinary
    // non-death case, or for a death sequence that hasn't reached the
    // platform yet). Capped so a state that never reports "done" can't
    // stall this forever.
    if ((TagAssist_IsInDeathSequence(team->assist) ||
        ftAnim_IsFramesRemaining(team->assist)) && team->despawn_grace > 0)
    {
        team->despawn_grace--;
        return;
    }
    TagAssist_SpawnDespawnEffect(team->assist);
    TagAssist_SetBenched(team->assist);
    team->assist_out = false;
}

/// Captures whether this team is one human point + one CPU assist, as
/// opposed to two real players, plus (only for the CPU case) the one real
/// controller port and the CPU's own AI profile -- once, right as the team
/// first becomes initialized, before any tag has ever touched either
/// fighter's cpu.kind/x618_player_id/player_slots[].pkind. See the v4 design
/// comment above and TagAssist_ApplyControlRoles for why these need to
/// survive independent of which GObj currently holds "point".
static void TagAssist_InitControlRoles(TeamState* team)
{
    Fighter* pointFp = GET_FIGHTER(team->point);
    Fighter* assistFp = GET_FIGHTER(team->assist);
    bool pointIsCpu = Player_8003248C(pointFp->player_id, pointFp->is_sub_fighter) == Gm_PKind_Cpu;
    bool assistIsCpu = Player_8003248C(assistFp->player_id, assistFp->is_sub_fighter) == Gm_PKind_Cpu;

    team->is_cpu_team = assistIsCpu && !pointIsCpu;
    if (team->is_cpu_team) {
        team->human_pad_port = pointFp->x618_player_id;
        team->cpu_pad_port = assistFp->x618_player_id;
        // CpuKind_5 is both a real, ordinary AI profile AND the sentinel
        // TagAssist_ApplyControlRoles/TagAssist_SetBenched use elsewhere to
        // mean "not really CPU-controlled" (ftCo_IsCpuControlled special-
        // cases exactly that value). If the CPU fighter's own real,
        // CSS-assigned cpu.kind happens to BE 5, restoring it verbatim onto
        // the demoted fighter would silently defeat ftCo_IsCpuControlled
        // for real, and Fighter_Spaghetti_8006AD10 would fall through to
        // reading fp->x618_player_id instead -- which, without the
        // cpu_pad_port fix above, used to still be pointed at the human's
        // real controller from an earlier tag, driving both fighters off
        // one controller at once. Confirmed via real playtesting. Substitute
        // any other valid kind when this collision happens; which one
        // doesn't matter; the collision is what matters.
        team->saved_cpu_kind = (assistFp->cpu.kind == CpuKind_5)
                                    ? CpuKind_4 : assistFp->cpu.kind;
    }
    OSReport("[TagAssist] team init: point kind=%d player_id=%d, assist kind=%d "
             "player_id=%d, is_cpu_team=%d\n",
             pointFp->kind, pointFp->player_id, assistFp->kind,
             assistFp->player_id, team->is_cpu_team);
}

/// No-ops for a human+human team (each player's fp->x618_player_id and
/// player_slots[].pkind never move away from their own real port/Human
/// designation, so they already always drive their own fighter regardless
/// of role). For a human+CPU team, moves real control to whichever of
/// `newPoint`/`newAssist` needs it after a tag:
///  - newPoint gets fp->cpu.kind forced to CpuKind_5, which defeats
///    ftCo_IsCpuControlled on its own (same trick TagAssist_SetBenched
///    already uses, just for input routing here instead of benching) --
///    its own player_slots[].pkind never needs touching, since
///    ftCo_IsCpuControlled only reaches the cpu.kind check at all when
///    pkind == Gm_PKind_Cpu, which stays true for the originally-CPU
///    fighter regardless of which role it's playing. Its
///    fp->x618_player_id is repointed at the team's one real controller
///    port (team->human_pad_port) so Fighter_Spaghetti_8006AD10 reads the
///    right physical pad from the very next frame -- no per-frame copying,
///    same analog/deadzone/edge-detection path retail already runs.
///  - newAssist gets its own player_slots[].pkind flipped to Gm_PKind_Cpu
///    (Player_SetSlottype) -- needed because ftCo_IsCpuControlled
///    short-circuits false on pkind before it would ever reach a cpu.kind
///    check for a fighter CSS originally configured as human -- plus a
///    real cpu.kind restored from team->saved_cpu_kind (its own cpu.kind
///    field was never meaningfully set by CSS for a human port, and may
///    have been overwritten to CpuKind_5 by an earlier tag if it has
///    already played point once).
/// Called unconditionally every tag, including the case where newPoint
/// already happens to be the originally-human fighter -- each write is a
/// no-op in that case, not worth special-casing.
static void TagAssist_ApplyControlRoles(TeamState* team, Fighter_GObj* newPoint,
                                        Fighter_GObj* newAssist)
{
    Fighter* newPointFp;
    Fighter* newAssistFp;

    if (!team->is_cpu_team) {
        return;
    }

    newPointFp = GET_FIGHTER(newPoint);
    newAssistFp = GET_FIGHTER(newAssist);

    Player_SetSlottype(newPointFp->player_id, Gm_PKind_Human);
    newPointFp->cpu.kind = CpuKind_5;
    newPointFp->x618_player_id = team->human_pad_port;
    // Pause gating (gm_DefaultVSGetPauser, gmvs.c) doesn't read
    // x618_player_id or pkind at all -- it brute-forces every hardware
    // port against every slot looking for HSD_PAD_START, and only accepts
    // a match where `mpPlayerId == Player_GetPlayerId(slot)`, i.e.
    // player_slots[slot].player_id (a SEPARATE field from both of the
    // above, fixed at CSS time and otherwise never touched by this mod).
    // Without this, tagging into an originally-CPU slot silently breaks
    // Start/pause for the real controller now driving it -- confirmed via
    // playtesting. Keep it in sync with x618_player_id so the slot's
    // registered hardware port always matches whichever real controller is
    // actually driving it right now.
    Player_SetPlayerId(newPointFp->player_id, team->human_pad_port);

    // Seed newPointFp's own button-edge history to match what the real
    // controller is holding RIGHT NOW, not whatever this fighter's own
    // held_buttons[1]/[2] last held (stale CPU-synthesized state, likely
    // all zero for a button like D-Pad Down the CPU AI never presses).
    // Without this, Fighter_Spaghetti_8006AD10's edge detection compares
    // the fresh real read against that stale history on the very next
    // frame -- if the player is still physically holding the tag button
    // down (completely normal; a press isn't released instantaneously),
    // this fighter sees a brand-new rising edge on an already-held button,
    // synthesizing a SECOND tag input one frame after the first and
    // immediately swapping back. Confirmed via [TagAssist] logs: every
    // tag fired twice in a row and canceled itself out, invisible to the
    // player. Seeding all three held_buttons slots (matching whichever of
    // Fighter_Spaghetti_8006AD10's two rotation branches runs next) means
    // the first frame reading real input sees no discontinuity, so no
    // spurious edge -- same defensive pattern TagAssist_Unbench already
    // uses when handing a fighter's input back to a live, non-stale
    // source after a freeze.
    newPointFp->input.held_buttons[0] = HSD_PadGameStatus[team->human_pad_port].button;
    newPointFp->input.held_buttons[1] = newPointFp->input.held_buttons[0];
    newPointFp->input.held_buttons[2] = newPointFp->input.held_buttons[0];

    Player_SetSlottype(newAssistFp->player_id, Gm_PKind_Cpu);
    newAssistFp->cpu.kind = team->saved_cpu_kind;
    // Restore newAssistFp's own ORIGINAL pad port too, not just its pkind/
    // cpu.kind -- if this fighter has previously played point, its
    // x618_player_id is still left pointed at team->human_pad_port from
    // that promotion (nothing else ever moves it back). That's normally
    // harmless (ftCo_IsCpuControlled's human/CPU branch never reads
    // x618_player_id for a genuinely CPU-controlled fighter), but it turns
    // into "both fighters respond to the same controller" the instant
    // ftCo_IsCpuControlled misreads this fighter as not CPU-controlled for
    // ANY reason -- confirmed in practice via the CpuKind_5 collision this
    // same tag guards against above. Belt and suspenders: even if that
    // check is ever wrong again for some other reason, this fighter falls
    // back to its own real CPU pad port, never the human's.
    newAssistFp->x618_player_id = team->cpu_pad_port;
    // Mirror the point-side Player_SetPlayerId fix -- restore this slot's
    // registered hardware port back to the CPU's own (no-controller) port
    // too, so pause's port<->slot lookup doesn't keep pointing at the
    // human's real port for a slot the human isn't driving anymore.
    Player_SetPlayerId(newAssistFp->player_id, team->cpu_pad_port);

    OSReport("[TagAssist] control roles applied: point player_id=%d now pad_port=%d "
             "cpu.kind=%d; assist player_id=%d now pad_port=%d cpu.kind=%d\n",
             newPointFp->player_id, newPointFp->x618_player_id, newPointFp->cpu.kind,
             newAssistFp->player_id, newAssistFp->x618_player_id, newAssistFp->cpu.kind);
}

/// Point's own D-Pad Down while the assist is already out: a full role
/// swap, not another call. Unlike TagAssist_TryCallAssist, this never
/// forces a move Enter or repositions anyone -- both fighters are already
/// mid-match wherever they actually are, so this is a pure handoff of the
/// "point" label (and, for a human+CPU team, of real control -- see
/// TagAssist_ApplyControlRoles). The newly-demoted fighter is left exactly
/// as active/visible as it already was and simply starts counting down the
/// same cameo timer TagAssist_UpdateTimer already runs for an ordinary
/// call, so it benches itself the same way once that runs out (or sooner,
/// if it gets KO'd -- TagAssist_HasReachedRebirth handles that path
/// already, unchanged).
static void TagAssist_TryTag(TeamState* team)
{
    Fighter* pointFp = GET_FIGHTER(team->point);
    Fighter_GObj* newPoint;
    Fighter_GObj* newAssist;

    if (!(pointFp->input.pressed_buttons & TAG_ASSIST_PRESSED)) {
        return;
    }
    if (team->tagged_this_call) {
        // Only one tag allowed per assist call -- a second (or third...)
        // press within the same cameo window is ignored, not treated as
        // another swap back and forth. Resets on the next real call (see
        // TagAssist_TryCallAssist).
        OSReport("[TagAssist] tag input BLOCKED: already tagged once this "
                 "call\n");
        return;
    }
    if (sFrameCounter < team->tag_ready_frame) {
        // see TAG_MIN_CALL_TO_TAG_FRAMES -- too soon after the call. This
        // only fires on an actual button press (pressed_buttons is
        // edge-triggered), so it's a one-shot log per rejected attempt, not
        // per-frame spam.
        OSReport("[TagAssist] tag input BLOCKED: %u frame(s) left on the "
                 "call-to-tag delay\n", team->tag_ready_frame - sFrameCounter);
        return;
    }

    newPoint = team->assist;
    newAssist = team->point;
    TagAssist_ApplyControlRoles(team, newPoint, newAssist);

    team->point = newPoint;
    team->assist = newAssist;
    team->assist_out = true;
    team->assist_timer = ASSIST_DURATION_FRAMES;
    team->despawn_grace = ASSIST_DESPAWN_GRACE_FRAMES;
    team->tagged_this_call = true;
    TagAssist_SpawnTagEffect(newPoint, newAssist);
    OSReport("[TagAssist] tag OK: new point kind=%d player_id=%d, new assist "
             "kind=%d player_id=%d\n",
             GET_FIGHTER(newPoint)->kind, GET_FIGHTER(newPoint)->player_id,
             GET_FIGHTER(newAssist)->kind, GET_FIGHTER(newAssist)->player_id);
}

/// Detects a new match starting: our module-level TeamState is a plain C
/// static that lives for the whole process, but every match creates fresh
/// Fighter_GObj instances -- without this, a second match in the same
/// Dolphin session inherits the previous match's stale pointers/flags
/// (initialized=true with dead GObj pointers, an assist_out/timer left
/// over from however the last match ended, etc.), which is exactly what
/// produced the "next match started with the assist moving around, then
/// randomly went invisible" symptom. Resets THIS team's state the moment
/// either PORT's gobj changes out from under it; converges correctly
/// regardless of which port's frame call notices first (see the inline
/// comments below).
///
/// Deliberately keyed on port_gobj[], not point/assist: point/assist are
/// current-ROLE labels a tag can swap at any time (TagAssist_TryTag), while
/// `roleIdx` here is always the fixed, CSS-time PORT role
/// (TagAssist_IsPortPoint), recomputed fresh every single frame from the
/// CSS's own team/point selection. Comparing that against point/assist
/// directly (an earlier version of this function did) meant a tag's own
/// swap looked identical to a new match starting -- port_gobj[roleIdx]
/// hadn't actually changed, but team->point/assist had, so every frame
/// immediately reset the whole team back to the CSS-original pairing,
/// silently undoing every tag the instant it happened. Confirmed root
/// cause of "tagging doesn't work" / "CPU keeps ending up controlling the
/// swapped-to character forever, can't tag back": port_gobj[] never moves
/// with a tag, so it can't ever be fooled by one.
static void TagAssist_HandleNewMatch(TeamState* team, int roleIdx,
                                     Fighter_GObj* gobj)
{
    int otherIdx = roleIdx ^ 1;
    if (team->port_gobj[roleIdx] != NULL && team->port_gobj[roleIdx] != gobj) {
        // If this ever fires mid-match (not right after a real match
        // start/reset), that's a real bug -- it means something made a
        // port's own gobj pointer look like it changed when it shouldn't
        // have, which silently wipes assist_out/point/assist. Loud on
        // purpose: this should be rare.
        OSReport("[TagAssist] NEW MATCH DETECTED for roleIdx=%d (was "
                 "port_gobj[%d]=%p, now gobj=%p) -- resetting team state\n",
                 roleIdx, roleIdx, (void*) team->port_gobj[roleIdx], (void*) gobj);
        team->initialized = false;
        team->benched_once = false;
        team->settle_timer = 0;
        team->assist_out = false;
        team->assist_timer = 0;
        team->point = NULL;
        team->assist = NULL;
        team->port_gobj[otherIdx] = NULL; // let the other port's own call re-set this
    }
    team->port_gobj[roleIdx] = gobj;
}

bool TagAssist_IsTagBattleOn(void)
{
    return sTagBattleOn;
}

void TagAssist_ToggleTagBattle(void)
{
    sTagBattleOn = !sTagBattleOn;
    if (!sTagBattleOn) {
        // Leaving Tag Battle: drop any explicit point choices so a later
        // re-toggle starts from the same lower-port-is-point default as a
        // fresh CSS entry, rather than resurrecting a stale choice from a
        // completely different set of doors/colors.
        sExplicitPointPort[0] = -1;
        sExplicitPointPort[1] = -1;
    }
}

void TagAssist_CssSyncPortTeam(int port, u8 team_color)
{
    sPortTeamColor[port] = team_color;
}

void TagAssist_SetExplicitPoint(u8 team_color, int port)
{
    if (team_color < 2) {
        sExplicitPointPort[team_color] = (s8) port;
    }
}

bool TagAssist_IsPortPoint(int port)
{
    u8 color = sPortTeamColor[port];
    s8 chosen;
    int i;

    if (color >= 2) {
        return false; // Green, or no color yet -- not a valid Tag Battle team
    }

    chosen = sExplicitPointPort[color];
    // Only honor an explicit choice while it still names a port actually on
    // this team -- if that port has since switched colors, fall through to
    // the default below instead of pointing at the wrong team's player.
    if (chosen >= 0 && sPortTeamColor[chosen] == color) {
        return chosen == port;
    }

    for (i = 0; i < 4; i++) {
        if (sPortTeamColor[i] == color) {
            return i == port; // lowest-numbered port on this team is default
        }
    }
    return false;
}

void TagAssist_OnFighterInputFrame(Fighter_GObj* gobj)
{
    Fighter* fp = GET_FIGHTER(gobj);
    u8 color;
    int teamIdx;
    int roleIdx;
    TeamState* team;

    if (!sTagBattleOn) {
        return; // untoggled: play as ordinary Melee, no benching/assist-calls
    }

    // Ice Climbers' Nana is not a separate assist-able role -- she's a
    // dependent sub-fighter of Popo and shares Popo's own fp->player_id
    // (retail's sub-fighter design: every one of Nana's own move-scripts
    // looks Popo up via Player_GetEntityAtIndex(nana_fp->player_id, 0),
    // e.g. ftnanaspecials.c). Without this check, Nana's Fighter_GObj hits
    // this same function every frame with the SAME team/role as Popo but a
    // DIFFERENT gobj, and TagAssist_HandleNewMatch's "did the stored
    // pointer change?" logic reads that as team->assist actually
    // switching fighters every frame -- thrashing initialized/benched_once
    // back to false continuously. Confirmed root cause of "Ice Climbers
    // assist spawns in as a normal, never-benched CPU": the benching path
    // below never gets to latch because initialized/benched_once are
    // reset before they ever settle.
    if (fp->is_sub_fighter) {
        return;
    }

    if (fp->player_id >= 4) {
        return; // only the first 4 ports are handled
    }
    color = sPortTeamColor[fp->player_id];
    if (color >= 2) {
        return; // not on Red or Blue -- no Tag Battle pairing for this port
    }
    teamIdx = color; // 0 = Red -> sTeams[0], 1 = Blue -> sTeams[1]
    roleIdx = TagAssist_IsPortPoint(fp->player_id) ? 0 : 1;
    team = &sTeams[teamIdx];
    TagAssist_HandleNewMatch(team, roleIdx, gobj);

    if (!team->initialized) {
        if (team->port_gobj[0] == NULL || team->port_gobj[1] == NULL) {
            return; // waiting on both point and assist to (re)spawn
        }
        team->initialized = true;
        team->point = team->port_gobj[0];
        team->assist = team->port_gobj[1];
        team->settle_timer = INITIAL_SETTLE_FRAMES;
        team->ready_frame = sFrameCounter + TAG_ASSIST_FIRST_CALL_GRACE_FRAMES;
        TagAssist_InitControlRoles(team);
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

    if (team->assist_out) {
        TagAssist_TryTag(team);
    } else {
        TagAssist_TryCallAssist(team);
    }
    TagAssist_UpdateTimer(team);
}

/// Advances sFrameCounter once per frame, unconditionally, every scene --
/// call once per frame from a scene-independent hook (see gmscene.c). Drives
/// TAG_ASSIST_FIRST_CALL_GRACE_FRAMES gating in TagAssist_TryCallAssist.
void TagAssist_Tick(void)
{
    sFrameCounter++;
}

void TagAssist_OnReset(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        sTeams[i].port_gobj[0] = NULL;
        sTeams[i].port_gobj[1] = NULL;
        sTeams[i].point = NULL;
        sTeams[i].assist = NULL;
        sTeams[i].initialized = false;
        sTeams[i].benched_once = false;
        sTeams[i].settle_timer = 0;
        sTeams[i].assist_out = false;
        sTeams[i].assist_timer = 0;
        sTeams[i].despawn_grace = 0;
    }
}

void TagAssist_RevertControlRolesForMatchEnd(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        TeamState* team = &sTeams[i];
        if (!team->initialized || !team->is_cpu_team) {
            continue;
        }
        // port_gobj[0]/[1] are the CSS-original point/assist identities --
        // always human/CPU respectively, never touched by any tag (see
        // TagAssist_HandleNewMatch's own comment) -- so handing them
        // straight to ApplyControlRoles as "new point"/"new assist"
        // restores exactly the pre-match-start pairing, regardless of how
        // many times team->point/assist have actually swapped since.
        TagAssist_ApplyControlRoles(team, team->port_gobj[0], team->port_gobj[1]);
    }
}

Gm_PKind TagAssist_GetOriginalPkindForMatchEnd(int player_id)
{
    int i;
    for (i = 0; i < 2; i++) {
        TeamState* team = &sTeams[i];
        if (!team->initialized || !team->is_cpu_team) {
            continue;
        }
        if (GET_FIGHTER(team->port_gobj[0])->player_id == player_id) {
            return Gm_PKind_Human;
        }
        if (GET_FIGHTER(team->port_gobj[1])->player_id == player_id) {
            return Gm_PKind_Cpu;
        }
    }
    return Gm_PKind_NA;
}
