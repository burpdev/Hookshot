"""host/coop.c and host/coop_net.c without the game (tests/coop_session_test.c, one mode per process, plus checks that
read the sources they depend on). The room is a real TCP room on loopback: the fixture's sessions hold it and join it
over sockets, on threads of their own, so what is measured is what the game will do.

  unit          host/coop_net.c on loopback (a door that owns its port, a frame handed over whole, a frame cut in two,
                two frames in one write, a length no reader can hold, a closed link, random bytes, the clock, an event,
                a once, a thread, the addresses shown to a joiner), coop_server_parse, and the process's session
                switched on and off with the thread and socket counts of the process itself
  unit, menu    the switching on and off again, on its own, with the counts
  room          three sessions: one holds the room, two join it. The maker is member 1, ids are given once, a message
                reaches the others and not its own sender, an event arrives whole, a player that leaves is out of the
                roster, the maker ends another player's join, and a session that stops leaves nothing behind
  hostile       a client that means harm: another protocol's HELLO, bytes that are not frames, a frame length no reader
                can hold, a connection that never says who it is, a room already full
  auto          the process's session started from the settings the way host/hos_main.c starts it (HOS_COOP=1,
                HOS_COOP_AUTO=host, HOS_COOP_PORT, HOS_COOP_NAME), a session joining it, and coop_shutdown ending it
  the counter   the last message counter of a join, sent once, and exhaustion leaving the room instead of wrapping
                (the check cuts send_message out of host/coop.c and compiles it with stand-ins)
  Windows bind  the port is asked for exclusively before bind, and a failure there closes the socket rather than
                leaving a half-open door (the check cuts coop_tcp_listen out of host/coop_net.c and compiles its
                Windows branch against Winsock stand-ins)

Linux runs use AddressSanitizer and UBSan, and a second ThreadSanitizer build for the two unit runs, when the compiler
can link them; when it cannot, the runs go ahead without them and say so.

    python -I -B tests/test_coop_session.py [--cc gcc] [--only unit|room|hostile|auto]
"""
import argparse
import concurrent.futures
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import time

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]
SOURCE = ["tests/coop_session_test.c", "host/coop.c", "host/coop_net.c", "host/coop_proto.c"]   # (Monocypher comes in as its own object)
SANITIZERS = [[], ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"],
              ["-fsanitize=address,undefined"]]   # (a compiler that has one but not the other still gets a run)


def compile_fixture(cc, folder, extra=(), note=""):
    """The fixture with these extra flags; (exe, flags) or None when this compiler cannot link them."""
    exe = folder / ("session%s.exe" % note)
    flags = list(extra) + ([] if os.name == "nt" else ["-pthread"])
    libs = ["-lws2_32", "-lbcrypt", "-liphlpapi"] if os.name == "nt" else []
    mono = folder / ("mono%s.o" % note)
    commands = [
        [cc, "-std=c99", "-O2", "-w", "-c", str(ROOT / "third_party/monocypher/monocypher.c"), "-o", str(mono)] + flags,
        [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE",
         "-I" + str(ROOT / "host")] + [str(ROOT / s) for s in SOURCE] + [str(mono), "-o", str(exe)] + flags + libs,
    ]
    for command in commands:
        r = subprocess.run(command, capture_output=True, text=True, timeout=300)
        if r.returncode:
            tail = ((r.stdout + r.stderr).strip().splitlines() or ["no output"])[-1]
            print("(not%s: %s)" % (note, tail))
            return None
    return exe, flags


