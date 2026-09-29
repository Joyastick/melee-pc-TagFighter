#!/usr/bin/env python3
"""Group netplay test: 3 or 4 instances on one machine, a direct session, asserts.

    tools/net_group_test.py [--machines 4] [--frames N] [--minutes M] [--no-match]
                            [--key] [--loss PCT] [--delay MS] [--jitter] [--input-delay N]
                            [--exe build-pc/melee] [--disc tag_melee.iso] [--port 42100]

Machine i runs MELEE_NET=<every other machine, ascending> MELEE_NET_PLAYER=i
(net.c pc_net_connect_group), so machine 0 hosts and the rest are guests.
There is no lobby: the session agrees its own match (RULES/READY with each
guest, then GO, net_handshake.c), and by default no MELEE_NET_KEY is set so the
datagram keys under test are the ones the handshake derives, host-to-guest and
guest-to-guest. --key pins one instead (exercises the transport alone).

Two phases. Connect, handshake on all machines, run at the menus in lockstep
until MELEE_NET_EXIT_AFTER_FRAMES. Unless --no-match: also drive machine 0
(through MELEE_KEY_FIFO: a fifo on POSIX, a tailed plain file on Windows)
into a debug match with every machine a human (MELEE_DEBUG_VS_PLAYERS), keep
all of them moving, and require rollbacks to stay in sync. Pass = every
instance prints "net: test done", exits 0, no DESYNC / peer silent / lost
rollback, all report the same handshake seed and start frame, and every frame's
checksum agrees across instances from start_frame on. Exit code 0 on pass.
Logs: <work>/m0.log ... Reuses tools/net_test.py's helpers.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import net_test as nt  # noqa: E402
import net_pair_test as npt  # noqa: E402  (TagWorkout, check_tag)

BOOT_FRAMES = 2400  # the session is up from boot; the handshake needs the game's rules filled in


class Machine:
    """The bits of net_test.Instance the shared helpers read: name, proc,
    rec_path, text(), key()."""

    def __init__(self, i, n, exe, disc, work, base_port, env):
        self.name = f"m{i}"
        self.port = base_port + i
        self.log_path = os.path.join(work, f"{self.name}.log")
        self.rec_path = os.path.join(work, f"{self.name}.rec")
        # A fifo where there is one; elsewhere (Windows) a plain file the game
        # tails (src/pc/keyboard.c) and key() appends to.
        self.fifo = os.path.join(work, f"{self.name}.keys")
        if hasattr(os, "mkfifo"):
            os.mkfifo(self.fifo)
        else:
            open(self.fifo, "w").close()
        others = [f"127.0.0.1:{base_port + m}" for m in range(n) if m != i]
        e = dict(os.environ)
        for k in ("MELEE_NET", "MELEE_NET_PLAYER", "MELEE_NET_KEY", "MELEE_DEBUG_VS",
                  "MELEE_NET_REPLAY", "MELEE_LOG_FILE"):
            e.pop(k, None)
        e.update({
            "MELEE_VSYNC": "0",
            "MELEE_NET_PORT": str(self.port),
            "MELEE_CACHE_DIR": os.path.join(work, f"cache{i}"),
            "MELEE_WINDOW_TITLE": f"melee-group-{self.name}",
            "MELEE_SEED": "7",
            "MELEE_FPS": "1",
            "MELEE_NET_RECORD": self.rec_path,
            "MELEE_NET": ",".join(others),
            "MELEE_NET_PLAYER": str(i),
            "MELEE_DEBUG_VS": "1",
            "MELEE_DEBUG_VS_PLAYERS": str(n),
        })
        e["MELEE_KEY_FIFO"] = self.fifo
        if hasattr(os, "mkfifo"):
            e.setdefault("SDL_VIDEO_DRIVER", os.environ.get("SDL_VIDEODRIVER", "x11"))
        e.update(env)
        for k in [k for k, v in e.items() if v is None]:  # env value None = unset
            del e[k]
        os.makedirs(e["MELEE_CACHE_DIR"], exist_ok=True)
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([exe, "--no-card", disc], env=e, stdout=self.log,
                                     stderr=subprocess.STDOUT)

    def key(self, line):
        if hasattr(os, "mkfifo"):
            nt.fifo_write(self.fifo, line)
        else:
            with open(self.fifo, "a") as f:
                f.write(line + "\n")

    def text(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def kill(self, grace=0.0):
        deadline = time.time() + grace
        while self.proc.poll() is None and time.time() < deadline:
            time.sleep(0.25)
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()
        self.log.close()


# Instances share one stderr on Windows and a line can be cut by another's
# ("start_[FileCache] STORED"), so "done" is matched on its own and the numbers
# only where the line came through whole.
DONE_RX = re.compile(r"net: handshake done")
HANDSHAKE_RX = re.compile(r"net: handshake done seed=(\d+) start_frame=(-?\d+) \(frame")  # whole line only: a cut one has a short number
GROUP_RX = re.compile(r"net: group of (\d+) machines, we are machine (\d+)")


def check_group(ms, loss=0):
    """Cross-instance assertions: everyone joined the same group, agreed on
    the seed and start frame, and simulated identical frames."""
    fails = []
    n = len(ms)
    for i, m in enumerate(ms):
        g = GROUP_RX.findall(m.text())
        if n > 2 and g != [(str(n), str(i))]:
            fails.append(f"{m.name}: group line {g}, want [({n}, {i})]")
    # A refusal is a failure; a "done" line the game's own threads tore apart
    # is not (the checksum comparison below is the real proof it finished).
    for m in ms:
        if re.search(r"handshake timed out|refusing this session|READY rejected", m.text()):
            fails.append(f"{m.name}: the handshake failed")
    if fails:
        return fails
    done = [len(DONE_RX.findall(m.text())) for m in ms]
    if any(d > 1 for d in done):
        fails.append(f"handshake done more than once: {done}")
        return fails
    hs = [h for h in (HANDSHAKE_RX.findall(m.text()) for m in ms) if h]
    if len({h[0] for h in hs}) > 1:
        fails.append(f"machines disagree on (seed, start_frame): {[h[0] for h in hs]}")
        return fails
    start = int(hs[0][0][1]) if hs else 120
    cks = [nt.record_cks(m.rec_path) for m in ms]
    common = min(len(c) for c in cks)
    if common <= start:
        fails.append(f"only {common} recorded frames, handshake start_frame is {start}")
        return fails
    for f in range(start, common):
        if len({c[f] for c in cks}) != 1:
            fails.append(f"checksums differ at frame {f}: " +
                         " ".join(c[f].hex() for c in cks))
            break
    # a guest must have keyed every link: no bad-MAC drops after the handshake
    # (a handful at the start is the key handover: a datagram stamped before
    # its sender had the key can land after the receiver verified a tagged one)
    for m in ms:
        bad = max([int(x) for x in re.findall(r"bad_mac (\d+)", m.text())] or [0])
        if bad > 10 + 6 * loss:  # loss widens the key handover, which drops a few more at the start
            fails.append(f"{m.name}: {bad} datagrams dropped for a bad MAC")
    return fails


def css_walk(ms):
    """From the Tag Battle CSS (MELEE_BOOT_SCENE=meleevs): every machine picks on
    its own port (A), Start moves on to the SSS, every machine picks a stage
    (A), and the match follows. Each step repeats until the next scene's own
    archive shows up in every log, as net_test.press_until does."""
    for _ in range(12):  # pick (A), then Start once everyone has a character
        if nt.count(ms[0], nt.SCENE_FILE["sss"]) and all(nt.count(m, nt.SCENE_FILE["sss"]) for m in ms):
            break
        nt.press(ms, "X", 8, 1.5)
        nt.press(ms, "Return", 8, 1.5)
    else:
        return False
    fight = r"net: scene \d+ -> 2 at frame"  # into the fight (a fresh cache logs STORED, not HIT)
    for d in ("Right", "Down", "Left", "Up") * 6:  # sweep the cursor over the grid, A on each stop
        if all(nt.count(m, fight) for m in ms):
            return True
        nt.press(ms, d, 15, 0.3)
        nt.press(ms, "X", 8, 1.0)
    deadline = time.time() + 30
    while time.time() < deadline and not all(nt.count(m, fight) for m in ms):
        time.sleep(1)
    return all(nt.count(m, fight) for m in ms)


def run(args):
    n = args.machines
    if not 2 <= n <= 4:
        print("net_group_test: --machines is 2 to 4 (2 is the plain two-machine path, a baseline)")
        return False
    have_keys = not args.no_match
    frames = args.frames or (int(args.minutes * 3600) + BOOT_FRAMES + (3600 if have_keys else 0))
    sim = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames)}
    if args.key:
        sim["MELEE_NET_KEY"] = nt.NET_KEY
    if args.loss:
        sim["MELEE_NET_SIM_LOSS"] = str(args.loss)
    if args.delay:
        sim["MELEE_NET_SIM_DELAY_MS"] = str(args.delay)
    if args.jitter:
        sim["MELEE_NET_SIM_JITTER_MS"] = "20"
    if args.tag:  # a Tag Battle: 2v2 by machine (ports 0,1 vs 2,3), assists called and tagged
        sim["MELEE_DEBUG_VS"] = "tag2v2"
    if args.stocks:  # a stock match: someone loses the last stock and GAME! ends it
        sim["MELEE_DEBUG_VS_STOCKS"] = str(args.stocks)
    if args.css:  # the real menus instead of the debug match shortcut
        sim.update({"MELEE_DEBUG_VS": None, "MELEE_DEBUG_VS_PLAYERS": None,
                    "MELEE_BOOT_SCENE": "meleevs"})
    if args.input_delay is not None:
        sim["MELEE_NET_DELAY"] = str(args.input_delay)  # low: forces predictions, so rollbacks
    work = args.work or tempfile.mkdtemp(prefix="net_group_")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for i in range(n):
        if not nt.wait_port_free(args.port + i):
            return False
    ms = [Machine(i, n, args.exe, args.disc, work, args.port, sim) for i in range(n)]
    print(f"net_group_test: {n} machines, frames={frames}, keys={'pinned' if args.key else 'derived'}, "
          f"match={'yes' if have_keys else 'no (menus only)'}, pids={[m.proc.pid for m in ms]}, "
          f"logs={work}", flush=True)
    ok = True
    workout = None
    try:
        # Soft wait: the marker can come out mangled, and the checks after the
        # run say whether the handshake really finished.
        deadline = time.time() + 90
        while time.time() < deadline and not all(DONE_RX.search(m.text()) for m in ms):
            if any(m.proc.poll() is not None for m in ms):
                break
            time.sleep(1)
        if any(m.proc.poll() is not None for m in ms):
            print("net_group_test: a machine exited before the match", flush=True)
            ok = False
        elif have_keys and args.css:
            # No wait_match here: it wants a moving checksum, and an idle match has none
            # until the workout starts, so it would wait for itself.
            if not css_walk(ms):
                print("net_group_test: never got through CSS and SSS into the match", flush=True)
                ok = False
            else:
                workout = npt.TagWorkout(ms) if args.tag else nt.Workout(ms)
        elif have_keys:
            if not nt.press_until(ms, "Return", 9, nt.SCENE_FILE["match"], tries=40, each=8.0,
                                  on=(ms[0],)) or not nt.wait_match(ms, 150):
                print("net_group_test: never got into the match", flush=True)
                ok = False
            else:
                workout = npt.TagWorkout(ms) if args.tag else nt.Workout(ms)
        term = (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"net: disconnected", r"cannot roll back")
        deadline = time.time() + frames / 12.0 + 60
        last_frame, last_progress = -1, time.time()
        while ok and time.time() < deadline and any(m.proc.poll() is None for m in ms):
            time.sleep(1)
            texts = "".join(m.text() for m in ms)
            if any(re.search(t, texts) for t in term):
                break
            fr = max([int(x) for x in re.findall(r"net: frame (\d+)", texts)] or [-1])
            if fr > last_frame:
                last_frame, last_progress = fr, time.time()
            elif time.time() - last_progress > 25 and last_frame >= 0:
                print("net_group_test: no frame progress for 25 s, giving up", flush=True)
                break
    finally:
        print("net_group_test: at the end: " + ", ".join(
            f"{m.name} " + ("running" if m.proc.poll() is None else f"exited {m.proc.poll()}")
            for m in ms), flush=True)
        if workout is not None:
            print(f"net_group_test: workout wrote {workout.done()} key lines", flush=True)
        for m in ms:
            m.kill(20)
    fails = check_group(ms, args.loss) if ok else []
    if ok and args.tag:
        fails += npt.check_tag(ms)
    for m in ms:
        f, line, _ = nt.summarize(m, need_match=have_keys and ok)
        print(line)
        if f:
            ok = False
            print(f"[{m.name}] FAIL: " + ", ".join(f))
    if fails:
        ok = False
        for f in fails:
            print("net_group_test: FAIL: " + f)
    return ok


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--machines", type=int, default=4)
    p.add_argument("--frames", type=int, default=0, help="exit frame (default: boot + minutes)")
    p.add_argument("--minutes", type=float, default=0.5)
    p.add_argument("--no-match", action="store_true", help="menus only, no key driving")
    p.add_argument("--tag", action="store_true",
                   help="a Tag Battle with assist calls and tags (needs 3 or 4 machines: teams 0,1 vs 2,3)")
    p.add_argument("--stocks", type=int, default=0,
                   help="debug match with N stocks each, so it ends on GAME! (match-end path)")
    p.add_argument("--css", action="store_true",
                   help="walk the real Tag Battle CSS and SSS instead of the debug match")
    p.add_argument("--key", action="store_true", help="pin MELEE_NET_KEY instead of deriving keys")
    p.add_argument("--loss", type=int, default=0)
    p.add_argument("--delay", type=int, default=0)
    p.add_argument("--jitter", action="store_true")
    p.add_argument("--input-delay", type=int, default=None,
                   help="fixed input delay in frames instead of auto (1 forces rollbacks)")
    exe = os.path.join(root, "build-pc", "melee.exe" if os.name == "nt" else "melee")
    p.add_argument("--exe", default=exe)
    p.add_argument("--disc", default=os.path.join(root, "tag_melee.iso"))
    p.add_argument("--port", type=int, default=42100)
    p.add_argument("--work", default="")
    args = p.parse_args()
    ok = run(args)
    print("net_group_test: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
