#!/usr/bin/env python3
"""Drive the game through its menus and screenshot the steps.

    tools/menu_shot.py OUTDIR STEP [STEP ...]
    tools/menu_shot.py OUTDIR --tour        # every MELEE VS page, then checks

Steps (one argument each):
    wait:SECONDS         sleep
    menu                 press Start until the main menu has loaded (run it
                         after a boot wait, e.g. wait:12 menu)
    key:NAME@HOLD_MS     hold a key, e.g. key:Return@100 (Start / confirm),
                         key:X@100 (A), key:Z@100 (B), key:Up@100, key:Down@100
    shot:NAME            save OUTDIR/NAME.png (the whole primary screen)

Keys go in through MELEE_KEY_FIFO (src/pc/keyboard.c), so the game window does
not need focus; on Windows that is a plain file the game tails. Screenshots are
of the screen, so keep the game window visible and unobscured while it runs:
PowerShell + System.Drawing on Windows, ImageMagick `import` or `scrot` on
Linux. The game log is OUTDIR/game.log.

--tour walks MAIN MENU > MELEE VS (root, ONLINE, PARTY, CREDITS and back out),
saves a shot at each page, and fails if the game exited early, logged a fatal
error, or the page banners did not change (a title that never draws shows up
as identical shots). It is a smoke test for the menu code, not a pixel test.

Needs the disc image (default tag_melee.iso in the repo root) and a built game
(default build-pc/melee[.exe]).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PS_SHOT = r"""
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height
$g=[System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
$bmp.Save("%s")
"""


def screenshot(path):
    if os.name == "nt":
        subprocess.run(["powershell", "-NoProfile", "-Command", PS_SHOT % path], check=True)
    elif shutil.which("import"):
        subprocess.run(["import", "-window", "root", path], check=True)
    elif shutil.which("scrot"):
        subprocess.run(["scrot", "-o", path], check=True)
    else:
        raise RuntimeError("no screenshot tool: install ImageMagick (import) or scrot")


class Game:
    def __init__(self, out, exe, disc, extra_env=None, port="42200", cache_seed=None):
        os.makedirs(out, exist_ok=True)
        self.out = out
        self.keys = os.path.join(out, "keys.txt")
        self.log_path = os.path.join(out, "game.log")
        if hasattr(os, "mkfifo"):
            if os.path.exists(self.keys):
                os.remove(self.keys)
            os.mkfifo(self.keys)
        else:
            open(self.keys, "w").close()
        env = dict(os.environ)
        for k in ("MELEE_NET", "MELEE_DEBUG_VS", "MELEE_BOOT_SCENE"):
            env.pop(k, None)
        env.update({
            "MELEE_KEY_FIFO": self.keys,
            "MELEE_VSYNC": "0",
            "MELEE_CACHE_DIR": os.path.join(out, "cache"),
            "MELEE_WINDOW_TITLE": "menu_shot",
            "MELEE_NET_PORT": port,
        })
        env.update(extra_env or {})
        if cache_seed and os.path.isdir(cache_seed) and not os.path.isdir(env["MELEE_CACHE_DIR"]):
            # a cold shader-pipeline cache takes minutes to build and starves
            # every other instance on the machine; start from a warm one
            shutil.copytree(cache_seed, env["MELEE_CACHE_DIR"])
        os.makedirs(env["MELEE_CACHE_DIR"], exist_ok=True)
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([exe, "--no-card", disc], env=env, stdout=self.log,
                                     stderr=subprocess.STDOUT, cwd=os.path.dirname(exe))

    def text(self):
        with open(self.log_path, errors="replace") as f:
            return f.read()

    def key(self, spec):
        line = spec.replace("@", " ")
        if hasattr(os, "mkfifo"):
            fd = os.open(self.keys, os.O_WRONLY)  # blocks until the game reads
            with os.fdopen(fd, "w") as f:
                f.write(line + "\n")
        else:
            with open(self.keys, "a") as f:
                f.write(line + "\n")
        time.sleep(0.9)

    def to_main_menu(self, tries=60):
        """Start (also the card-message and title confirm) until MnMaAll loads."""
        for _ in range(tries):
            if "HIT: MnMaAll" in self.text():
                break
            self.key("Return@100")
            time.sleep(0.4)
        time.sleep(4)

    def shot(self, name):
        path = os.path.join(self.out, name + ".png")
        screenshot(path)
        print("shot", path, flush=True)
        return path

    def alive(self):
        return self.proc.poll() is None

    def close(self):
        if self.alive():
            self.proc.kill()
        self.proc.wait()
        self.log.close()


def run_steps(game, steps):
    for step in steps:
        kind, _, arg = step.partition(":")
        if kind == "wait":
            time.sleep(float(arg))
        elif kind == "menu":
            game.to_main_menu()
        elif kind == "key":
            game.key(arg)
        elif kind == "shot":
            game.shot(arg)
        else:
            raise SystemExit(f"menu_shot: unknown step {step!r}")
        if not game.alive():
            print("menu_shot: the game exited", game.proc.returncode, flush=True)
            return False
    return True


# MAIN MENU > MELEE VS. The cursor starts on 1-P Mode; one Up wraps to the last
# row, MELEE VS. Confirm is Start, back is B.
TOUR = [
    "wait:12", "menu",
    "key:Up@100", "wait:2", "shot:main",
    "key:Return@100", "wait:3", "shot:root",
    "key:Down@100", "wait:1", "key:Return@100", "wait:3", "shot:online",
    "key:Down@100", "wait:1", "key:Down@100", "wait:1", "key:Return@100", "wait:3", "shot:party",
    "key:Z@100", "wait:3", "key:Z@100", "wait:3", "shot:root_again",
    "key:Down@100", "wait:1", "key:Return@100", "wait:3", "shot:credits",  # backing out left ONLINE hovered
    "key:Z@100", "wait:3", "key:Z@100", "wait:3", "shot:main_again",
]


def check_tour(game, out):
    fails = []
    if not game.alive():
        fails.append(f"the game exited early ({game.proc.returncode})")
    if "[FATAL]" in game.text():
        fails.append("the log has a [FATAL] line")
    try:
        from PIL import Image, ImageChops, ImageStat
    except ImportError:
        print("menu_shot: Pillow missing, skipping the banner comparison", flush=True)
        return fails

    def banner(name):
        # the title strip, where the page name is drawn; the window sits at the
        # top left of the screen in these runs
        return Image.open(os.path.join(out, name + ".png")).convert("L").crop((180, 85, 600, 155))

    def differs(a, b):
        # mean grey-level difference: a changed title is 25+, the same title over
        # the animated background about 4
        return ImageStat.Stat(ImageChops.difference(banner(a), banner(b))).mean[0] > 12.0

    for a, b in (("main", "root"), ("root", "online"), ("online", "party"), ("party", "credits")):
        if not differs(a, b):
            fails.append(f"the banner did not change between {a} and {b}")
    if differs("main", "main_again"):
        fails.append("the main menu banner did not come back after leaving MELEE VS")
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("steps", nargs="*")
    ap.add_argument("--tour", action="store_true")
    exe = os.path.join(ROOT, "build-pc", "melee.exe" if os.name == "nt" else "melee")
    ap.add_argument("--exe", default=exe)
    ap.add_argument("--disc", default=os.path.join(ROOT, "tag_melee.iso"))
    args = ap.parse_args()
    if not args.tour and not args.steps:
        ap.error("give steps or --tour")
    game = Game(args.out, args.exe, args.disc)
    try:
        ok = run_steps(game, TOUR if args.tour else args.steps)
        if args.tour:
            fails = check_tour(game, args.out) if ok else ["the tour did not finish"]
            for f in fails:
                print("menu_shot: FAIL:", f)
            ok = ok and not fails
            print("menu_shot: " + ("PASS" if ok else "FAIL"))
    finally:
        game.close()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
