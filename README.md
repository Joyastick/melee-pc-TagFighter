# MeleeXKO - Melee Tag Fighter

**Alpha, for testing only.** A gameplay mod for Super Smash Bros. Melee
(NTSC-U 1.02) adding a 2-vs-2 tag mode: each human "point" fighter can call in
a partner "assist" fighter mid-match, similar to *Marvel vs. Capcom* switching
or *2XKO*-style assist calls, instead of Melee's normal
1-life-per-slot model. The mod lives almost entirely in
[`src/melee/mod/`](src/melee/mod), layered as a thin, largely non-matching
addition on top of the real, already-decompiled game code rather than a fork
of the engine itself.

This repository is a fork of
[999sian/melee-pc](https://github.com/999sian/melee-pc), a native PC port of
Melee built from [doldecomp/melee](https://github.com/doldecomp/melee) on top
of [aurora](https://github.com/encounter/aurora) (GX/OS/PAD/DVD/CARD/THP
compatibility layer with a WebGPU backend) and SDL3. Everything melee-pc
provides — the native build, launcher, settings overlay, HD texture packs,
custom soundtrack streaming, and so on — still applies here; this fork adds
the Tag Fighter mod on top of it. For melee-pc's own feature list, porting
notes, and contributing guide, see
[the upstream README](https://github.com/999sian/melee-pc/blob/master/README.md).

You need your own disc image. No game data ships here.

![Tag Fighter gameplay](docs/screenshots/tagfighter-demo.gif)

## Tag Fighter roadmap

### Completed

- [x] Grounded assist calls, implemented for every character.
- [x] Air assist calls, implemented for every character.

### Planned

- [ ] Tagging between characters (swapping which fighter is point mid-match,
  not just a timed assist call).
- [ ] Duo play support (shared-team play in the style of *2XKO*).
- [ ] A separate Tag mode selectable alongside vanilla Melee, so the base
  game remains playable without any tag-mod behavior enabled.
- [ ] Online play, built on melee-pc's own online implementation once that
  lands upstream.

## Building

Needs GCC (the game code relies on `scalar_storage_order("big-endian")`, which
only GCC implements), CMake 3.25+, Ninja, and a Vulkan driver. Aurora fetches
its own Dawn/SDL3/nod prebuilts.

```sh
cmake -B build -G Ninja
ninja -C build
```

No disc data is needed to build. The two HSD font atlases are pixel data from
the retail DOL, so instead of being committed they are read out of the disc
you supply, at boot (`src/pc/discfont.c`).

The release artifacts are produced by the same scripts CI runs, so they work
locally too. Windows cross-compiles from Linux with MinGW-w64; Android needs an
NDK (`ANDROID_NDK_HOME`) and a JDK 17.

```sh
tools/package_linux.sh      # dist/Melee-x86_64.AppImage + tarball
tools/package_windows.sh    # dist/Melee-Windows-x86_64.zip
tools/build_android.sh      # dist/Melee-Android-arm64.apk (signed release)
```

## Running

```sh
build/melee                              # open the launcher
build/melee <disc.iso|.gcm|.ciso|.rvz>
```

Only **Melee USA revision 2 (NTSC-U 1.02, GALE01)** is supported. A valid disc
path on the command line boots straight in; a missing or invalid one returns to
the launcher. Settings and the selected path live in `launcher.cfg` in SDL's
`melee-pc_TagFighter` preference directory (usually
`~/.local/share/melee-pc_TagFighter`) — namespaced separately from a vanilla
melee-pc install so the two don't collide.

Verification reads the disc through nod, compressed images included, and compares
SHA-1 against the
[Redump DAT](https://github.com/libretro/libretro-database/blob/master/metadat/redump/Nintendo%20-%20GameCube.dat):
`d4e70c064cc714ba8400a849cf299dbd1aa326fc`, 1,459,978,240 bytes. It supports
progress and cancellation, and is not cached between launches. Unverified images
still play.

Keep `resources/` next to the binary when distributing. The bundled Liberation
Sans fonts are covered by `resources/FONT-LICENSE.txt`.

## Playing Tag Fighter

Set up a 4-player VS match: Port 1 + Port 3 are Team A (point + assist),
Port 2 + Port 4 are Team B. Set each slot's assist port to CPU on the
character-select screen — the mod takes it over (benched, no CPU AI) once the
match starts.

- **Call an assist**: D-Pad Down. The assist spawns already performing its
  assigned move (grounded or airborne, depending on the point character's own
  state), then despawns after a few seconds or immediately if it's KO'd while
  out.
- Ice Climbers bench/unbench Popo and Nana together.

### Character assist moves

In v1, the aerial assist call is the same move as the grounded one for every
character (tracked as a future refinement, not a limitation you need to work
around). Kept in sync with
[docs/tag_assist_roster.csv](docs/tag_assist_roster.csv) — update that file
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
| Roy | Up Special (Blazer) | Same as grounded |
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

## Controls

Keyboard: arrows = stick, IJKL = C-stick, X = A, Z = B, C = X, V = Y, Q/E = L/R,
Tab = Z, Enter = Start, TFGH = D-pad. Gamepads work through SDL.

| | Keyboard | Gamepad |
|---|---|---|
| Navigate | Up/Down, Tab | D-pad or left stick |
| Adjust | Left/Right | D-pad left/right |
| Change tab | Left/Right on the tab strip | L/R shoulders |
| Select | Enter | A |
| Close overlay | Escape, F1 | B, Start, Back |

## Settings overlay

**F1**, or Back/Select on a gamepad, opens the overlay. The game pauses while it
is open.

- Display: fullscreen/windowed and VSync apply immediately. `MELEE_VSYNC`
  overrides the saved preference.
- Internal resolution and UI scale are sliders. UI scale covers 75% to 150%.
- Post-processing picks the presentation shader and applies immediately.
- Anti-aliasing and anisotropic filtering apply on the next launch. MSAA offers
  only off and 4x because WebGPU guarantees sample counts 1 and 4.
- Audio: master volume, mute, FPS counter, all immediate.
- Controls remaps a gamepad. Pick the port, select a GameCube button, then press
  the physical button. Escape cancels, Restore resets the port. Back cannot be
  bound since it opens the menu. Sticks and triggers remap the same way, and a
  direction accepts either a stick axis or a button.

Melee's own menu sounds play in the overlay. Bindings are stored in aurora's
per-device `.controller` files; everything else shares `launcher.cfg`.

## Environment variables

| Variable | Effect |
|---|---|
| `MELEE_SEED=<n>` | Deterministic RNG for the attract demo. |
| `MELEE_HEAP_CHECK=1` | Canaries on every heap allocation, checked each frame; aborts at the first stomp. |
| `MELEE_FPS=1` | Print frame rate once a second. |
| `MELEE_AUDIO_DUMP=<file>` | Also write the mix as raw f32 stereo 32 kHz. |
| `MELEE_WINDOW_TITLE=<t>` | Window title. |
| `--no-card` | Boot without a memory card. |
| `--dvd <image>` | Explicit form of the positional disc argument. |

Diagnostics are off by default and cost nothing when unset. They measure or
suppress only; none of them fixes anything. See
[the upstream README](https://github.com/999sian/melee-pc/blob/master/README.md#environment-variables)
for the full rendering/audio diagnostic variable list.

## Layout

- `src/melee/mod` - the Tag Fighter mod itself.
- `src/melee`, `src/sysdolphin` - game code from the decomp (upstream commit in
  `src/UPSTREAM_COMMIT`), adapted to the PC data model.
- `src/pc` - platform layer: main, OS/VI/GX glue, keyboard, audio mixer, THP,
  vertex-array sizing.
- `extern/aurora` - vendored aurora with local changes.

For decomp porting notes, dev tools, and coding-style/contributing
guidelines, see
[the upstream README](https://github.com/999sian/melee-pc/blob/master/README.md)
and [CODING_STYLE.md](CODING_STYLE.md).

## License

Three situations, spelled out in [LICENSE.md](LICENSE.md): the decompiled
game code in `src/melee` and `src/sysdolphin` is **not licensed** and remains
the property of its copyright holders; the port code in `src/pc`, `tools`,
`platforms`, `cmake` and `.github` is **GPL-3.0-or-later** ([COPYING](COPYING));
bundled third-party components keep their own licenses. Because the game code
cannot be relicensed, the repository as a whole is not distributable under the
GPL. No game assets are in this repository.
