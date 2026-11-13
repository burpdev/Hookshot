"""host/coop.c, co-op's session layer, and host/coop_net.c, its sockets, threads and random bytes, without the game
(tests/coop_session_test.c, compiled with host/coop_proto.c and Monocypher; on Linux with AddressSanitizer and UBSan,
and a second time with ThreadSanitizer, when the compiler has them). Seven runs at once, each its own process:

  unit        the operating-system layer on loopback, coop_server_parse, a stop's order, and the process's session off
              (no thread, no socket) unless HOS_COOP or the options file's coop=1 switches it on, and ended (no thread
              left) when the game closes
  unit, menu  co-op off at start, switched on and off by coop_configure (the menu's Co-op tab) while the game runs:
              off, on, off cycles with a link open, each off leaving no thread (counted in /proc/self/task) and no
              socket (/proc/self/fd), and stops the thread is too slow for. Before taking a baseline, each joined
              fixture helper's specific Linux TID must disappear within a bounded wait; no count range is allowed.
  the same two under ThreadSanitizer (Linux): the co-op thread started, ended and started again beside its callers
  hostile   the hostile host's scenarios alone, each on a port of its own: what a host does to a player that trusts it
              when it answers nothing, sends bytes that are not a frame, a frame of another version, every refusal it
              has, a message sealed with another room's key or one it has already sent, or a roster that makes the
              player the maker
  mock 0/30   sessions of this process, one hosting on a port of its own and the others reaching it by address, with
              0 % and 30 % of the frames each receives thrown away: a room hosted, joined, filled, lost and made again
              on the same port, messages both ways, a member whose link closed abruptly coming back with the id it
              held, the maker removing a member, the process's own session started from the settings, and on Linux a
              child process in the room ended by SIGTERM, which says nothing and is dropped when its link closes

Also checks the wiring: the game starts co-op beside the Discord status on both platforms (host/hos_main.c), says
goodbye when its window closes (host/hos_relaunch.c), and links Winsock and BCrypt on Windows (nx2/build.py).

A compiled boundary fixture also checks the actual sender with the real encryption code: the last counter is sent
once, and exhaustion leaves the room without emitting a wrapped counter.
The actual Windows socket-opening function is also compiled against a call-recording Winsock stand-in: it must
request exclusive ownership before binding and close the socket if that request, the bind, the listen or the move to
nonblocking fails. Real cross-family bind checks run in unit mode on Windows and Linux, which is where the POSIX
branch's SO_REUSEADDR is checked (the stand-in is compiled as Windows, so it cannot see that branch); no parallel
mode or existing assertion is removed.

    python -I -B tests/test_coop_session.py [--cc gcc] [--only unit|menu|hostile|mock]   (COOP_TEST_VERBOSE=1: every log line)
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]
MONOCYPHER = ROOT / "third_party" / "monocypher"


def compile_fixture(cc, folder, thread_sanitizer=False):
    """The fixture's path and compiler flags; with sanitizers on Linux when available. For ThreadSanitizer,
    return None when this compiler cannot make the fixture."""
    run = lambda cmd: subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    exe = folder / ("coop_session_test" + ("_tsan" if thread_sanitizer else "") + (".exe" if os.name == "nt" else ""))
    if os.name == "nt":   # (mingw's printf takes %zu with its own stdio; Winsock and BCryptGenRandom)
        tries, libs = [["-D__USE_MINGW_ANSI_STDIO=1", "-D_CRT_SECURE_NO_WARNINGS"]], ["-lws2_32", "-lbcrypt", "-liphlpapi"]
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
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
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


# host/coop_net.c's coop_tcp_listen, compiled as the Windows branch with every socket call recorded: the port must be
# owned before it is bound, and a socket that cannot be owned, bound, listened on or made nonblocking is closed rather
# than left open. The POSIX branch (SO_REUSEADDR, which lets a port be hosted on again right after a game closed
# without letting a second socket share a live one) is checked by the real binds of tests/coop_session_test.c's
# check_listen_ownership, which run in unit mode on both platforms.
BIND_FIXTURE = r'''
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#define _WIN32 1
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL bind: %s\n", #x); exit(1); } } while (0)
typedef int Sock;
typedef int socklen_t;
enum { SOCK_NONE = -1, AF_INET6 = 10, SOCK_STREAM = 1, IPPROTO_TCP = 6, IPPROTO_IPV6 = 41,
       IPV6_V6ONLY = 27, SOL_SOCKET = 1, SO_EXCLUSIVEADDRUSE = 100 };
struct in6_addr { unsigned char bytes[16]; };
static const struct in6_addr in6addr_any = {{0}};
struct sockaddr { unsigned short family; };
struct sockaddr_in6 { unsigned short sin6_family; uint16_t sin6_port; struct in6_addr sin6_addr; };
typedef struct { Sock s; uint16_t port; } CoopTcpListen;
enum { COOP_TCP_MAX = 32 };
static int exclusive, bound, listed, nonblocking, closed;
static int fail_exclusive, fail_bind, fail_listen, fail_nonblock;
static uint16_t want;
static int net_up(void) { return 1; }
static Sock socket(int family, int kind, int protocol) {
    CHECK(family == AF_INET6 && kind == SOCK_STREAM && protocol == IPPROTO_TCP);
    exclusive = bound = listed = nonblocking = closed = 0;
    return 7;
}
static int setsockopt(Sock s, int level, int option, const char *value, int size) {
    CHECK(s == 7 && !bound && size == sizeof(int));   /* (every option is set before the bind) */
    if (option == SO_EXCLUSIVEADDRUSE) {
        CHECK(level == SOL_SOCKET && *(const int *)value == 1);
        if (fail_exclusive) return -1;
        exclusive = 1;
    } else CHECK(level == IPPROTO_IPV6 && option == IPV6_V6ONLY && *(const int *)value == 0);
    return 0;
}
static int bind(Sock s, const struct sockaddr *address, int size) {
    const struct sockaddr_in6 *six = (const struct sockaddr_in6 *)address;
    CHECK(s == 7 && exclusive && !closed && address && size == sizeof(struct sockaddr_in6));
    CHECK(six->sin6_family == AF_INET6 && six->sin6_port == want && !memcmp(six->sin6_addr.bytes, in6addr_any.bytes, 16));
    bound++;
    return fail_bind ? -1 : 0;
}
static int listen(Sock s, int backlog) {
    CHECK(s == 7 && bound && !closed && backlog == COOP_TCP_MAX);
    if (fail_listen) return -1;
    listed = 1;
    return 0;
}
static int getsockname(Sock s, struct sockaddr *address, socklen_t *size) {
    CHECK(s == 7 && listed && !closed && *size == sizeof(struct sockaddr_in6));
    ((struct sockaddr_in6 *)address)->sin6_port = 32123;   /* (the port the stand-in gave the wildcard bind) */
    return 0;
}
static void sock_close(Sock s) { CHECK(s == 7 && !closed); closed++; }
static uint16_t htons(uint16_t n) { return n; }
static uint16_t ntohs(uint16_t n) { return n; }
static int sock_nonblock(Sock s) {
    CHECK(s == 7 && listed && !closed);
    if (fail_nonblock) return 0;
    nonblocking = 1;
    return 1;
}
@@LISTEN@@
int main(void) {
    want = 0;
    CoopTcpListen *l = coop_tcp_listen(0);
    CHECK(l && exclusive && bound == 1 && listed && nonblocking && !closed && l->port == 32123);
    sock_close(l->s);
    free(l);
    fail_exclusive = 1;
    CHECK(!coop_tcp_listen(0) && !bound && closed == 1);
    fail_exclusive = 0, fail_bind = 1, want = 32123;
    CHECK(!coop_tcp_listen(32123) && bound == 1 && closed == 1);
    fail_bind = 0, fail_listen = 1;
    CHECK(!coop_tcp_listen(32123) && bound == 1 && !listed && closed == 1);
    fail_listen = 0, fail_nonblock = 1;
    CHECK(!coop_tcp_listen(32123) && listed && !nonblocking && closed == 1);
    puts("PASS: Windows port ownership precedes bind; a socket that cannot be owned, bound, listened on or made "
         "nonblocking is closed");
    return 0;
}
'''


def check_bind(cc, folder, flags, source):
    src, exe = folder / "bind.c", folder / "bind.exe"
    start = source.index("CoopTcpListen *coop_tcp_listen(")
    opened = source[start:source.index("\nuint16_t coop_tcp_listen_port(", start)]
    src.write_text(BIND_FIXTURE.replace("@@LISTEN@@", opened), encoding="utf-8")
    subprocess.run([cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)] + flags,
                   check=True, timeout=300)
    subprocess.run([str(exe)], check=True, timeout=30)


def run_mode(exe, name, args, work):
    """One run of the fixture: (name, passed, its output)."""
    cmd = [str(exe)] + (args or [])
    if work:
        cmd += ["--work", str(work)]
    started = time.monotonic()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=str(ROOT))
    lines = []
    timer = threading.Timer(480, p.kill)
    timer.start()
    try:
        for line in p.stdout:
            lines.append(line.rstrip())
        code = p.wait()
    finally:
        timer.cancel()
        p.stdout.close()
    text = "\n".join(lines)
    ok = code == 0 and "coop-session-test: passed" in text
    head = "--- %s: %s in %.1f s (exit %d)" % (name, "passed" if ok else "FAILED", time.monotonic() - started, code)
    return name, ok, head + "\n" + text


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "gcc")
    p.add_argument("--only", choices=("unit", "menu", "hostile", "mock"))
    a = p.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix="hookshot-coop-session-") as temp:
        temp = Path(temp)
        exe, flags = compile_fixture(a.cc, temp)
        check_counter(a.cc, temp, flags, (ROOT / "host/coop.c").read_text(encoding="utf-8"))
        check_bind(a.cc, temp, flags, (ROOT / "host" / "coop_net.c").read_text(encoding="utf-8"))
        # (the first field is what --only selects by; the second is the name the run is reported under)
        runs = [({"unit"}, "unit", exe, ["--unit"], None),
                ({"unit", "menu"}, "unit, menu", exe, ["--unit-menu"], None),
                ({"hostile"}, "hostile", exe, ["--hostile"], None),
                ({"mock"}, "mock, 0 % lost", exe, ["--mock", "--drop", "0"], True),
                ({"mock"}, "mock, 30 % lost", exe, ["--mock", "--drop", "30"], True)]
        tsan_build = compile_fixture(a.cc, temp, thread_sanitizer=True)
        if tsan_build:   # (a data race it reports ends the run with exit 66)
            tsan, _ = tsan_build
            runs[2:2] = [({"unit"}, "unit, ThreadSanitizer", tsan, ["--unit"], None),
                         ({"unit", "menu"}, "unit, menu, ThreadSanitizer", tsan, ["--unit-menu"], None)]
        runs = [r for r in runs if not a.only or a.only in r[0]]
        works = []
        for i in range(len(runs)):
            works.append(temp / ("work%d" % i))
            works[-1].mkdir()
        with concurrent.futures.ThreadPoolExecutor(len(runs)) as pool:
            results = list(pool.map(lambda ir: run_mode(ir[1][2], ir[1][1], ir[1][3], works[ir[0]] if ir[1][4] else None),
                                    enumerate(runs)))
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
