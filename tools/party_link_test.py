#!/usr/bin/env python3
"""Two game instances on this machine link as a party through the real menus.

    tools/party_link_test.py OUTDIR [--disc ISO] [--exe melee.exe]

Both walk MAIN MENU > MELEE VS > ONLINE > PARTY > PARTNER. The first hosts its
code, the second dials it (MELEE_DIRECT_TARGET), each with its own identity
key (MELEE_IDENTITY_DIR). Passes when both log "party linked", and then the
guest's POINT press reaches the host. It uses the real DHT and pairing server,
so it needs the internet and a visible desktop (the game windows must exist;
keys go in by MELEE_KEY_FIFO like tools/menu_shot.py). Logs are OUTDIR/host/
and OUTDIR/guest/game.log.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from menu_shot import Game, ROOT  # noqa: E402

TO_PARTNER = ["Up", "Return", "Down", "Return", "Down", "Down", "Return", "Up", "Return"]


def wait_for(game, text, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if text in game.text():
            return True
        if not game.alive():
            return False
        time.sleep(1)
    return False


def walk(game):
    for k in TO_PARTNER:
        game.key(k + "@100")
        time.sleep(2 if k == "Return" else 0.5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-pc", "melee.exe" if os.name == "nt" else "melee"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "tag_melee.iso"))
    args = ap.parse_args()
    dirs = {n: os.path.join(args.out, n) for n in ("host", "guest")}
    for d in dirs.values():
        os.makedirs(os.path.join(d, "id"), exist_ok=True)
    host = guest = None
    fails = []
    try:
        host = Game(dirs["host"], args.exe, args.disc, port="42301", extra_env={
            "MELEE_IDENTITY_DIR": os.path.join(dirs["host"], "id"), "MELEE_DIRECT_TARGET": ""})
        time.sleep(12)
        host.to_main_menu()
        walk(host)
        time.sleep(6)
        host.key("Return@100")  # START: host our code
        if not wait_for(host, "hosting as", 30):
            raise SystemExit("host never reached the code page")
        code = re.search(r"hosting as '?([A-Za-z0-9#]+)", host.text()).group(1)
        print("host code", code, flush=True)
        guest = Game(dirs["guest"], args.exe, args.disc, port="42302", extra_env={
            "MELEE_IDENTITY_DIR": os.path.join(dirs["guest"], "id"), "MELEE_DIRECT_TARGET": code})
        time.sleep(12)
        guest.to_main_menu()
        walk(guest)
        time.sleep(6)
        guest.key("Return@100")  # START: dial the code
        for name, g in (("host", host), ("guest", guest)):
            if not wait_for(g, "party linked", 240):
                fails.append(name + " never linked")
        if not fails:
            print("linked", flush=True)
            time.sleep(8)  # both scenes are back on the Party page
            for k in ("Down", "Down", "Down", "Return"):  # PARTNER > POINT, press it
                guest.key(k + "@100")
                time.sleep(1)
            if not wait_for(host, "party point is now machine 1", 30):
                fails.append("the host never saw the guest's point change")
    finally:
        for g in (host, guest):
            if g:
                g.close()
    for f in fails:
        print("party_link_test: FAIL:", f)
    print("party_link_test: " + ("FAIL" if fails else "PASS"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