def fixture_flags(cc, folder):
    """(exe, flags) with the strongest sanitizers this compiler can link, or the plain build."""
    for extra in reversed(SANITIZERS):
        got = compile_fixture(cc, folder, extra)
        if got:
            return got
    raise SystemExit("FAIL: the fixture does not compile")


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
    # (the room is the host's own socket: nothing in the build or the game may bring a relay back)
    for name in ("nx2/build.py", "host/hos_main.c", "host/coop_menu.c", "gpu/vk/vk_menu.cpp"):
        text = (ROOT / name).read_text(encoding="utf-8")
        if "coop_relay" in text:
            raise SystemExit("FAIL: %s still mentions coop_relay, which this room does not have" % name)


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
    uint64_t session;
    uint32_t out_counter;
    int self, left;
} CoopSess;
static unsigned delivered;
static uint8_t frame[COOP_MAX_PAYLOAD];
static size_t frame_len;
static int frame_dst, frame_type, frame_kind, frame_src;
static int send_frame(CoopSess *s, int dst, uint8_t type, uint8_t kind, const void *payload, size_t n, int src) {
    (void)s;
    CHECK(n <= sizeof frame);
    memcpy(frame, payload, n), frame_len = n;
    frame_dst = dst, frame_type = type, frame_kind = kind, frame_src = src;
    delivered++;
    return 1;
}
static void go_left(CoopSess *s, const char *why, int log_it) {
    (void)why; (void)log_it;
    s->left = 1;
}
@@SEND@@
int main(void) {
    CoopSess s = {1234, UINT32_MAX - 1, 1, 0};
    uint8_t input = 0x42, kind = 0;
    uint64_t session = 0;
    uint32_t counter = 0;
    const uint8_t *body = NULL;
    size_t n = 0;
    send_message(&s, COOP_DST_ALL, 0x37, &input, 1);
    CHECK(delivered == 1 && !s.left && s.out_counter == UINT32_MAX);
    CHECK(frame_type == COOP_F_DATA && frame_dst == COOP_DST_ALL && frame_src == 1);
    CHECK(coop_msg_parse(frame, frame_len, &session, &counter, &kind, &body, &n));
    CHECK(session == 1234 && counter == UINT32_MAX && kind == 0x37 && n == 1 && body[0] == 0x42);
    send_message(&s, COOP_DST_ALL, 0x37, &input, 1);   // (the last counter was used: this join's sending is over)
    CHECK(delivered == 1 && s.left);
    send_message(&s, COOP_DST_ALL, 0x37, &input, 1);   // (and it stays over: no counter, no message, no wrap)
    CHECK(delivered == 1 && s.left);
    puts("PASS: the last counter of a join is sent once; exhaustion leaves the room instead of wrapping");
    return 0;
}
'''


def run_check(cc, name, compile_command, exe):
    """Compile and run one cut-out fixture; a failure says what the check said."""
    r = subprocess.run(compile_command, capture_output=True, text=True, timeout=300)
    if r.returncode:
        raise SystemExit("FAIL: %s does not compile\n%s" % (name, r.stdout + r.stderr))
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    print(r.stdout.strip() or r.stderr.strip())
    if r.returncode:
        raise SystemExit("FAIL: %s\n%s" % (name, r.stdout + r.stderr))


def check_counter(cc, folder, flags, source):
    src, exe = folder / "counter.c", folder / "counter.exe"
    src.write_text(COUNTER_FIXTURE.replace("@@SEND@@", block(source, "static void send_message(CoopSess *s")), encoding="utf-8")
    run_check(cc, "the last message counter of a join", [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "host"), str(src), str(ROOT / "host/coop_proto.c"),
                    str(folder / "monocypher.o"), "-o", str(exe)] + flags, exe)


BIND_FIXTURE = r'''
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32
#define _WIN32 1
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL bind: %s\n", #x); exit(1); } } while (0)
typedef int Sock;
typedef int socklen_t;
typedef int BOOL;
typedef unsigned long DWORD;
typedef unsigned long u_long;
enum { SOCK_NONE = -1, AF_INET6 = 10, SOCK_STREAM = 1, IPPROTO_TCP = 6, IPPROTO_IPV6 = 41,
       SOL_SOCKET = 1, IPV6_V6ONLY = 27, SO_EXCLUSIVEADDRUSE = 100, FIONBIO = 200, FALSE = 0, COOP_MAX_PENDING = 8 };
