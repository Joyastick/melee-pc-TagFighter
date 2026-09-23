# MeleeVS - Melee Tag Fighter

![Tag Fighter gameplay](docs/screenshots/tagfighter-demo.gif)
https://ko-fi.com/joyastick

A gameplay mod for Super Smash Bros. Melee
(NTSC-U 1.02) adding a 2-vs-2 tag mode: each human "point" fighter can call in
a partner "assist" fighter mid-match, similar to *Marvel vs. Capcom* switching
or *2XKO*-style assist calls, instead of Melee's normal
1-life-per-slot model. Each team can be **Solo Play** (one human point
character with a CPU assist) or **Duo Play** (two humans sharing a team, each
on their own controller). The mod lives almost entirely in
[`src/melee/mod/`](src/melee/mod), layered as a thin, largely non-matching
addition on top of the real, already-decompiled game code rather than a fork
of the engine itself.

MeleeVS is a fork of [999sian/melee-pc](https://github.com/999sian/melee-pc),
a native PC port of Melee built from the
[doldecomp/melee](https://github.com/doldecomp/melee) decompilation. **This
README only covers the MeleeVS mod.** For everything the port itself provides
(system requirements, building from source, the launcher and settings
overlay, keyboard/gamepad controls, HD textures, custom music, netplay
internals, troubleshooting, and the port's own roadmap), go to
**[melee-pc](https://github.com/999sian/melee-pc)** and its
[project site](https://999sian.github.io/melee-pc/). Bugs in the port itself
belong there; bugs in Tag Battle belong
[here](https://github.com/Joyastick/melee-pc-TagFighter/issues).

You need your own disc image. **No game data ships here.** Details under
[License](#license).

## MeleeVS Features and Roadmap

### It features: 
[x] Assists for every character - Press D-Pad Down to perform a predetermined assist move for each character

[x] Active Tag - When both characters are on the screen at the same time, press D-Pad Down to swap 'Point' between them

[x] Multiple 'Freestyle' tags - Ability to active tag 3 times per assist call

[x] 2XKO-Like Duos - Team up with your friends to create crazy combos

[x] Tag Animation Canceling - Cancel any animation the character was in (besides getting hit/grabbed) into full control of the character

### Planned Roadmap

The full list, including known issues, is in [ROADMAP.md](ROADMAP.md).

[] Early Fall 2026 - 2 Player Online Play (Direct Connect): Can connect to one other MeleeVS player with Rollback Netcode

[] Late Fall 2026 - 4 Player Online Play (Direct Connect): Can connect with up to 4 total MeleeVS players with Rollback Netcode

## Download

MeleeVS builds are on
[this fork's releases page](https://github.com/Joyastick/melee-pc-TagFighter/releases),
not upstream's (which only ships vanilla melee-pc). Grab the
`Melee-Windows-x86_64-vX.Y.Z.zip` asset from the latest release, unzip it, and
run `melee.exe`. You need a **Melee USA revision 2 (NTSC-U 1.02, GALE01)**
disc image. For building from source, see
[melee-pc's build docs](https://github.com/999sian/melee-pc/blob/master/docs/building.md);
the steps are the same for this fork.

## Playing MeleeVS

MeleeVS has its own entry in the main menu, so a normal VS Mode or Team
Battle match plays exactly like vanilla Melee and is unaffected by any of
this.

**Start a Tag Battle**: Main Menu → VS Mode → **MELEE VS** (the entry below
ONLINE). This drops straight into character select - its own two-tone
"MeleeVS" title replaces the usual mode banner - with all 4 doors already
open and paired up 2v2 - ports 1 and 3 on Red, ports 2 and 4 on Blue, each
Human if a controller is plugged into that port or CPU otherwise - so you can
go straight to picking characters instead of opening doors one at a time.
Switching to a different VS Mode entry (Melee, Tournament, Special Melee)
leaves Tag Battle behind; there's no in-CSS toggle for it anymore.

**Pick your team**: each door's team-color button cycles between Red and Blue
only (Green is unavailable - a 2v2 mode has no room for a third team), and a
door can't be closed either - its toggle just flips between CPU and Player
(Player only if a controller is on that port). Whoever picks Red plays
together, whoever picks Blue plays together - team pairing isn't tied to
which port you're in. A team is **Solo Play** if only one of its two doors is
human (the other stays CPU), or **Duo Play** if both are human, sharing the
team on their own controllers.

**Pick your point character**: press **Z** anywhere on a door's card. That
player becomes their team's point (human-controlled) character, their
teammate becomes the assist, and a colored **POINT** tag appears on their
door in their team's color. If nobody presses Z, the lower port number on
each team is point by default.

**Start**: requires exactly 2 players on Red and 2 on Blue.

- **Call an assist**: D-Pad Down. The assist spawns already performing its
  assigned move (grounded or airborne, depending on the point character's own
  state). While it's out, both fighters get a nametag showing everything you
  need to track the call at a glance:
  - **Point: N** over the active fighter, where `N` is how many more tags
    are still allowed this call (starts at 3, counts down as you use them).
  - A live countdown over the assist, showing how long until it auto-benches
    (shown as "..." instead if it's mid-hitstun/grabbed/etc. and the bench
    is briefly waiting that out).
- **Tag**: D-Pad Down again while the assist is out swaps which of the two is
  point. The fighter you tag into instantly cancels whatever it was doing and
  you get full control right away - unless it's genuinely unable to act
  (hitstun, grabbed, frozen, and other exotic "stuck" states), in which case
  it plays that out naturally first instead of handing you a free escape. Up
  to 3 tags are allowed per call, each on its own short cooldown, and tagging
  doesn't restart the assist's cameo timer.
- Ice Climbers bench/unbench Popo and Nana together.
- **Point runs out of stocks**: the assist is promoted to point instead of
  ending the team's run. If the sole survivor has more than one stock left,
  pressing **D-Pad Down** (the same assist-call input) donates one of their
  own stocks to revive the eliminated teammate back in as the assist.


### Character assist moves

In v1, the aerial assist call is the same move as the grounded one for every
character (tracked as a future refinement, not a limitation you need to work
around). Kept in sync with
[docs/tag_assist_roster.csv](docs/tag_assist_roster.csv) - update that file
first if a mapping below goes stale.

| Character | Grounded assist | Aerial assist |
|---|---|---|
| Mario | Neutral Special (Fireball) | Same as grounded |
| Dr. Mario | Down Special (Tornado) | Same as grounded |
| Fox | Up Smash | Same as grounded |
| Falco | Neutral Special (Blaster) | Same as grounded |
| Captain Falcon | Neutral Special (Falcon Punch) | Same as grounded |
| Ganondorf | Down Special (Wizard's Foot) | Same as grounded |
| Zelda | Up Smash | Same as grounded |
| Sheik | Up Smash | Same as grounded |
| Donkey Kong | Neutral Special (Giant Punch, released instantly uncharged) | Same as grounded |
| Bowser | Up Special (Whirling Fortress) | Same as grounded |
| Mr. Game & Watch | Neutral Special (Chef) | Same as grounded |
| Ice Climbers | Down Special (Blizzard) | Same as grounded |
| Luigi | Down Special (Luigi Cyclone) | Same as grounded |
| Marth | Neutral Special (Shield Breaker) | Same as grounded |
| Roy | Neutral Special (Shield Breaker) | Same as grounded |
| Yoshi | Up Special (Egg Throw) | Same as grounded |
| Mewtwo | Side Special (Confusion) | Same as grounded |
| Peach | Down Smash | Same as grounded |
| Samus | Side Special (Missile) | Same as grounded |
| Pikachu | Down Special (Thunder) | Same as grounded |
| Pichu | Neutral Special (Thunder Jolt) | Same as grounded |
| Jigglypuff | Side Special (Pound) | Same as grounded |
| Kirby | Side Special (Hammer Flip) | Same as grounded |
| Link | Up Special (Spin Attack) | Same as grounded |
| Young Link | Side Special (Boomerang) | Same as grounded |
| Ness | Side Special (PK Fire) | Same as grounded |

## Online Tag Battle (in progress)

Online Tag Battle is under active development: 2-player matches work over LAN,
Direct Connect, and Unranked search. Tag Battle searches use their own
Unranked pool, so you are only matched with other MeleeVS players in Tag
Battle, never with a plain VS search. Both players need the same MeleeVS
build and the same disc image. Ranked has no Tag Battle entry. For how
melee-pc's netplay works (ports, firewall, environment variables, test
harnesses), see [melee-pc](https://github.com/999sian/melee-pc).

## Contributing

The mod lives in [`src/melee/mod/`](src/melee/mod); the rest of the tree is
melee-pc and the decomp, and changes there belong upstream. See
[CODING_STYLE.md](CODING_STYLE.md) for the mod's conventions and the
pre-PR checklist.

## License

Three situations, spelled out in [LICENSE.md](LICENSE.md): the decompiled
game code in `src/melee` and `src/sysdolphin` is **not licensed** and remains
the property of its copyright holders; the port code in `src/pc`, `tools`,
`platforms`, `cmake` and `.github` is **GPL-3.0-or-later** ([COPYING](COPYING));
bundled third-party components keep their own licenses. Because the game code
cannot be relicensed, the repository as a whole is not distributable under the
GPL. No game assets are in this repository.
