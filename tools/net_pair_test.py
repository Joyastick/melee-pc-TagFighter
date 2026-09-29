#!/usr/bin/env python3
"""Two-machine netplay harness for Windows: two local instances, a direct session, asserts.

    tools/net_pair_test.py [--frames N] [--minutes M] [--no-match] [--loss PCT] [--delay MS]
                           [--jitter] [--input-delay N] [--exe build-pc/melee]
                           [--disc tag_melee.iso] [--port 42100]

Same checks as tools/net_test.py's direct mode, but keys reach the game through
a plain file it tails (src/pc/keyboard.c) where there is no fifo, so it runs on
Windows. Machine 0 is driven into the debug match (MELEE_DEBUG_VS=1); pass =
both print "net: test done", exit 0, no DESYNC / peer silent / lost rollback,
same handshake seed and start frame, and every frame's checksum agrees from
start_frame on. --input-delay 1 forces predictions, so rollbacks. Exit code 0
on pass. Logs: <work>/m0.log, m1.log. Reuses tools/net_test.py's helpers.
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
        })
        e["MELEE_KEY_FIFO"] = self.fifo
        if hasattr(os, "mkfifo"):
            e.setdefault("SDL_VIDEO_DRIVER", os.environ.get("SDL_VIDEODRIVER", "x11"))
        e.update(env)
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
HANDSHAKE_RX = re.compile(r"net: handshake done seed=(\d+) start_frame=(-?\d+)")


def check_group(ms):
    """Cross-instance assertions: everyone joined the same group, agreed on
    the seed and start frame, and simulated identical frames."""
    fails = []
    n = len(ms)
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
        if bad > 10:
            fails.append(f"{m.name}: {bad} datagrams dropped for a bad MAC")
    return fails


def run(args):
    n = 2
    have_keys = not args.no_match
    frames = args.frames or (int(args.minutes * 3600) + BOOT_FRAMES + (3600 if have_keys else 0))
    sim = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames)}
    sim["MELEE_NET_KEY"] = nt.NET_KEY  # a direct session pins its key
    if args.loss:
        sim["MELEE_NET_SIM_LOSS"] = str(args.loss)
    if args.delay:
        sim["MELEE_NET_SIM_DELAY_MS"] = str(args.delay)
    if args.jitter:
        sim["MELEE_NET_SIM_JITTER_MS"] = "20"
    if args.input_delay is not None:
        sim["MELEE_NET_DELAY"] = str(args.input_delay)  # low: forces predictions, so rollbacks
    work = args.work or tempfile.mkdtemp(prefix="net_group_")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for i in range(n):
        if not nt.wait_port_free(args.port + i):
            return False
    ms = [Machine(i, n, args.exe, args.disc, work, args.port, sim) for i in range(n)]
    print(f"net_pair_test: {n} machines, frames={frames}, "
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
            print("net_pair_test: a machine exited before the match", flush=True)
            ok = False
        elif have_keys:
            if not nt.press_until(ms, "Return", 9, nt.SCENE_FILE["match"], tries=40, each=8.0,
                                  on=(ms[0],)) or not nt.wait_match(ms, 150):
                print("net_pair_test: never got into the match", flush=True)
                ok = False
            else:
                workout = nt.Workout(ms)
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
                print("net_pair_test: no frame progress for 25 s, giving up", flush=True)
                break
    finally:
        if workout is not None:
            print(f"net_pair_test: workout wrote {workout.done()} key lines", flush=True)
        for m in ms:
            m.kill(20)
    fails = check_group(ms) if ok else []
    for m in ms:
        f, line, _ = nt.summarize(m, need_match=have_keys and ok)
        print(line)
        if f:
            ok = False
            print(f"[{m.name}] FAIL: " + ", ".join(f))
    if fails:
        ok = False
        for f in fails:
            print("net_pair_test: FAIL: " + f)
    return ok


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--frames", type=int, default=0, help="exit frame (default: boot + minutes)")
    p.add_argument("--minutes", type=float, default=0.5)
    p.add_argument("--no-match", action="store_true", help="menus only, no key driving")
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
    print("net_pair_test: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
