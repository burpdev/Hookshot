"""host/coop_proto.c, co-op's wire format, compiled with Monocypher (third_party/monocypher) and tests/coop_proto_test.c:
known answers for BLAKE2b and the signatures, every byte vector of tests/coop_vectors.txt, 400000 random buffers and one
changed byte of every vector into each parser, and the parts an event of a given size has. On Linux with
AddressSanitizer and UBSan when the compiler has them. Also checks that the runtime's build compiles Monocypher
(nx2/build.py), without which the game does not link.

    python -I -B tests/test_coop_proto.py [--cc gcc]
    python -I -B tests/test_coop_proto.py --write-vectors   rewrite tests/coop_vectors.txt from the codec, for when the
                                                            format itself is what changed (docs/coop.md)
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


def compile_fixture(cc, folder):
    """The fixture's path; with sanitizers on Linux when this compiler links them."""
    run = lambda cmd: subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    exe = folder / "coop_proto_test.exe"
    # (mingw's printf takes %zu and %lld with its own stdio)
    tries = [["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], []] if os.name != "nt" else [["-D__USE_MINGW_ANSI_STDIO=1", "-D_CRT_SECURE_NO_WARNINGS"]]
    for i, extra in enumerate(tries):
        mono = folder / "monocypher.o"
        r = run([cc, "-std=c99", "-O2", "-w", "-c", str(MONOCYPHER / "monocypher.c"), "-o", str(mono)] + extra)
        if r.returncode == 0:
            r = run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host"), "-I" + str(MONOCYPHER),
                     str(ROOT / "tests" / "coop_proto_test.c"), str(ROOT / "host" / "coop_proto.c"), str(mono), "-o", str(exe)] + extra)
        if r.returncode == 0:
            print("compiled%s" % (" with " + " ".join(extra) if extra else ""))
            return exe
        if i + 1 == len(tries):
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: the fixture does not compile")
        print("(not with %s: %s)" % (" ".join(extra), ((r.stdout + r.stderr).strip().splitlines() or [""])[-1]))


def check_build_wiring():
    build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8")
    if 'os.path.join(MONOCYPHER, "monocypher.c")' not in build or '"third_party", "monocypher")' not in build:
        raise SystemExit("FAIL: nx2/build.py does not compile third_party/monocypher/monocypher.c with the runtime "
                         "(host/coop_proto.c calls it)")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "gcc")
    p.add_argument("--write-vectors", action="store_true")
    a = p.parse_args()
    check_build_wiring()
    with tempfile.TemporaryDirectory(prefix="hookshot-coop-proto-") as temp:
        exe = compile_fixture(a.cc, Path(temp))
        if a.write_vectors:
            w = subprocess.run([str(exe), "--write-vectors"], capture_output=True, text=True, timeout=300)
            if w.returncode:
                print(w.stdout + w.stderr)
                raise SystemExit("FAIL: tests/coop_proto_test.c could not write the byte vectors")
            (ROOT / "tests" / "coop_vectors.txt").write_text(w.stdout, encoding="utf-8")
            print("wrote tests/coop_vectors.txt: %d vectors" %
                  len([l for l in w.stdout.splitlines() if l and not l.startswith("#")]))
            return 0
        args = [str(ROOT / "tests" / "coop_vectors.txt")]
        r = subprocess.run([str(exe)] + args, capture_output=True, text=True, timeout=300)
        sys.stdout.write(r.stdout)
        sys.stdout.write(r.stderr)
        if r.returncode:
            print("FAIL: tests/coop_proto_test.c exited %d" % r.returncode)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
