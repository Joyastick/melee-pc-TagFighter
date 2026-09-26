#!/usr/bin/env python3
"""Two-process signed pairing/ranked regression with isolated profiles and UDP."""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from net_test_support import sdl_includes

ROOT = Path(__file__).resolve().parents[1]


def start_pairing_server(work):
    """Build and start server/pairing on loopback. Exits 77 (skipped) when
    there is no Go toolchain; CI's runners have one."""
    go = shutil.which("go")
    if not go:
        # CI installs Go (actions/setup-go), so there a missing one is a bug.
        assert not os.getenv("CI"), "no Go toolchain in CI"
        print("SKIP: no Go toolchain for the pairing server")
        sys.exit(77)
    binary = work / "pairing"
    subprocess.run([go, "build", "-o", str(binary), "."], cwd=ROOT / "server/pairing", check=True)
    key = work / "server.key"
    public = subprocess.run([str(binary), "-genkey", str(key)], check=True,
                            capture_output=True, text=True).stdout.strip()
    # The NAT check answers on port + 1, so find two free ports in a row.
    for _ in range(50):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as a,                 socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as b:
            a.bind(("127.0.0.1", 0))
            port = a.getsockname()[1]
            try:
                b.bind(("127.0.0.1", port + 1))
                break
            except OSError:
                continue
    server = subprocess.Popen([str(binary), "-key", str(key), "-listen", f"127.0.0.1:{port}",
                               "-listen2", f"127.0.0.1:{port + 1}"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    # Wait until it answers STATS.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.settimeout(0.2)
        for _ in range(50):
            sock.sendto(b"MPS1S", ("127.0.0.1", port))
            try:
                sock.recvfrom(256)
                break
            except socket.timeout:
                time.sleep(0.1)
        else:
            server.kill()
            raise AssertionError("pairing server did not start")
    return server, {"MELEE_PAIRING_SERVER": f"127.0.0.1:{port}", "MELEE_PAIRING_KEY": public,
                    "MATCH_NO_CANDIDATE": "1"}


def run():
    with tempfile.TemporaryDirectory(prefix="net-match-") as work:
        executable = Path(work) / "match"
        includes = [ROOT / "extern/aurora/include", ROOT / "src", ROOT / "src/sdk_include",
                    *sdl_includes(ROOT), ROOT / "extern/monocypher"]
        sources = [ROOT / "tools/test_net_match.c"]
        sources += [ROOT / f"src/pc/{name}.c" for name in (
            "net_identity", "net_rank", "net_rank_store", "net_rank_session",
            "libm/pc_rank_exp", "libm/pc_rank_sqrt")]
        sources += [ROOT / name for name in (
            "src/pc/net_rendezvous.c",
            "extern/dht/sha1.c", "extern/monocypher/monocypher.c",
            "extern/monocypher/monocypher-ed25519.c")]
        subprocess.run([
            "cc", "-std=gnu11", "-DTARGET_PC=1", "-DMELEE_PC=1", "-DAURORA", "-DPC_RDV_NO_THREAD",
            *[f"-I{path}" for path in includes], *map(str, sources),
            "-lm", "-o", str(executable)], check=True)

        sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
        try:
            for sock in sockets:
                sock.bind(("127.0.0.1", 0))
            ports = [sock.getsockname()[1] for sock in sockets]
        finally:
            for sock in sockets:
                sock.close()
        server = None
        server_env = {}
        if os.getenv("MATCH_PAIRING_SERVER"):
            server, server_env = start_pairing_server(Path(work))
        processes = []
        try:
            for player in range(2):
                profile = Path(work) / f"p{player}"
                profile.mkdir()
                env = os.environ | server_env | {
                    "MATCH_NAME": f"P{player}", "MATCH_DIR": str(profile)}
                processes.append(subprocess.Popen(
                    [executable, str(ports[player]), str(ports[1-player])],
                    env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE))
            output = [process.communicate(timeout=20) for process in processes]
            assert all(process.returncode == 0 for process in processes), output
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                process.wait()
            if server:
                server.kill()
                server.wait()

        if not any(os.getenv(name) for name in (
                "MATCH_PROOF_TIMEOUT", "MATCH_PROOF_MISMATCH", "MATCH_RECOVER", "MATCH_DECLINE")):
            rows = [item[0].strip().split() for item in output]
            assert {row[1] for row in rows} == {"0", "1"}, output
            assert len({row[2] for row in rows}) == 1, output
            # Same X25519 session secret on both sides (a fingerprint of it).
            assert len({row[3] for row in rows}) == 1, output
        profile = Path(work) / "cancel"
        profile.mkdir()
        subprocess.run([executable, "cancel"], check=True, timeout=20,
                       env=os.environ | {"MATCH_NAME": "CANCEL", "MATCH_DIR": str(profile)})

    if os.getenv("MATCH_DECLINE"):
        detail = "a declined opponent is dropped on both sides with no Offer sent"
    elif os.getenv("MATCH_RECOVER"):
        detail = "restart republishes durable immutable history and stale mutable state"
    elif os.getenv("MATCH_COMPLETE"):
        detail = "dual-signed durable set, immutable publication and mutable retry"
    elif os.getenv("MATCH_PROOF_TIMEOUT") or os.getenv("MATCH_PROOF_MISMATCH"):
        detail = "unverified peer state refused before socket handoff"
    elif os.getenv("MATCH_PAIRING_SERVER"):
        detail = "paired through the Go pairing server alone (no DHT candidates)"
    elif os.getenv("MATCH_RANKED"):
        detail = "BEP44 genesis proofs, signed ranked session and READY barrier"
    else:
        detail = ("signed pairing, X25519 session secret, socket handoff, role election "
                  "and READY barrier")
    print(f"PASS: {detail}; cancellation and same-key name refresh")


if __name__ == "__main__":
    run()
