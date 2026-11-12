"""host/coop.c, co-op's session layer, and host/coop_net.c, its sockets, threads and random bytes, without the game
(tests/coop_session_test.c, compiled with host/coop_proto.c and Monocypher; on Linux with AddressSanitizer and UBSan,
and a second time with ThreadSanitizer, when the compiler has them). Eight runs at once, each its own process:

  unit        the operating-system layer on loopback, coop_server_parse, a stop's order, and the process's session off
              (no thread, no socket) unless HOS_COOP or the options file's coop=1 switches it on, and ended (no thread
              left) when the game closes
  unit, menu  co-op off at start, switched on and off by coop_configure (the menu's Co-op tab) while the game runs:
              off, on, off cycles with a socket open, each off leaving no thread (counted in /proc/self/task) and no
              socket (/proc/self/fd), and stops the thread is too slow for. Before taking a baseline, each joined
              fixture helper's specific Linux TID must disappear within a bounded wait; no count range is allowed.
  the same two under ThreadSanitizer (Linux): the co-op thread started, ended and started again beside its callers
  mock 0/30   two sessions through the test's own relay, which also acts as a hostile one (changed, forged and replayed
              datagrams, garbage, forged rosters, lies), with 0 % and 30 % of the received datagrams thrown away
  relay 0/30  the same two through scripts/coop_relay.py on 127.0.0.1, restarted midway, plus the process's session
              started from the settings by coop_init and, on Linux, a child process ended by SIGTERM, which says
              nothing (co-op installs no signal handler): the relay drops it for its silence, as its log must say

Also checks the wiring: the game starts co-op beside the Discord status on both platforms (host/hos_main.c), says
goodbye when its window closes (host/hos_relaunch.c), and links Winsock and BCrypt on Windows (nx2/build.py).

A compiled boundary fixture also checks the actual sender with the real encryption code: the last counter is sent
once, and exhaustion leaves the room without emitting a wrapped counter.

    python -I -B tests/test_coop_session.py [--cc gcc] [--only unit|mock|relay]   (COOP_TEST_VERBOSE=1: every log line)
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
MONOCYPHER = ROOT / "third_party" / "monocypher"
RELAY = ROOT / "scripts" / "coop_relay.py"


def compile_fixture(cc, folder, thread_sanitizer=False):
    """The fixture's path and compiler flags; with sanitizers on Linux when available. For ThreadSanitizer,
    return None when this compiler cannot make the fixture."""
    run = lambda cmd: subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    exe = folder / ("coop_session_test" + ("_tsan" if thread_sanitizer else "") + (".exe" if os.name == "nt" else ""))
    if os.name == "nt":   # (mingw's printf takes %zu with its own stdio; Winsock and BCryptGenRandom)
        tries, libs = [["-D__USE_MINGW_ANSI_STDIO=1"]], ["-lws2_32", "-lbcrypt", "-liphlpapi"]
    elif thread_sanitizer:
        tries, libs = [["-fsanitize=thread"]], ["-pthread"]
    else:
        tries, libs = [["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], []], ["-pthread"]
    if thread_sanitizer and os.name == "nt":
        return None
    for i, extra in enumerate(tries):
        mono = folder / ("monocypher%s.o" % ("_tsan" if thread_sanitizer else ""))
        r = run([cc, "-std=c99", "-O2", "-w", "-c", str(MONOCYPHER / "monocypher.c"), "-o", str(mono)] + extra)
        if r.returncode == 0:
            r = run([cc, "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "host"),
                     str(ROOT / "tests" / "coop_session_test.c"), str(ROOT / "host" / "coop.c"), str(ROOT / "host" / "coop_net.c"),
                     str(ROOT / "host" / "coop_relay.c"),
                     str(ROOT / "host" / "coop_proto.c"), str(mono), "-o", str(exe)] + extra + libs)
        if r.returncode == 0:
            print("compiled%s" % (" with " + " ".join(extra) if extra else ""))
            return exe, extra
        if thread_sanitizer:
            print("(not with %s: %s: no ThreadSanitizer runs)" % (" ".join(extra), ((r.stdout + r.stderr).strip().splitlines() or [""])[-1]))
            return None
        if i + 1 == len(tries):
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: the fixture does not compile")
        print("(not with %s: %s)" % (" ".join(extra), ((r.stdout + r.stderr).strip().splitlines() or [""])[-1]))




def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_wiring():
    main_c = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    starts = [i for i in range(len(main_c)) if main_c.startswith("hos_discord_start();", i)]
    if len(starts) != 2:
        raise SystemExit("FAIL: host/hos_main.c starts the Discord status %d times (two expected: Windows, Linux)" % len(starts))
    for at in starts:
        after = main_c[at:at + 200]
        if "coop_init();" not in after or after.index("coop_init();") > after.index("}"):
            raise SystemExit("FAIL: host/hos_main.c does not start co-op (coop_init) beside hos_discord_start at offset %d" % at)
    goodbye = block((ROOT / "host" / "hos_relaunch.c").read_text(encoding="utf-8"), "void hos_window_closed_exit(void) {")
    if "coop_shutdown();" not in goodbye or goodbye.index("coop_shutdown();") > goodbye.index("hos_log_flush();"):
        raise SystemExit("FAIL: host/hos_relaunch.c hos_window_closed_exit does not say goodbye (coop_shutdown) before the log's flush")
    # (the control bridge's quit, docs/control.md, is the game's other own way out)
    quit_ = block((ROOT / "host" / "control.c").read_text(encoding="utf-8"), "static void game_quit(void) {")
    if "coop_shutdown();" not in quit_ or quit_.index("coop_shutdown();") > quit_.index("hos_log_flush();"):
        raise SystemExit("FAIL: host/control.c game_quit does not say goodbye (coop_shutdown) before the log's flush")
    build = (ROOT / "nx2" / "build.py").read_text(encoding="utf-8").splitlines()
    at = next(i for i, line in enumerate(build) if line.startswith("HOST_LIBS = "))
    end = next(i for i in range(at + 1, len(build)) if not build[i].startswith(" "))   # (the statement's continued lines)
    windows = " ".join(build[at:end]).split(" else ", 1)[-1]
    if '"-lws2_32"' not in windows or '"-lbcrypt"' not in windows:
        raise SystemExit("FAIL: nx2/build.py's Windows HOST_LIBS lack -lws2_32 and -lbcrypt (host/coop_net.c: Winsock, BCryptGenRandom)")
    applet = (ROOT / "host" / "nn_applet.c").read_text(encoding="utf-8")
    if "int hos_ini_text(const char *key, char *out, size_t size)" not in applet:
        raise SystemExit("FAIL: host/nn_applet.c has no hos_ini_text (host/coop.c reads coop_name= and coop_server= with it)")


COUNTER_FIXTURE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coop_proto.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL: %s\n", #x); exit(1); } } while (0)
typedef struct {
    uint8_t msg_key[COOP_MSG_KEY_LEN];
    int self, left;
    uint64_t session;
    uint32_t out_counter;
} CoopSess;
static unsigned delivered;
static uint8_t payload[COOP_MAX_PAYLOAD];
static size_t payload_len;
static void send_room(CoopSess *s, uint8_t type, uint8_t dst, const void *bytes, size_t n) {
    (void)s; (void)type; (void)dst;
    CHECK(n <= sizeof payload);
    memcpy(payload, bytes, n);
    payload_len = n;
    delivered++;
}
void go_left(CoopSess *s, const char *why, int log_it) {
    (void)why; (void)log_it;
    s->left = 1;
}
@@SEND@@
int main(void) {
    CoopSess s = {.self = 1, .session = 1234, .out_counter = UINT32_MAX - 1};
    uint8_t input = 0x42, output = 0, kind = 0;
    uint64_t session = 0;
    uint32_t counter = 0;
    size_t n = 0;
    send_sealed(&s, COOP_DST_ALL, 0x37, &input, 1);
    CHECK(delivered == 1 && !s.left);
    CHECK(coop_envelope_peek(payload, payload_len, &session, &counter));
    CHECK(session == s.session && counter == UINT32_MAX);
    CHECK(coop_open(payload, payload_len, s.msg_key, 1, COOP_DST_ALL, &kind, &output, 1, &n));
    CHECK(kind == 0x37 && n == 1 && output == input);
    send_sealed(&s, COOP_DST_ALL, 0x37, &input, 1);
    CHECK(delivered == 1 && s.left);
    send_sealed(&s, COOP_DST_ALL, 0x37, &input, 1);
    CHECK(delivered == 1 && s.left);
    puts("PASS: the last encrypted counter is sent once; exhaustion leaves without wrapping");
    return 0;
}
'''


def check_counter(cc, folder, flags, source):
    src, exe = folder / "counter.c", folder / "counter.exe"
    src.write_text(COUNTER_FIXTURE.replace("@@SEND@@", block(source, "static void send_sealed(")), encoding="utf-8")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "host"), str(src), str(ROOT / "host/coop_proto.c"),
                    str(folder / "monocypher.o"), "-o", str(exe)] + flags, check=True, timeout=300)
    subprocess.run([str(exe)], check=True, timeout=30)


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class RelayProc:
    """scripts/coop_relay.py as a process on a fixed port, so that it can be stopped and started again. Its log is kept
    across restarts, for a failure's message."""

    def __init__(self, port):
        self.port, self.p, self.log, self.reader = port, None, [], None

    def _read(self, out):
        try:
            for line in out:
                self.log.append(line.rstrip())
        except (OSError, ValueError):
            pass   # closed by stop()

    def start(self):
        self.p = subprocess.Popen([sys.executable, "-I", str(RELAY), "--host", "127.0.0.1", "--port", str(self.port), "--log-joins",
                                   "--log-addrs"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        line = self.p.stdout.readline()
        self.log.append(line.rstrip())
        if "listening" not in line:
            raise RuntimeError("the relay did not start: " + line)
        self.reader = threading.Thread(target=self._read, args=(self.p.stdout,), daemon=True)
        self.reader.start()

    def stop(self):
        if self.p:
            self.p.kill()
            self.p.wait()
            self.reader.join(5)
            self.p.stdout.close()
            self.p = None
            self.log.append("(the relay stopped)")


def run_mode(exe, name, args, relay_drop=None, work=None):
    """One run of the fixture: (name, passed, its output)."""
    relay = None
    cmd = [str(exe)] + (args or [])
    if relay_drop is not None:
        relay = RelayProc(free_udp_port())
        relay.start()
        cmd += ["--relay", "127.0.0.1:%d" % relay.port, "--drop", str(relay_drop), "--work", str(work)]
    started = time.monotonic()
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=str(ROOT))
    lines, restarted = [], False
    timer = threading.Timer(480, p.kill)
    timer.start()
    try:
        for line in p.stdout:
            lines.append(line.rstrip())
            if "coop-session-test: restart the relay now" in line and relay:
                relay.stop()
                time.sleep(0.3)
                relay.start()
                restarted = True
                p.stdin.write("restarted\n")
                p.stdin.flush()
        code = p.wait()
    finally:
        timer.cancel()
        p.stdin.close()
        p.stdout.close()
        if relay:
            relay.stop()
    text = "\n".join(lines)
    # (the child ended by SIGTERM said no goodbye: the relay's own line for it is "dropped, silent 15 s", not "left")
    dropped = relay is None or os.name == "nt" or any("Player-" in l and "dropped, silent 15 s" in l for l in relay.log)
    if not dropped:
        text += "\nFAIL: the relay's log has no 'Player-xxxx (member 3) dropped, silent 15 s' line for the child ended by SIGTERM"
    ok = code == 0 and "coop-session-test: passed" in text and (relay is None or restarted) and dropped
    head = "--- %s: %s in %.1f s (exit %d)" % (name, "passed" if ok else "FAILED", time.monotonic() - started, code)
    if relay and (not ok or os.environ.get("COOP_TEST_VERBOSE")):
        text += "\n--- the relay's log ---\n" + "\n".join(relay.log[-300:])
    return name, ok, head + "\n" + text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "gcc")
    p.add_argument("--only", choices=("unit", "mock", "relay"))
    a = p.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix="hookshot-coop-session-") as temp:
        temp = Path(temp)
        exe, flags = compile_fixture(a.cc, temp)
        check_counter(a.cc, temp, flags, (ROOT / "host/coop.c").read_text(encoding="utf-8"))
        runs = [("unit", exe, ["--unit"], None), ("unit, menu", exe, ["--unit-menu"], None),
                ("mock, 0 % lost", exe, ["--mock", "--drop", "0"], None), ("mock, 30 % lost", exe, ["--mock", "--drop", "30"], None),
                ("relay, 0 % lost", exe, None, 0), ("relay, 30 % lost", exe, None, 30)]
        tsan_build = compile_fixture(a.cc, temp, thread_sanitizer=True)
        if tsan_build:   # (a data race it reports ends the run with exit 66)
            tsan, _ = tsan_build
            runs[2:2] = [("unit, ThreadSanitizer", tsan, ["--unit"], None), ("unit, menu, ThreadSanitizer", tsan, ["--unit-menu"], None)]
        runs = [r for r in runs if not a.only or r[0].startswith(a.only)]
        works = []
        for i in range(len(runs)):
            works.append(temp / ("work%d" % i))
            works[-1].mkdir()
        with concurrent.futures.ThreadPoolExecutor(len(runs)) as pool:
            results = list(pool.map(lambda ir: run_mode(ir[1][1], ir[1][0], ir[1][2], ir[1][3], works[ir[0]]), enumerate(runs)))
    failed = [name for name, ok, _ in results if not ok]
    for _, _, text in results:
        print(text)
    if failed:
        print("FAIL: " + ", ".join(failed))
        return 1
    print("PASS: the co-op session layer, %d runs" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
