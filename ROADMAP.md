# MeleeVS Roadmap

Where the MeleeVS Tag Battle mod is and where it's going. This covers the mod
only; the PC port underneath it (graphics, platforms, netcode internals) has
its own roadmap in [melee-pc](https://github.com/999sian/melee-pc).

## Shipped

- **Assist calls** for all 26 playable characters: D-Pad Down spawns your
  partner already performing a fixed per-character move, grounded or
  airborne. The move list is in the [README](README.md#character-assist-moves)
  and [docs/tag_assist_roster.csv](docs/tag_assist_roster.csv).
- **Active tag**: while the assist is out, D-Pad Down again swaps point
  between the two fighters, up to 3 times per call, each on a short cooldown.
- **Tag animation canceling**: the fighter you tag into drops whatever it was
  doing and hands you control at once, unless it's in hitstun, grabbed, or
  similarly stuck.
- **Solo and Duo Play**: a team is one human with a CPU assist, or two humans
  sharing the team on their own controllers.
- **Point elimination and stock sharing**: when point runs out of stocks, the
  assist is promoted instead of the team losing, and a survivor with spare
  stocks can donate one to revive the teammate.
- **Its own menu entry**: Main Menu → VS Mode → MELEE VS drops into a
  pre-paired 2v2 character select with a MeleeVS title and POINT markers.
- **Tag Bind setting**: optionally map one extra button (X, Y, L or R) to
  call/tag alongside D-Pad Down.
- **MeleeVS default rules**: Stock, 3 lives, items off, 8-minute timer.

## In progress

- **2-player online Tag Battle** with rollback netcode, over LAN, Direct
  Connect, and Unranked search. Tag Battle has its own Unranked pool, so it
  only ever matches other Tag Battle players. Works in two-machine testing;
  not yet in a release.

## Planned

- **4-player online Tag Battle** (Duo Play online, up to 4 players).
- **Dedicated aerial assists**: the aerial call currently uses the same move
  as the grounded one for every character. The lookup is already split so
  individual characters can get their own aerial move.
- **HUD and results feedback** for point promotions and stock-share revivals,
  which currently happen with no on-screen indication.
- **Cooldown between consecutive tags** (today only the call-to-first-tag gap
  is enforced).
- **Camera favoring the controlled fighter** while both teammates are on
  screen. Melee's own multi-fighter camera treats them equally.
- **A standalone game mode**: Tag Battle currently rides on VS Mode's scenes
  under the hood.

## Known issues

- Zelda/Sheik teams: a crash has been reported when Zelda is both point and
  assist and transforms into Sheik at match start, and control roles can
  occasionally go wrong after several tags on a transformed Zelda/Sheik.
  Still being investigated.

## Not planned for now

- **Choosing your assist move at call time** (for example picking a special
  or a smash per call). Each character keeps one fixed assist move.

Found a bug or have a request? Open an issue on
[this repo](https://github.com/Joyastick/melee-pc-TagFighter/issues) for
anything Tag Battle related, or on
[melee-pc](https://github.com/999sian/melee-pc/issues) for the port itself.