struct in6_addr { unsigned char bytes[16]; };
static const struct in6_addr in6addr_any = {{0}};
struct sockaddr { unsigned short family; };
struct sockaddr_in6 { unsigned short sin6_family; uint16_t sin6_port; struct in6_addr sin6_addr; };
typedef struct { Sock s; int listening; } CoopTcp;   // (only the two fields tcp_new touches)
static int exclusive, bound, listened, closed, fail_exclusive, fail_bind, fail_listen, fail_nonblocking;
static int net_up(void) { return 1; }
static Sock socket(int family, int kind, int protocol) {
    CHECK(family == AF_INET6 && kind == SOCK_STREAM && protocol == IPPROTO_TCP);
    exclusive = bound = listened = closed = 0;
    return 7;
}
static int setsockopt(Sock s, int level, int option, const char *value, int size) {
    CHECK(s == 7 && !bound && size == sizeof(int));
    if (option == SO_EXCLUSIVEADDRUSE) {
        CHECK(level == SOL_SOCKET && *(const int *)value == 1);
        if (fail_exclusive) return -1;
        exclusive = 1;
    } else CHECK(level == IPPROTO_IPV6 && option == IPV6_V6ONLY && *(const int *)value == 0);
    return 0;
}
static int bind(Sock s, const struct sockaddr *address, int size) {
    CHECK(s == 7 && exclusive && !closed && address && size == sizeof(struct sockaddr_in6));
    bound++;
    return fail_bind ? -1 : 0;
}
static int listen(Sock s, int pending) {
    CHECK(s == 7 && bound && pending == COOP_MAX_PENDING);
    listened++;
    return fail_listen ? -1 : 0;
}
static int getsockname(Sock s, struct sockaddr *address, socklen_t *size) {
    CHECK(s == 7 && bound && *size == sizeof(struct sockaddr_in6));
    ((struct sockaddr_in6 *)address)->sin6_port = 32123;
    return 0;
}
static void sock_close(Sock s) { CHECK(s == 7 && !closed); closed++; }
static uint16_t htons(uint16_t n) { return n; }
static uint16_t ntohs(uint16_t n) { return n; }
static int ioctlsocket(Sock s, int command, u_long *on) {
    CHECK(s == 7 && bound && listened && command == FIONBIO && *on == 1);
    return fail_nonblocking ? -1 : 0;
}
@@LISTEN@@
int main(void) {
    CoopTcp *t = coop_tcp_listen(0);
    CHECK(t && t->listening && exclusive && bound == 1 && listened == 1 && !closed && coop_tcp_port(t) == 32123);
    free(t);
    fail_exclusive = 1;
    CHECK(!coop_tcp_listen(0) && !bound && closed == 1);   // (no ownership: the socket is closed, never bound)
    fail_exclusive = 0, fail_bind = 1;
    CHECK(!coop_tcp_listen(0) && bound == 1 && !listened && closed == 1);
    fail_bind = 0, fail_listen = 1;
    CHECK(!coop_tcp_listen(0) && bound == 1 && listened == 1 && closed == 1);   // (a door that cannot queue joins is closed, not handed on)
    fail_listen = 0, fail_nonblocking = 1;
    CHECK(!coop_tcp_listen(0) && closed == 1);   // (a door that would block its host's thread is closed, not handed on)
    puts("PASS: Windows port ownership precedes bind, listen and the non-blocking call; each failure closes the socket");
    return 0;
}
'''


def check_bind(cc, folder, flags, source):
    src, exe = folder / "bind.c", folder / "bind.exe"
    start = source.index("CoopTcp *coop_tcp_listen(uint16_t port) {")
    end = source.find("\nuint16_t coop_tcp_port(", start)
    if end < 0:
        end = len(source)
    pieces = (block(source, "static int sock_exclusive(Sock s) {") + "\n" +
              block(source, "static int sock_nonblocking(Sock s) {") + "\n" +
              block(source, "static CoopTcp *tcp_new(Sock s, int listening) {") + "\n" +
              source[start:end] + "\n" + block(source, "uint16_t coop_tcp_port(const CoopTcp *t) {"))
    src.write_text(BIND_FIXTURE.replace("@@LISTEN@@", pieces), encoding="utf-8")
    run_check(cc, "the Windows room door", [cc, "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                                            str(src), "-o", str(exe)] + flags, exe)


def run_mode(exe, name, args):
    """One run of the fixture: (name, passed, its output)."""
    started = time.monotonic()
    p = subprocess.run([str(exe)] + args, capture_output=True, text=True, timeout=300, cwd=str(ROOT))
    text = (p.stdout + p.stderr).strip()
    ok = p.returncode == 0 and "coop-session-test: passed" in text
    return name, ok, "--- %s: %s in %.1f s (exit %d)\n%s" % (
        name, "passed" if ok else "FAILED", time.monotonic() - started, p.returncode, text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "gcc")
    parser.add_argument("--only")
    parser.add_argument("--source-root", type=Path, default=ROOT)
    args = parser.parse_args()
    check_wiring()
    with tempfile.TemporaryDirectory(prefix="hookshot-coop-session-") as temp:
        temp = Path(temp)
        mono = temp / "monocypher.o"
        subprocess.run([args.cc, "-std=c99", "-O2", "-w", "-c", str(ROOT / "third_party/monocypher/monocypher.c"),
                        "-o", str(mono)], check=True, timeout=300)
        exe, flags = fixture_flags(args.cc, temp)
        coop_c = (ROOT / "host/coop.c").read_text(encoding="utf-8")
        coop_net = (ROOT / "host/coop_net.c").read_text(encoding="utf-8")
        check_counter(args.cc, temp, flags, coop_c)
        check_bind(args.cc, temp, flags, coop_net)
        runs = [("--unit", "unit"), ("--unit-menu", "unit, menu"), ("--room", "room"),
                ("--hostile", "hostile"), ("--auto", "auto")]
        if args.only:
            runs = [r for r in runs if r[1].startswith(args.only)]
        tsan = compile_fixture(args.cc, temp, ["-fsanitize=thread"], "-tsan")
        if tsan:
            runs = [(a, n + ", ThreadSanitizer") for a, n in runs[:2]] + runs[2:]
        else:
            print("(no ThreadSanitizer runs)")
        with concurrent.futures.ThreadPoolExecutor(len(runs)) as pool:
            results = list(pool.map(lambda r: run_mode(exe, r[1], [r[0]]), runs))
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
