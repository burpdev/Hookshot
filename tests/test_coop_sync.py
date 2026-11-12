"""host/coop_state.c, co-op's progress merge engine, between simulated games on a simulated network
(tests/coop_sync_test.c, with host/coop_proto.c and Monocypher): merging in any order and under loss, late joiners,
the room's rules and the pools, nothing carried from one room or save to the next, hostile members and an attacker who
holds the room's secret (refused values, forged sections, records and rosters, replays, removals, resets), identities
and acceptance by key, and a randomised run against a ledger the test keeps itself. No game, socket or clock: a minute
of play takes milliseconds. On Linux with AddressSanitizer (leaks too) and UBSan when the compiler has them.

    python -I -B tests/test_coop_sync.py [--cc gcc]
    COOP_ADVERSARY=<first>:<count> python -I -B tests/test_coop_sync.py   only those seeds of the randomised run
                                                                         (COOP_ADVERSARY_TRACE=1: its events)
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
MONOCYPHER = ROOT / "third_party" / "monocypher"


def compile_fixture(cc, folder, source=ROOT):
    """The fixture's path; with sanitizers on Linux when this compiler links them."""
    run = lambda cmd: subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    exe = folder / "coop_sync_test.exe"
    # (mingw's printf takes %zu and %llu with its own stdio)
    tries = [["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], []] if os.name != "nt" else [["-D__USE_MINGW_ANSI_STDIO=1", "-D_CRT_SECURE_NO_WARNINGS"]]
    for i, extra in enumerate(tries):
        mono = folder / "monocypher.o"
        r = run([cc, "-std=c99", "-O2", "-w", "-c", str(source / "third_party/monocypher/monocypher.c"), "-o", str(mono)] + extra)
        if r.returncode == 0:
            r = run([cc, "-std=c11", "-O2" if not extra or extra[0].startswith("-D") else "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                     "-I" + str(source / "host"), str(ROOT / "tests" / "coop_sync_test.c"), str(source / "host" / "coop_state.c"),
                     str(source / "host" / "coop_proto.c"), str(mono), "-o", str(exe)] + extra)
        if r.returncode == 0:
            print("compiled%s" % (" with " + " ".join(extra) if extra else ""))
            return exe
        if i + 1 == len(tries):
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: the fixture does not compile")
        print("(not with %s: %s)" % (" ".join(extra), ((r.stdout + r.stderr).strip().splitlines() or [""])[-1]))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "gcc")
    p.add_argument("--source-root", type=Path, default=ROOT)
    a = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-coop-sync-") as temp:
        exe = compile_fixture(a.cc, Path(temp), a.source_root)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=900)
        sys.stdout.write(r.stdout)
        sys.stdout.write(r.stderr)
        if r.returncode:
            print("FAIL: tests/coop_sync_test.c exited %d" % r.returncode)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
