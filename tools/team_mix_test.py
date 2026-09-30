#!/usr/bin/env python3
"""A solo Matchmaking team against a linked party: three machines, one fight.

    tools/team_mix_test.py OUTDIR [--cache WARM_CACHE_DIR] [--disc ISO] [--exe melee.exe]

Three game instances on this machine, each with its own identity key: "p0" and
"p1" link as a party (tools/party_link_test.py), "s" searches Matchmaking with
the saved team (a fighter plus a CPU assist, or a couch player with a second
controller). Both searches meet, the two leaders accept, and all three must log
"team match, we are machine N of 3" and enter the fight. The solo machine must
hold two ports. Then all three pick SAME FIGHTER - REMATCH, which starts the
next fight in the same session. Needs the internet, a visible desktop and a
saved Matchmaking team and party fighter (set both in the menus once). Logs are
OUTDIR/<p0|p1|s>/game.log.
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from menu_shot import Game, ROOT  # noqa: E402
from party_link_test import wait_for, walk  # noqa: E402
from party_match_test import to_link, wait_for_count  # noqa: E402

PORTS = {"p0": "42331", "p1": "42332", "s": "42333"}


def start(out, exe, disc, name, target, cache):
    d = os.path.join(out, name)
    os.makedirs(os.path.join(d, "id"), exist_ok=True)
    return Game(d, exe, disc, port=PORTS[name], extra_env={
        "MELEE_IDENTITY_DIR": os.path.join(d, "id"), "MELEE_DIRECT_TARGET": target,
        "MELEE_PARTY_TEST_AFTER": "1"}, cache_seed=cache)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-pc", "melee.exe" if os.name == "nt" else "melee"))
    ap.add_argument("--disc", default=os.path.join(ROOT, "tag_melee.iso"))
    ap.add_argument("--cache", default=None)
    args = ap.parse_args()
    games, fails = {}, []
    try:
        games["p0"] = start(args.out, args.exe, args.disc, "p0", "", args.cache)
        to_link(games["p0"])
        if not wait_for(games["p0"], "hosting as", 40):
            raise SystemExit("p0 never reached the code page")
        code = re.search(r"hosting as '?([A-Za-z0-9#]+)", games["p0"].text()).group(1)
        games["p1"] = start(args.out, args.exe, args.disc, "p1", code, args.cache)
        to_link(games["p1"])
        for n in ("p0", "p1"):
            if not wait_for(games[n], "party linked", 240):
                raise SystemExit(n + " never linked")
        # the solo machine: MAIN MENU > MELEE VS > ONLINE > MATCHMAKING
        games["s"] = start(args.out, args.exe, args.disc, "s", "", args.cache)
        time.sleep(12)
        games["s"].to_main_menu()
        for k in ("Up", "Return", "Down", "Return"):
            games["s"].key(k + "@100")
            time.sleep(2 if k == "Return" else 0.5)
        time.sleep(6)
        time.sleep(8)  # the party's scenes are back on the Party page
        for k in ("Down", "Down", "Down", "Down", "Return"):  # MATCHMAKING / FIND MATCH
            games["s"].key(k + "@100")
            time.sleep(1)
        for k in ("Down", "Down", "Down", "Down", "Return"):
            games["p0"].key(k + "@100")
            time.sleep(1)
        for n in ("p0", "s"):
            if not wait_for(games[n], "recv valid MatchHello", 240):
                fails.append(n + " never found the other team")
        time.sleep(3)
        for n in ("p0", "s"):  # both leaders accept
            games[n].key("X@100")
        for n, g in games.items():
            if not wait_for(g, "team match, we are machine", 120):
                fails.append(n + " never connected")
        for n, g in games.items():
            if not wait_for(g, "entering party match", 120):
                fails.append(n + " never entered the fight")
        if not fails:
            line = re.findall(r"team match of (\d+) machines \(teams of (\d) and (\d)\), ports: (.*)",
                              games["s"].text())[-1]
            print("team match:", line, flush=True)
            if line[0] != "3" or sorted(line[1:3]) != ["1", "2"] or "cpu" not in line[3] and "human" not in line[3]:
                fails.append("not a 3 machine match: " + str(line))
            # All three pick SAME FIGHTER - REMATCH (the cursor starts on it).
            time.sleep(10)
            for g in games.values():
                g.key("X@100")
            for n, g in games.items():
                if not wait_for_count(g, "team match of", 2, 90):
                    fails.append(n + " never started the rematch fight")
    except SystemExit as e:
        fails.append(str(e))
    finally:
        for g in games.values():
            g.close()
    for f in fails:
        print("team_mix_test: FAIL:", f)
    print("team_mix_test: " + ("FAIL" if fails else "PASS"))
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
