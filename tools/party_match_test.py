#!/usr/bin/env python3
"""Two parties of two link, the leaders matchmake, and all four connect.

    tools/party_match_test.py OUTDIR [--disc ISO] [--exe melee.exe]

Four game instances on this machine, each with its own identity key
(MELEE_IDENTITY_DIR). Parties A and B link exactly as tools/party_link_test.py
does; then both leaders press FIND MATCH, accept the opponent, and the test
waits for all four to log "party match, we are machine N of 4" and to enter
the fight ("entering party match"). It uses the real DHT and pairing server,
so it needs the internet and a visible desktop; the game windows share the
screen, so keep them in view. Logs are OUTDIR/<a0|a1|b0|b1>/game.log.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from menu_shot import Game, ROOT  # noqa: E402
from party_link_test import wait_for, walk  # noqa: E402

PORTS = {"a0": "42311", "a1": "42312", "b0": "42313", "b1": "42314"}


def start(out, exe, disc, name, target, cache):
    d = os.path.join(out, name)
    os.makedirs(os.path.join(d, "id"), exist_ok=True)
    return Game(d, exe, disc, port=PORTS[name], extra_env={
        "MELEE_IDENTITY_DIR": os.path.join(d, "id"), "MELEE_DIRECT_TARGET": target,
        "MELEE_PARTY_TEST_AFTER": "1"},
        cache_seed=cache)


def to_link(game):
    time.sleep(12)
    game.to_main_menu()
    walk(game)
    time.sleep(6)
    game.key("Return@100")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-pc", "melee.exe" if os.name == "nt" else "melee"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "tag_melee.iso"))
    ap.add_argument("--cache", default=None,
                    help="a warm MELEE_CACHE_DIR to copy for each instance (a cold one takes minutes per instance)")
    args = ap.parse_args()
    games = {}
    fails = []
    try:
        codes = {}
        for name in ("a0", "b0"):  # the hosts first: their codes are what the others dial
            games[name] = start(args.out, args.exe, args.disc, name, "", args.cache)
            to_link(games[name])
            if not wait_for(games[name], "hosting as", 40):
                raise SystemExit(name + " never reached the code page")
            codes[name] = re.search(r"hosting as '?([A-Za-z0-9#]+)", games[name].text()).group(1)
            print(name, "code", codes[name], flush=True)
        for name, host in (("a1", "a0"), ("b1", "b0")):
            games[name] = start(args.out, args.exe, args.disc, name, codes[host], args.cache)
            to_link(games[name])
        for name, g in games.items():
            if not wait_for(g, "party linked", 240):
                fails.append(name + " never linked")
        if fails:
            raise SystemExit("linking failed")
        print("both parties linked", flush=True)
        time.sleep(8)  # every scene is back on its Party page
        for name in ("a0", "b0"):  # PARTNER > FIND MATCH, press it
            for k in ("Down", "Down", "Down", "Down", "Return"):
                games[name].key(k + "@100")
                time.sleep(1)
        for name in ("a0", "b0"):
            if not wait_for(games[name], "recv valid MatchHello", 180):
                fails.append(name + " never found the other party")
        time.sleep(3)
        for name in ("a0", "b0"):  # A accepts (X)
            games[name].key("X@100")
        for name, g in games.items():
            if not wait_for(g, "party match, we are machine", 120):
                fails.append(name + " never connected")
        for name, g in games.items():
            if not wait_for(g, "entering party match", 120):
                fails.append(name + " never entered the fight")
        if not fails:
            # The fight is skipped (MELEE_PARTY_TEST_AFTER): all four are in the
            # after-match lobby. One machine picks BACK TO PARTY, which ends it
            # for all four (keys sent to the others would land on the Party page).
            time.sleep(10)
            games["a0"].key("Down@100")
            time.sleep(0.5)
            games["a0"].key("X@100")
            for name, g in games.items():
                if not wait_for(g, "after match: party leaves", 60):
                    fails.append(name + " never left the after-match lobby")
            for name, g in games.items():  # back on the Party page, teammate linked again
                end = time.time() + 90
                while time.time() < end and g.text().count("party linked, we are machine") < 2:
                    time.sleep(1)
                if g.text().count("party linked, we are machine") < 2:
                    fails.append(name + " never re-linked with its teammate")
            time.sleep(30)  # past the link's 20 s silence limit: it must be alive
            for name, g in games.items():
                if "partner went silent" in g.text():
                    fails.append(name + " lost its teammate after the match")
    except SystemExit as e:
        fails.append(str(e))
    finally:
        for g in games.values():
            g.close()
    for f in fails:
        print("party_match_test: FAIL:", f)
    print("party_match_test: " + ("FAIL" if fails else "PASS"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
