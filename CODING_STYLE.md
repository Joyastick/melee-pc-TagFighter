# MeleeVS Coding Style

Rules specific to the MeleeVS Tag Battle mod. Everything general (formatting,
64-bit portability, the big-endian data model, testing) follows
[melee-pc's coding style](https://github.com/999sian/melee-pc/blob/master/CODING_STYLE.md),
and `python3 tools/check_style.py` enforces it here too.

## Where code goes

- The mod lives in [`src/melee/mod/tag_assist.c`](src/melee/mod/tag_assist.c)
  and [`tag_assist.h`](src/melee/mod/tag_assist.h). New mod logic goes there.
- Everything else in the tree belongs to melee-pc or the decomp. Touch it only
  to add a **hook**: a single call into a `TagAssist_*` function, with a short
  comment saying what it's for. Keep the logic on the mod side of the call.
  Small hooks keep merges from upstream melee-pc painless.
- Fixes to melee-pc or decomp code that aren't about Tag Battle belong
  upstream, not here.

## Vanilla must stay vanilla

- Every hook must no-op when Tag Battle is off (`TagAssist_IsTagBattleOn()`).
  A normal VS match has to play exactly like unmodified melee-pc.
- Reuse the game's own code instead of copying it. An assist move is started
  by calling that move's own `_Enter` function. If the real entry point is
  file-private, mirror only the few lines needed, with a comment naming the
  original.

## Naming

- Public functions: `TagAssist_` prefix, declared in `tag_assist.h`.
- Internal helpers: `static`, same prefix.
- File-scope state: `static` with an `s` prefix (`sTeams`, `sTagBattleOn`).
- Tunables: `#define` in ALL_CAPS, with the unit in the name when it's a
  duration (`TAG_COOLDOWN_FRAMES`).

## Documenting

- Each public function gets a `///` comment in the header covering when to
  call it, from where, and what it returns when Tag Battle is off.
- Comments explain *why*, especially when working around retail behavior:
  what broke, and what the fix relies on.
- Debug output goes through `OSReport` with a `[TagAssist]` prefix.

## Characters with two fighters

Ice Climbers (Popo and Nana) and Zelda/Sheik each have two fighter objects.
Anything that benches, unbenches, or changes control roles must apply to both
halves (`TagAssist_GetIceClimberPartner`, `TagAssist_GetTransformPartner`).
Nana is a sub-fighter sharing Popo's `player_id`, so per-fighter hooks must
skip `fp->is_sub_fighter`.

## Online and rollback safety

Online matches re-simulate frames, and both machines must compute the same
result.

- Keep mod state in `static` storage. The rollback snapshot captures game
  statics, and a heap allocation would escape it.
- Gameplay decisions may only depend on synced inputs, synced rules, and game
  state. Never read a local setting live during a match. Settings that affect
  gameplay (like Tag Bind) are exchanged in the handshake and read from there.
- No wall-clock time and no non-game randomness in gameplay code.
- New state that changes the simulation should be added to the per-frame
  checksum in `src/pc/net_snapshot.c`, like point role and tags remaining, so
  a desync shows up on the frame it happens.

## Keep the assist chart in sync

A change to `TagAssist_GetAssistMoveEnter` or
`TagAssist_GetAssistMoveEnterAerial` must also update
[docs/tag_assist_roster.csv](docs/tag_assist_roster.csv) and the
[README's assist table](README.md#character-assist-moves).

## Before opening a pull request

1. Build the native PC target (see
   [melee-pc's build docs](https://github.com/999sian/melee-pc/blob/master/docs/building.md)).
2. Run `python3 tools/check_style.py`.
3. Play a Tag Battle match that exercises your change, and a plain VS match to
   confirm vanilla is unaffected.
4. For anything online-related, play a match between two instances over LAN
   and check the logs for `net: DESYNC`.
