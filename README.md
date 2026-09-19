# MeleeXKO - Melee Tag Fighter

**Now in Beta!** A gameplay mod for Super Smash Bros. Melee
(NTSC-U 1.02) adding a 2-vs-2 tag mode: each human "point" fighter can call in
a partner "assist" fighter mid-match, similar to *Marvel vs. Capcom* switching
or *2XKO*-style assist calls, instead of Melee's normal
1-life-per-slot model. Each team can be **Solo Play** (one human point
character with a CPU assist) or **Duo Play** (two humans sharing a team, each
on their own controller). The mod lives almost entirely in
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

You need your own disc image. **No game data ships here.** The decompiled game
code is not licensed and is not relicensed by this project; only the port code
is GPL-3.0-or-later. Details under [License](#license).

> New here? The **[project site](https://999sian.github.io/melee-pc/)** has the
> five-step setup, the FAQ (supported disc, Windows first run, older Intel GPUs,
> first-use shader stutter, Android requirements, where the log and settings
> live) and the per-platform known-issues list. Bugs go through the
> [bug report form](https://github.com/999sian/melee-pc/issues/new?template=bug_report.yml);
> questions on [Discord](https://discord.gg/aurt34svq).

![Tag Fighter gameplay](docs/screenshots/tagfighter-demo.gif)

## Tag Fighter roadmap

### Completed

- [x] Grounded assist calls, implemented for every character.
- [x] Air assist calls, implemented for every character.
- [x] Tag Battle toggle in the character-select rules screen -- off by
  default, so vanilla Melee (including normal Team Battle) is completely
  unaffected until it's turned on.
- [x] Player-driven Red/Blue team pairing and point-character selection,
  replacing the old fixed Port1+3/Port2+4 layout.
- [x] Tagging between characters (swapping which fighter is point mid-match,
  not just a timed assist call) -- including control handoff for a Solo
  Play (human+CPU) team, so the human always ends up playing whichever
  fighter is currently point.
- [x] Point-elimination promotion: if point runs out of stocks, the assist
  is promoted to point instead of softlocking the team, with a manual
  stock-share revival to bring a permanently-eliminated teammate back once
  the survivor has a spare stock.
- [x] Duo Play (human+human team) confirmed working -- the same
  team-pairing and tag mechanic Solo Play uses covers two humans sharing a
  team with no CPU-specific control-handoff code path involved. The assist
  cameo now runs 4 seconds (up from 3) and allows up to 3 tags per call
  (each on its own 20-frame cooldown) instead of one tag ending the cameo.
- [x] Solo Play's CPU assist behaves passively (idle, no attacking, but
  still works its way back toward the stage if knocked off) for the whole
  time it's out on either a tag or a plain call, instead of fighting/
  taunting like a genuine opponent or, on a plain call, having no AI at
  all after its scripted move finished.
- [x] Dedicated **TAG BATTLE** entry in the main menu's VS submenu, replacing
  the old CSS-only Z+A hotkey -- jumps straight into character select with
  all 4 doors already open and paired 2v2 (Red/Blue by port, Human or CPU
  by controller presence), doors locked to CPU/Player so the mode can't
  drop below 4 players, and the VS Mode/Team Battle/Tag Battle switch
  locked for the rest of that CSS session.

### Planned

- [ ] Dedicated Tag Battle art in the character-select screen itself
  (currently reuses Team Battle's own banner and team-color icons, since
  there's no Tag Battle-specific CSS art yet -- only the main menu entry
  has its own label).
- [ ] Online play, built on melee-pc's own online implementation once that
  lands upstream.

The phases behind the planned rows, and why they are ordered that way, are in
[ROADMAP.md](ROADMAP.md).

## Download

Builds for every platform are on the
[releases page](https://github.com/999sian/melee-pc/releases). Release notes
list the per-platform files, known issues and requirements.

```sh
./Melee-x86_64.AppImage                  # open the launcher
./Melee-x86_64.AppImage /path/to/melee.iso
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

**Melee USA revision 2 (NTSC-U 1.02, GALE01)** is the supported disc. A
**Europe (PAL, GALP01)** image also boots (experimental, see
[porting-notes.md](docs/porting-notes.md#regions) upstream), but the mod is
only verified against NTSC-U. A valid disc path on the command line boots
straight in; a missing or invalid one returns to the launcher. Settings and
the selected path live in `launcher.cfg` in SDL's `melee-pc_TagFighter`
preference directory (usually `~/.local/share/melee-pc_TagFighter`) —
namespaced separately from a vanilla melee-pc install so the two don't
collide.

Verification reads the disc through nod, compressed images included, and compares
SHA-1 against the
[Redump DAT](https://github.com/libretro/libretro-database/blob/master/metadat/redump/Nintendo%20-%20GameCube.dat):
`d4e70c064cc714ba8400a849cf299dbd1aa326fc`, 1,459,978,240 bytes. It supports
progress and cancellation, and is not cached between launches. Unverified images
still play; PAL images have no reference hash and always report as unverified.

Building from source: [docs/building.md](docs/building.md).

## Requirements

The renderer is WebGPU (Dawn) at its compatibility level, so the floor is
Dawn's per-backend floor:

| Platform | API tried, in order | Floor |
|---|---|---|
| Windows 10/11 (x86-64, ARM64) | Direct3D 12 → Direct3D 11 → Vulkan | Feature level 11_0. Dawn refuses D3D12 on Intel Gen7 (HD 4000/4400/4600, Ivy Bridge/Haswell); the intended fallback for those is Direct3D 11, which is untested on that hardware (see the status table). Vulkan 1.1 with a vendor ICD. |
| Linux (x86-64, aarch64) | Vulkan | Vulkan 1.1 (Mesa radv/anv/hasvk, NVIDIA proprietary or NVK). |
| macOS / iOS | Metal | Any Metal GPU; Apple Silicon tested, iOS 14+. |
| Android | Vulkan | Vulkan 1.1, arm64. |

On Windows that means any Intel Gen8 (Broadwell, 2014) or newer, AMD GCN or
newer, NVIDIA Fermi or newer runs on Direct3D 12. Direct3D 11 is a
compatibility path, not a performance one (FXC shaders, no DXC). OpenGL is not
built. The log records every backend that was skipped and why, then one summary
line with the adapter and driver.

- Keep `resources/` (and on Windows the DLLs: `webgpu_dawn.dll`,
  `dxcompiler.dll`, `dxil.dll`, `SDL3.dll`, the VC++ runtime) beside the
  executable. `dxcompiler.dll` and `dxil.dll` are the D3D12 shader compiler;
  D3D11 needs no extra DLL, since `d3d11.dll`, `dxgi.dll` and the FXC
  compiler are Windows components.
- Settings, memory cards, `music/` and `textures/` live in the `melee-pc`
  preference directory above.

## Playing Tag Fighter

Tag Battle has its own entry in the main menu, so a normal VS Mode or Team
Battle match plays exactly like vanilla Melee and is unaffected by any of
this.

**Start a Tag Battle**: Main Menu → VS Mode → **TAG BATTLE** (the entry below
Name Entry). This drops straight into character select with all 4 doors
already open and paired up 2v2 — ports 1 and 3 on Red, ports 2 and 4 on Blue,
each Human if a controller is plugged into that port or CPU otherwise — so
you can go straight to picking characters instead of opening doors one at a
time. Switching to a different VS Mode entry (Melee, Tournament, Special
Melee) leaves Tag Battle behind; there's no in-CSS toggle for it anymore.

**Pick your team**: each door's team-color button cycles between Red and Blue
only (Green is unavailable — a 2v2 mode has no room for a third team), and a
door can't be closed either — its toggle just flips between CPU and Player
(Player only if a controller is on that port). Whoever picks Red plays
together, whoever picks Blue plays together — team pairing isn't tied to
which port you're in. A team is **Solo Play** if only one of its two doors is
human (the other stays CPU), or **Duo Play** if both are human, sharing the
team on their own controllers.

**Pick your point character**: hover a door's team-color button and press
**Z**. That player becomes their team's point (human-controlled) character,
their teammate becomes the assist, and their team icon starts flashing. If
nobody presses Z, the lower port number on each team is point by default.

**Start**: requires exactly 2 players on Red and 2 on Blue.

- **Call an assist**: D-Pad Down. The assist spawns already performing its
  assigned move (grounded or airborne, depending on the point character's own
  state), then despawns after a few seconds or immediately if it's KO'd while
  out.
- Ice Climbers bench/unbench Popo and Nana together.
- **Point runs out of stocks**: the assist is promoted to point instead of
  ending the team's run. If the sole survivor has more than one stock left,
  pressing **D-Pad Down** (the same assist-call input) donates one of their
  own stocks to revive the eliminated teammate back in as the assist.

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

Keyboard: arrows or WASD = stick, IJKL = C-stick, X = A, Z = B, C = X, V = Y,
Q/E = L/R, Tab = Z, Enter = Start, TFGH = D-pad. Gamepads work through SDL; an
official GameCube adapter is read directly instead (see the status table).

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
| `MELEE_BACKEND=<name>` | Pin the graphics backend (`vulkan`, `d3d12`, `d3d11`, `metal`, ...) instead of the platform's preferred order; an unknown name lists the valid ones. |
| `MELEE_VSYNC=0\|1` | Override the saved VSync preference. |
| `MELEE_LOG_FILE=<path>` | Write the log to a file (default `melee-pc.log` beside `melee.exe` on Windows; empty disables). |
| `MELEE_WINDOW_TITLE=<t>` | Window title. |
| `MELEE_FILES_DIR=<dir>` | Loose-file overlay: files here (or in `./files/`) replace the disc's. |
| `MELEE_CACHE_MAX_MB=<n>` | In-memory archive cache budget (default picked from installed RAM). |
| `MELEE_PREWARM=0` | Skip the background asset pre-warm after boot. |
| `MELEE_FAST_FADES=1` | Clamp scene fade delays. |
| `MELEE_PIPELINE_JOBS=<n>` | Background shader-pipeline compile threads (default half the hardware threads, 1..8). |
| `MELEE_UCF=1` | Universal Controller Fix (UCF 0.8x dashback and shield-drop rules); overrides the `ucf` launcher.cfg pref. |
| `MELEE_GC_ADAPTER=0` | Hand the GameCube adapter (WUP-028) back to SDL's gamepad driver instead of reading it raw. |
| `--no-card` | Boot without a memory card. |
| `--dvd <image>` | Explicit form of the positional disc argument. |
| `--version` | Print the build version and exit. |

Diagnostics are off by default and cost nothing when unset. They measure or
suppress only; none of them fixes anything. Diagnostic knobs (`MELEE_DEBUG`,
`MELEE_FPS`, `MELEE_HEAP_CHECK`, the `AURORA_*` draw filters, ...) are listed
in [docs/debugging.md](docs/debugging.md#diagnostic-environment-variables).

## Contributing & Coding Style

Please refer to [CODING_STYLE.md](CODING_STYLE.md) for architectural guidelines,
formatting standards, 64-bit portability rules, and verification procedures. Run
`python3 tools/check_style.py` before opening pull requests.

## Layout

- `src/melee/mod` - the Tag Fighter mod itself.
- `src/melee`, `src/sysdolphin` - game code from the decomp (upstream commit in
  `src/UPSTREAM_COMMIT`), adapted to the PC data model.
- `src/pc` - platform layer: main, OS/VI/GX glue, keyboard, audio mixer, THP,
  vertex-array sizing.
- `extern/aurora` - vendored aurora with local changes.

## Documentation

- [docs/building.md](docs/building.md) - toolchain, packaging, cross-compiling
  for Windows, Android, iOS and macOS.
- [docs/testing.md](docs/testing.md) - unit tests, drive/capture tools,
  port-bug harnesses.
- [docs/debugging.md](docs/debugging.md) - log files, crash handler, gdb, heap
  check, diagnostic environment variables.
- [docs/porting-notes.md](docs/porting-notes.md) - the big-endian data model,
  LP64 bug classes, PAL support.
- [docs/architecture.md](docs/architecture.md) - layers, threads, memory map,
  aurora.
- [ROADMAP.md](ROADMAP.md) - scope and sequencing of melee-pc's own remaining
  phases (not the Tag Fighter roadmap above).

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
