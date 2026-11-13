"""host/coop.c's reliable events, without the game (tests/coop_events_test.c, compiled with host/coop.c's own link
layer host/coop_net.c, host/coop_proto.c and Monocypher): one process holds a room over a real loopback TCP listener,
another joins it, and the fixture sits between them as the pipe. The fixture's own clock drives the session's event
timers, so every case measures what it says it measures:

  no-loss       every queued event, delivered once, in id order, the queue emptied
  loss          the same with the pipe throwing away 30 % of what it carries
  reverse       the pipe hands the frames over in the other order
  split         a frame written in two halves, a header split in two, two frames in one write
  parts         a 16 KiB event with one part lost: whole or nothing, and that part sent again
  duplicate     an ACK lost, a part sent again, a recorded frame sent again (its envelope is seen)
  queue         the bounded queue (INVALID, FULL, UNAVAILABLE), a recipient gone or replaced, this game leaving
  acceptance    nothing is claimed delivered before the other player is accepted, nor under a stale view
  cancel        an event abandoned mid-way, later events delivered, the abandoned one cannot return
  cancel-accept the same with the receiver accepting again
  reset-ack     a receiver that starts over: its old half is forgotten, and its old ACK cannot roll it back
  delayed-ack   the same with that old ACK arriving late
  reaccept-step consent revoked and given again between two steps still discards the assembly
  codec         the event part and ACK codecs: their lengths, their versions, their refusals
  threads       enqueueing from another thread while the room's thread steps (again under ThreadSanitizer)

Linux runs use AddressSanitizer and UBSan, and a second ThreadSanitizer build for `threads`, when the compiler can
link them; when it cannot, the cases still run without them, as test_coop_session.py does.

    python -I -B tests/test_coop_events.py [--cc gcc] [--source-root path/to/other/source]
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]
CASES = ("no-loss", "loss", "reverse", "split", "parts", "duplicate", "queue", "acceptance", "codec", "threads",
         "cancel", "cancel-accept", "reset-ack", "delayed-ack", "reaccept-step")


def compile_fixture(cc, source, folder, sanitizers, note):
    """The fixture with the given sanitizer flags; None when this compiler cannot link them."""
    exe = folder / ("events.exe" if note == "" else "events" + note.replace("=", "") + ".exe")
    if os.name == "nt":
        flags, libs = ["-D__USE_MINGW_ANSI_STDIO=1", "-D_CRT_SECURE_NO_WARNINGS"], ["-lws2_32", "-lbcrypt", "-liphlpapi"]
    else:
        flags, libs = list(sanitizers), ["-pthread"]
    mono = folder / ("mono%s.o" % note)
    commands = [
        [cc, "-std=c99", "-O2", "-w", "-c", str(source / "third_party/monocypher/monocypher.c"), "-o", str(mono)] + flags,
        [cc, "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I" + str(source / "host"),
         str(ROOT / "tests/coop_events_test.c"), str(source / "host/coop_net.c"), str(source / "host/coop_proto.c"),
         str(mono), "-o", str(exe)] + flags + libs,
    ]
    for command in commands:
        r = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if r.returncode:
            print("(not%s: %s)" % (note, (r.stdout + r.stderr).strip().splitlines()[-1] if (r.stdout + r.stderr).strip() else "failed"))
            return None
    print("compiled " + (" ".join(flags) if flags else "plain"))
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or "clang")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hookshot-coop-events-") as temp:
        folder = Path(temp)
        exe = compile_fixture(args.cc, args.source_root, folder,
                              [] if os.name == "nt" else ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], "")
        if exe is None:
            exe = compile_fixture(args.cc, args.source_root, folder, [], "")
        for case in CASES:
            subprocess.run([str(exe), case], check=True, timeout=120)
        tsan_note = ""
        if os.name != "nt":
            tsan = compile_fixture(args.cc, args.source_root, folder, ["-fsanitize=thread"], "-tsan")
            if tsan:
                subprocess.run([str(tsan), "threads"], check=True, timeout=120)
                tsan_note = " and ThreadSanitizer"
        print("PASS: reliable session events over a room of its own, %d cases%s" % (len(CASES), tsan_note))


if __name__ == "__main__":
    main()
