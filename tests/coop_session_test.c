// host/coop.c (co-op's session layer) and host/coop_net.c (its sockets, threads and random bytes), without the game.
// Compiled by tests/test_coop_session.py with host/coop_proto.c and Monocypher, one mode per process.
//
//   coop_session_test --unit
//       1. host/coop_net.c on loopback: a door that owns its port, two links, a frame handed over whole, a frame cut in
//          two, two frames in one read, a length no reader can hold, a closed link, random bytes, the clock, an event,
//          a once, a thread, the addresses a name lookup gives and the text it prints.
//       2. coop_server_parse's cases.
//       3. The process's session switched off (HOS_COOP unset, HOS_COOP=0 over the options file's coop=1): no thread,
//          no socket, no command taken; then switched on and off again by coop_configure with the same counts.
//   coop_session_test --room
//       Three sessions in this process, over real loopback TCP: one holds the room (its own Host), two join it. What
//       the room must do, seen from their views: the maker is member 1, the ids are given once and not given again, a
//       message reaches the others and not its own sender, an event arrives whole and once, a player that leaves says
//       goodbye and is out of the roster, the room's maker can end another player's join, and a session that stops
//       leaves nothing behind.
//   coop_session_test --hostile
//       The same room against a client that means harm: a HELLO of another protocol, a room already full, bytes that
//       are not frames, a frame length no reader can hold, a frame of a kind the room does not route, a join that
//       never says who it is. The room keeps working, and counts what it threw away.
//   coop_session_test --auto
//       The process's session started from the settings the way the game starts it (HOS_COOP=1, HOS_COOP_AUTO=host,
//       HOS_COOP_PORT, HOS_COOP_NAME): it holds the room, a session of its own joins it, one message each way, and
//       coop_shutdown ends the thread and leaves the view saying the game is in no room.
//
// Timings come from CoopParams (host/coop.h), so a mode that waits for a silence waits two seconds, not eight.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "coop_net.h"   // (first: winsock2.h before windows.h)

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include "coop.h"
#include "coop_proto.h"

// host/coop.c's three calls into the game: the log, and the two option readers. What the fixture wants is the session,
// not the game's log file or its hookshot_options.ini.
void hos_log(const char *fmt, ...) {
    if (!getenv("SESS_LOG")) { (void)fmt; return; }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
int hos_ini_option(const char *key, int otherwise) { (void)key; return otherwise; }
int hos_ini_text(const char *key, char *out, size_t n) { (void)key; (void)out; (void)n; return 0; }

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop_session: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)
#define WAIT_FOR(cond, ms) ({ uint64_t t_ = coop_now_ms() + (ms); int ok_; while (!(ok_ = (cond)) && coop_now_ms() < t_) coop_sleep_ms(10); ok_; })

// ---- the session's own thread: coop_sess_step is made for a thread of its own (host/coop.h)
typedef struct { CoopSess *s; volatile int stop; CoopThread th; } Runner;   // (its own thread: nothing written past the runner)

static void run_session(void *arg) {
    Runner *r = (Runner *)arg;
    while (!r->stop) coop_sess_step(r->s);
}

static void start_runner(CoopSess *s, Runner *r) {
    memset(r, 0, sizeof *r);   // (host/coop_net.h: a CoopThread that has not started is zero)
    r->s = s, r->stop = 0;
    if (!coop_thread_start(&r->th, run_session, r)) CHECK(0, "the stepping thread would not start");
}
static void stop_runner(Runner *r) {
    r->stop = 1;
    coop_thread_join(&r->th, 3000);
}

// ---- counting what a process has, on Linux: a session switched off must leave no thread and no socket behind
#ifndef _WIN32
static unsigned count_dir_entries(const char *path) {
    DIR *d = opendir(path);
    if (!d) return 0;
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}
static unsigned count_sockets(void) {
    DIR *d = opendir("/proc/self/fd");
    if (!d) return 0;
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char link[512], target[256];
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t k = readlink(link, target, sizeof target - 1);
        if (k <= 0) continue;
        target[k] = 0;
        if (!strncmp(target, "socket:", 7)) n++;
    }
    closedir(d);
    return n;
}
static unsigned count_threads(void) { return count_dir_entries("/proc/self/task"); }
#endif
// ---- the codecs' limits this layer must respect, and the frame sizes
_Static_assert((int)COOP_TCP_MAX_FRAME == (int)COOP_MAX_FRAME, "host/coop_net.h's largest frame is not host/coop_proto.h's");

// One whole frame off a link, waiting up to ms for it. coop_tcp_readable is what pulls the bytes out of the OS,
// coop_tcp_frame is what joins them into a frame: a reader that only does the second never sees the first.
static int take_frame(CoopTcp *t, void *buf, size_t cap, size_t *out, uint32_t ms) {
    const uint64_t until = coop_now_ms() + ms;
    for (;;) {
        const int ready = coop_tcp_readable(t, 100);
        if (ready < 0) return -1;
        const int whole = coop_tcp_frame(t, buf, cap, out);
        if (whole != 0) return whole;
        if (coop_now_ms() >= until) return 0;
    }
}

/* ---- 1. the operating-system layer ---- */

static void once_body(void *arg) { (*(int *)arg)++; }
static void thread_body(void *arg) { (*(int *)arg)++; }

static void test_net(void) {
    CoopTcp *door = coop_tcp_listen(0);
    CHECK(door != NULL && coop_tcp_port(door) != 0, "no door on a port of its own");
    CoopTcp *again = coop_tcp_listen(coop_tcp_port(door));   // (the port is this room's: a second door on it fails)
    CHECK(again == NULL, "a second door opened on the port that was taken");
    if (again) coop_tcp_close(again);

    CoopAddr to;
    CHECK(coop_net_resolve("127.0.0.1", coop_tcp_port(door), &to), "127.0.0.1 did not resolve");
    char text[64];
    coop_addr_str(&to, text, sizeof text);
    char wanted[64];
    snprintf(wanted, sizeof wanted, "127.0.0.1:%u", coop_tcp_port(door));
    CHECK(!strcmp(text, wanted), "%s: the address did not print as itself (%s)", text, wanted);
    CoopTcp *a = coop_tcp_connect(&to, 2000);
    CHECK(a != NULL, "no link to the door");
    CoopAddr from;
    CoopTcp *b = NULL;
    for (int i = 0; i < 200 && !b; i++) coop_sleep_ms(10), b = coop_tcp_accept(door, &from);
    CHECK(b != NULL, "the door never had the join");
    if (!a || !b) return;

    // A whole frame, both ways.
    uint8_t body[4] = {1, 2, 3, 4};
    CoopFrame f = {COOP_F_DATA, 1, 2, COOP_K_LINE, 4};
    uint8_t frame[COOP_MAX_FRAME], got[COOP_MAX_FRAME + 8];
    size_t n = coop_frame_pack(frame, sizeof frame, &f, body, sizeof body), out = 0;
    CHECK(n == COOP_FRAME_HDR + 4 && coop_tcp_send(a, frame, n), "a frame would not go out");
    CHECK(take_frame(b, got, sizeof got, &out, 2000) == 1, "the frame never arrived");
    CoopFrame g;
    const uint8_t *p;
    CHECK(out == n && coop_frame_parse(got, out, &g, &p) && g.type == COOP_F_DATA && g.len == 4 && p[3] == 4,
          "the frame arrived as something else");
    CHECK(coop_tcp_send(b, frame, n) && take_frame(a, got, sizeof got, &out, 2000) == 1, "no link back");

    // A frame cut in two: the reader waits for the rest, it does not call the halves frames.
    n = coop_frame_pack(frame, sizeof frame, &f, body, sizeof body);
    CHECK(coop_tcp_send(a, frame, 5), "half a header would not go out");
    coop_tcp_readable(b, 100);
    CHECK(coop_tcp_frame(b, got, sizeof got, &out) == 0 && out == 0, "a half frame was handed over as a frame");
    CHECK(coop_tcp_send(a, frame + 5, n - 5), "the rest of it would not go out");
    CHECK(take_frame(b, got, sizeof got, &out, 2000) == 1 && out == n, "the two halves never joined");
    coop_tcp_frame(b, got, sizeof got, &out);   // (whatever the wait brought along, this case does not want it)

    // Two frames in one write: taken one at a time, in the order they were sent.
    CoopFrame fa = {COOP_F_DATA, 1, 2, COOP_K_LINE, 4}, fb = {COOP_F_DATA, 2, 1, COOP_K_LINE, 4};
    uint8_t two[2 * (COOP_FRAME_HDR + 4)];
    const size_t ka = coop_frame_pack(two, sizeof two, &fa, body, sizeof body);
    const size_t kb = coop_frame_pack(two + ka, sizeof two - ka, &fb, body, sizeof body);
    CHECK(ka == n && kb == n && coop_tcp_send(a, two, ka + kb), "two frames would not go out at once");
    size_t first = 0, second = 0;
    CoopFrame g1, g2;
    CHECK(take_frame(b, got, sizeof got, &first, 2000) == 1 && coop_frame_parse(got, first, &g1, &p) && g1.src == 1,
          "the first of two frames never came as itself");
    CHECK(coop_tcp_frame(b, got, sizeof got, &second) == 1 && coop_frame_parse(got, second, &g2, &p) && g2.src == 2,
          "the second of two frames in one write was not taken next");
    CHECK(first == ka && second == kb, "%u and %u: the two frames were not each one frame", (unsigned)first, (unsigned)second);

    // A link that closes says so, once: whoever is reading it is told to stop.
    coop_tcp_close(a);
    CHECK(WAIT_FOR(coop_tcp_readable(b, 100) == -1, 2000), "a closed link did not say it is closed");
    CHECK(coop_tcp_readable(b, 0) == -1, "a closed link answered twice");
    coop_tcp_close(b);

    // A length no reader can hold: the stream is not this room's. The layer cannot tell the reader that in
    // coop_tcp_readable (bytes really are there), so coop_tcp_frame says it, and the reader closes the link.
    a = coop_tcp_connect(&to, 2000);
    b = NULL;
    for (int i = 0; i < 200 && !b; i++) coop_sleep_ms(10), b = coop_tcp_accept(door, &from);
    CHECK(a && b, "no second pair of links to test a lie on");
    if (a && b) {
        uint8_t lie[COOP_FRAME_HDR] = {COOP_F_DATA, 1, 2, 0, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(coop_tcp_send(a, lie, sizeof lie), "the lie would not go out");
        CHECK(take_frame(b, got, sizeof got, &out, 2000) == -1, "a frame longer than the reader was not refused");
        coop_tcp_close(a);
        coop_tcp_close(b);
    }
    coop_tcp_close(door);

    // The other loopback address, and a name.
    CoopAddr six;
    CHECK(coop_net_resolve("::1", 43400, &six) && six.ip[15] == 1 && six.port == 43400, "::1 did not resolve");
    CoopAddr four;
    CHECK(coop_net_resolve("127.0.0.1", 43400, &four) && !coop_addr_eq(&six, &four),
          "::1 and 127.0.0.1 were called the same address: they are joined apart");
    CoopAddr named;
    CHECK(coop_net_resolve("localhost", 43400, &named) && named.port == 43400, "localhost did not resolve with its port");

    // Random bytes, the clock, an event, a once, a thread.
    uint8_t one[16], other[16];
    CHECK(coop_random(one, sizeof one) && coop_random(other, sizeof other) && memcmp(one, other, sizeof one),
          "random bytes repeated");
    const uint64_t t0 = coop_now_ns();
    coop_sleep_ms(30);
    CHECK(coop_now_ns() > t0, "the clock did not move");
    CoopEvent e;
    coop_event_init(&e, 0, 0);
    CHECK(!coop_event_wait_ns(&e, 20000000ull), "an unset event answered");
    coop_event_set(&e);
    CHECK(coop_event_wait_ns(&e, 0), "a set event did not answer");
    // coop_once: a body the process runs once, whoever asks for it.
    static int once_ran;
    CoopOnce o = COOP_ONCE_INIT;
    for (int i = 0; i < 3; i++) coop_once(&o, once_body, &once_ran);
    CHECK(once_ran == 1, "coop_once ran its body %d times", once_ran);

    // A thread that is waited for.
    static int ran;
    CoopThread th;
    memset(&th, 0, sizeof th);
    CHECK(coop_thread_start(&th, thread_body, &ran), "the thread would not start");
    CHECK(coop_thread_join(&th, 3000) && ran == 1, "the thread was not joined (%d)", ran);

    // The addresses a host can show a joiner: numeric, this PC's own, with the port attached. A PC with only the
    // loopback has none, which is the honest answer and not a failure.
    CoopAddr list[8];
    const unsigned found = coop_net_local_addresses(list, 2, 43400);
    CHECK(found <= 2, "it gave %u addresses when asked for 2", found);
    for (unsigned i = 0; i < found; i++) {
        CHECK(list[i].port == 43400, "%u: the port was not attached", i);
        static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1};
        static const uint8_t six_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        CHECK(memcmp(list[i].ip, loopback, 16) != 0 && memcmp(list[i].ip, six_loopback, 16) != 0,
              "%u: a loopback address was offered to a joiner", i);
        char where[64];
        coop_addr_str(&list[i], where, sizeof where);
        CHECK(strchr(where, ':') != NULL, "%u: '%s' cannot be typed back", i, where);
    }
}

/* ---- 2. the address a player types ---- */

static void test_server_parse(void) {
    char host[128];
    uint16_t port;
    struct { const char *text; const char *host; uint16_t port; int ok; } cases[] = {
        {"127.0.0.1", "127.0.0.1", 43400, 1},
        {"127.0.0.1:43500", "127.0.0.1", 43500, 1},
        {"example.com:1", "example.com", 1, 1},
        {"[2001:db8::7]:43410", "2001:db8::7", 43410, 1},
        {"2001:db8::7", "2001:db8::7", 43400, 1},
        {"", "", 0, 0},
        {"  ", "", 0, 0},
        {"127.0.0.1:", "", 0, 0},
        {"127.0.0.1:0", "", 0, 0},
        {"127.0.0.1:65536", "", 0, 0},
        {"127.0.0.1:not a port", "", 0, 0},
        {"[2001:db8::7", "", 0, 0},
        {"a b c", "a b c", 43400, 1},   // (a name with a space in it is the resolver's problem, not this parser's)
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        host[0] = 0, port = 0;
        const int got = coop_server_parse(cases[i].text, host, sizeof host, &port);
        CHECK(got == cases[i].ok, "\"%s\": said %d, wanted %d", cases[i].text, got, cases[i].ok);
        if (cases[i].ok) {
            CHECK(!strcmp(host, cases[i].host) && port == cases[i].port, "\"%s\": %s:%u, wanted %s:%u", cases[i].text,
                  host, port, cases[i].host, cases[i].port);
        }
        // (and every answer fits the buffer it was given: a parser that writes past it would corrupt the caller)
        char small[4];
        small[3] = '#';
        coop_server_parse(cases[i].text, small, 3, &port);
        CHECK(small[3] == '#', "\"%s\": wrote past the caller's buffer", cases[i].text);
    }
    char long_name[300];
    memset(long_name, 'a', sizeof long_name - 1);
    long_name[sizeof long_name - 1] = 0;
    CHECK(!coop_server_parse(long_name, host, sizeof host, &port), "a name longer than the buffer was taken");
}

/* ---- 3. the process's session, switched on and off as the menu does ---- */

static void test_process_session(void) {
#ifndef _WIN32
    const unsigned threads = count_threads(), sockets = count_sockets();
    CoopView v;
    coop_command(COOP_CMD_HOST, NULL);   // (no session: a command is taken by nobody)
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF && !v.hosting && v.self == 0, "a command reached a session that is off");
    CHECK(count_threads() == threads && count_sockets() == sockets,
          "co-op off: %u threads and %u sockets where there were %u and %u", count_threads(), count_sockets(), threads, sockets);

    CHECK(coop_configure(1, "Link", "127.0.0.1"), "co-op would not switch on");
    CHECK(WAIT_FOR(count_threads() == threads + 1, 4000), "the session thread did not start (%u)", count_threads());
    CHECK(count_sockets() >= sockets, "co-op on opened fewer sockets than it had");
    CHECK(coop_command(COOP_CMD_LEAVE, NULL), "a running session refused a command");
    coop_configure(0, NULL, NULL);
    CHECK(WAIT_FOR(count_threads() == threads, 6000), "co-op off left %u threads (was %u)", count_threads(), threads);
    CHECK(count_sockets() == sockets, "co-op off left %u sockets (was %u)", count_sockets(), sockets);
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "the view after switching off says %d", (int)v.state);

    // A stop the thread is too slow for: coop_configure(0) must still finish, and switching on again must work.
    CHECK(coop_configure(1, "Link", NULL), "co-op would not switch on again");
    CHECK(WAIT_FOR(count_threads() == threads + 1, 4000), "the second session thread did not start");
    CHECK(coop_command(COOP_CMD_HOST, NULL), "the host command was not taken");
    CHECK(WAIT_FOR(({ coop_view_get(&v); v.hosting && v.self == 1; }), 8000), "the room was not held (state %d)", (int)v.state);
    coop_view_get(&v);
    CHECK(v.port != 0 && v.room[0], "the room says port %u and id \"%s\"", v.port, v.room);
    coop_shutdown();
    CHECK(WAIT_FOR(count_threads() == threads, 6000), "coop_shutdown left %u threads (was %u)", count_threads(), threads);
    CHECK(count_sockets() == sockets, "coop_shutdown left %u sockets (was %u)", count_sockets(), sockets);
#else
    CoopView v;
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "the process's session is not off to start");
    CHECK(coop_configure(1, "Link", NULL) && coop_command(COOP_CMD_HOST, NULL), "co-op would not switch on");
    CHECK(WAIT_FOR(({ coop_view_get(&v); v.hosting; }), 8000), "the room was not held");
    coop_shutdown();
#endif
}

/* ---- the room ---- */

typedef struct {
    unsigned messages, from_self, maker_events, parts;
    uint64_t seen_id;
    uint8_t seen[COOP_EVENT_MAX];   // (an event can be that long: the fixture keeps what it was told)
    size_t seen_n;
    CoopView last;
} Room;
static Room room;

static void on_message(int from, const uint8_t *bytes, size_t n, void *user) {
    room.messages++;
    if (from == 1 && n >= 1) room.seen[0] = bytes[0];
    if (from == 0) room.from_self++;
    if (user) {   // (and what this session itself wrote, thrown back at it: the room must never do that, or the
                  //  game takes its own place and holds as another player's)
        CoopView v;
        coop_sess_view((CoopSess *)user, &v);
        if (v.state == COOP_JOINED && v.self && from == v.self) room.from_self++;
    }
}
static void on_event(int from, uint64_t session, uint64_t id, const uint8_t *bytes, size_t n, void *user) {
    (void)user; (void)session;
    if (from == 1) room.maker_events++;
    room.seen_id = id;
    room.parts++;
    if (n <= sizeof room.seen) memcpy(room.seen, bytes, n), room.seen_n = n;
}
static void on_view(const CoopView *v, void *user) { (void)user; room.last = *v; }

static CoopParams params(const char *tag, const char *server, const char *name, uint16_t port, int host_wanted) {
    CoopParams p;
    memset(&p, 0, sizeof p);
    p.tag = tag, p.server = server, p.name = name, p.client_id = 7, p.on = 1, p.port = port, p.host_wanted = host_wanted;
    p.ping_ms = 300, p.silence_ms = 2000, p.backoff_first_ms = 200, p.backoff_max_ms = 1000;
    return p;
}

static int view_of(CoopSess *s, CoopView *v) {
    coop_sess_view(s, v);
    return v->state;
}
static int in_room(CoopSess *s) {
    CoopView v;
    return WAIT_FOR(({ view_of(s, &v); v.state == COOP_JOINED; }), 12000);
}
static int left_room(CoopSess *s) {
    CoopView v;
    return WAIT_FOR(({ view_of(s, &v); v.state != COOP_JOINED; }), 12000);
}
// Accept every player the room lists, as the rules layer does: without it the session delivers no event to anyone.
// The room's roster moves under the consent: a view that is no longer the session's current one is refused
// (host/coop.c), so acceptance is given from a fresh view, again until it sticks.
static int accept_view(CoopSess *self, const CoopView *v);
static void accept_all(CoopSess *s) {
    CoopView v;
    CoopSess *self = s;
    for (int try_ = 0; try_ < 40; try_++) {
        if (!WAIT_FOR(({ view_of(self, &v); v.state == COOP_JOINED && v.n >= 1; }), 12000)) break;
        if (accept_view(self, &v)) return;
        coop_sleep_ms(50);
    }
    CHECK(0, "the room's players were never accepted");
}

static int accept_view(CoopSess *self, const CoopView *v) {
    uint32_t mask = 0;
    for (int i = 0; i < v->n; i++) {
        const int id = v->member[i].id;
        // (a player never accepts itself: host/coop.c leaves its own row out, and consent is only ever for the others)
        if (id >= 1 && id <= COOP_MAX_MEMBERS && id != v->self) mask |= 1u << (unsigned)id;
    }
    coop_sess_accept_members(self, v, mask);
    int all = mask != 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if ((mask & (1u << (unsigned)id)) && coop_sess_member_accepted(self, id) != 1) all = 0;
    return all;
}

static void test_room(void) {
    CoopTcp *probe = coop_tcp_listen(0);
    const uint16_t port = probe ? coop_tcp_port(probe) : 43499;
    if (probe) coop_tcp_close(probe);   // (the room's own door must be able to take this port)

    CoopParams pa = params("A", NULL, "Host", port, 0), pb = params("B", NULL, "Saria", 0, 0), pc = params("C", NULL, "Darunia", 0, 0);
    CoopSess *a = coop_sess_new(&pa), *b = coop_sess_new(&pb), *c = coop_sess_new(&pc);
    CHECK(a && b && c, "a session would not be made");
    if (!a || !b || !c) return;
    Runner ra, rb, rc;
    assert(coop_sess_on_message(b, COOP_K_LINE, on_message, b) == 1);
    assert(coop_sess_on_message(c, COOP_K_LINE, on_message, c) == 1);
    assert(coop_sess_on_event(b, COOP_K_TEST, on_event, NULL) == 1);
    assert(coop_sess_on_event(c, COOP_K_TEST, on_event, NULL) == 1);
    assert(coop_sess_on_view(a, on_view, NULL) == 1);
    start_runner(a, &ra);
    CoopParams hb = pb;   // (the room's address, as a player types it)
    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", port);
    hb.server = addr;
    pc.server = addr;
    coop_sess_configure(b, &hb);
    coop_sess_configure(c, &pc);
    start_runner(b, &rb);
    start_runner(c, &rc);

    CHECK(coop_sess_command(a, COOP_CMD_HOST, NULL) == 1, "the room would not be hosted on port %u", port);
    CHECK(in_room(a), "the host is not in its own room");
    CHECK(coop_sess_command(b, COOP_CMD_JOIN, addr) == 1, "the join command was not taken");
    CHECK(coop_sess_command(c, COOP_CMD_JOIN, addr) == 1, "the second join command was not taken");
    CHECK(in_room(b) && in_room(c), "the joins did not reach the room");

    CoopView va, vb, vc;
    view_of(a, &va), view_of(b, &vb), view_of(c, &vc);
    CHECK(va.self == 1 && va.maker == 1 && va.hosting && vb.self != 1 && vc.self != 1 && vb.self != vc.self,
          "ids: host %d maker %d hosting %d, others %d and %d", va.self, va.maker, va.hosting, vb.self, vc.self);
    CHECK(!strcmp(va.room, vb.room) && !strcmp(va.room, vc.room) && strlen(va.room) == COOP_ROOM_TEXT,
          "the room id each one was told: \"%s\" \"%s\" \"%s\"", va.room, vb.room, vc.room);
    CHECK(va.n == 3 && vb.n == 3 && vc.n == 3, "the roster: %d, %d, %d", va.n, vb.n, vc.n);
    CHECK(va.maker == vb.maker && vb.maker == 1, "the maker is not the same member everywhere");
    const int id_c = vc.self;   // (the id a player is given is its own for this join: see the leaving case)
    accept_all(a), accept_all(b), accept_all(c);

    // Messages: to everyone, and to one player; a sender does not hear its own.
    const uint8_t hello[3] = {0x41, 0x42, 0x43};
    CHECK(coop_sess_send(a, COOP_DST_ALL, COOP_K_LINE, hello, 1) == 1, "the host's message was not queued");
    CHECK(WAIT_FOR(room.messages >= 2, 6000), "the message reached %u players", room.messages);
    CHECK(room.from_self == 0, "a player heard its own message");
    room.messages = 0;
    CHECK(coop_sess_send(b, COOP_DST_ALL, COOP_K_LINE, hello, 1) == 1, "a joiner could not write to the room");
    CHECK(WAIT_FOR(room.messages >= 1, 6000), "the joiner's message did not reach the other player");
    (void)WAIT_FOR(0, 400);   // (an echo, if the room makes one, rides the same relay: give it time to turn up)
    CHECK(room.from_self == 0, "the room threw a joiner its own message back");   // (the place and holds lines are
                                                                                   //  written exactly this way: a
                                                                                   //  game hearing its own line draws
                                                                                   //  itself as a second player)
    room.messages = 0;
    const uint8_t ping[1] = {0x7f};
    CHECK(coop_sess_send(b, id_c, COOP_K_LINE, ping, 1) == 1, "a joiner could not write to another");
    CHECK(WAIT_FOR(room.messages >= 1, 6000), "a joiner's message did not arrive");
    CHECK(coop_sess_send(b, 9, COOP_K_LINE, ping, 1) == 0, "a message to member 9 was taken");

    // An event, whole and once, from the room's maker to everyone.
    uint8_t big[4096];
    for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 31u + 7);
    room.maker_events = 0, room.parts = 0;
    CHECK(coop_sess_send_event(a, COOP_DST_ALL, COOP_K_TEST, big, sizeof big) == COOP_EVENT_QUEUED, "no room for the event");
    CHECK(WAIT_FOR(room.maker_events >= 2 && room.parts >= 2, 12000), "the event arrived %u times for the maker's",
          room.maker_events);
    CHECK(room.seen_n == sizeof big && !memcmp(room.seen, big, sizeof big), "the event arrived as %u bytes", (unsigned)room.seen_n);

    // A player leaves: its goodbye is what the room learns it by, and its id is not given to someone else.
    room.messages = 0;
    CHECK(coop_sess_command(b, COOP_CMD_LEAVE, NULL) == 1, "a player could not leave");
    CHECK(WAIT_FOR(({ view_of(a, &va); va.n == 2; }), 8000), "the room still lists %d players", va.n);
    view_of(c, &vc);
    CHECK(vc.self == id_c, "the player that stayed was renumbered %d (was %d)", vc.self, id_c);
    view_of(b, &vb);
    CHECK(vb.state != COOP_JOINED && vb.self == 0, "the player that left is in a room still (%d, id %d)",
          (int)vb.state, vb.self);

    // The room's maker ends another player's join.
    CHECK(coop_sess_remove(a, id_c) == 1, "the maker could not remove member %d", id_c);
    CHECK(WAIT_FOR(({ view_of(a, &va); va.n == 1; }), 8000), "the room still lists %d after a removal", va.n);
    CHECK(left_room(c), "the removed player is still in the room");
    CHECK(coop_sess_remove(b, 1) == 0, "a player that is not the maker removed someone");

    // The room's door is not a suggestion: joining a room that is not there is refused, not hung on.
    CoopSess *d = coop_sess_new(&pa);
    CHECK(d != NULL, "no session for the lonely joiner");
    if (d) {
        CoopParams hd = params("D", "127.0.0.1:1", "Lonely", 0, 0);
        hd.retry = 0;
        coop_sess_configure(d, &hd);
        Runner rd;
        start_runner(d, &rd);
        CHECK(coop_sess_command(d, COOP_CMD_JOIN, "127.0.0.1:1") == 1, "the join command was not taken");
        CHECK(WAIT_FOR(({ view_of(d, &vc); vc.state == COOP_LEFT || vc.status[0]; }), 8000), "the lonely join said nothing");
        view_of(d, &vc);
        CHECK(vc.state != COOP_JOINED, "a join to nothing joined a room");
        coop_sess_stop(d);
        CHECK(coop_sess_wait_off(d, 6000), "the lonely join would not stop");
        stop_runner(&rd);
        coop_sess_free(d);
    }

    // Stopping a session: the view says off before the waiter wakes, and the room notices.
    coop_sess_stop(a), coop_sess_stop(b), coop_sess_stop(c);
    CHECK(coop_sess_wait_off(a, 6000) && coop_sess_wait_off(b, 6000) && coop_sess_wait_off(c, 6000), "a session would not stop");
    stop_runner(&ra), stop_runner(&rb), stop_runner(&rc);
    view_of(a, &va), view_of(b, &vb);
    CHECK(va.state == COOP_OFF && vb.state == COOP_OFF, "after a stop the views say %d and %d", (int)va.state, (int)vb.state);
    coop_sess_free(a), coop_sess_free(b), coop_sess_free(c);
}

/* ---- a client that means harm ---- */

static CoopTcp *client_to(uint16_t port) {
    CoopAddr to;
    if (!coop_net_resolve("127.0.0.1", port, &to)) return NULL;
    return coop_tcp_connect(&to, 2000);
}

static void test_hostile(void) {
    CoopTcp *probe = coop_tcp_listen(0);
    const uint16_t port = probe ? coop_tcp_port(probe) : 43498;
    if (probe) coop_tcp_close(probe);

    CoopParams pa = params("A", NULL, "Host", port, 0);
    CoopSess *a = coop_sess_new(&pa);
    if (!a) return;
    Runner ra;
    start_runner(a, &ra);
    CHECK(coop_sess_command(a, COOP_CMD_HOST, NULL) == 1, "no room to attack");
    CoopView va;
    CHECK(in_room(a), "the room did not hold");

    uint8_t got[COOP_MAX_FRAME + 8];
    size_t n = 0;
    CoopFrame f;
    const uint8_t *p;

    // A HELLO of another protocol: an error that says why, and a closed link.
    CoopTcp *x = client_to(port);
    CHECK(x != NULL, "no link to the room");
    if (x) {
        CoopHello h;
        memset(&h, 0, sizeof h);
        h.proto = COOP_PROTO + 1, h.session = 5;
        strcpy(h.name, "Liar");
        uint8_t body[COOP_MAX_PAYLOAD], frame[COOP_MAX_FRAME];
        size_t k = coop_hello_pack(body, sizeof body, &h);
        CoopFrame g = {COOP_F_HELLO, 0, 0, 0, 0};
        k = coop_frame_pack(frame, sizeof frame, &g, body, k);
        CHECK(k && coop_tcp_send(x, frame, k), "the wrong HELLO would not go out");
        int error = 0;
        for (int i = 0; i < 100 && !error; i++) {
            coop_sleep_ms(20);
            if (coop_tcp_readable(x, 0) > 0 && coop_tcp_frame(x, got, sizeof got, &n) == 1 &&
                coop_frame_parse(got, n, &f, &p) && f.type == COOP_F_ERROR) {
                CoopError e;
                error = coop_error_parse(p, f.len, &e) && e.reason == COOP_E_PROTO;
            }
        }
        CHECK(error, "a HELLO of another protocol was not answered with its own error");
        coop_tcp_close(x);
    }

    // Bytes that are not frames, and a length no reader can hold: the room turns them away and stays itself.
    x = client_to(port);
    if (x) {
        static const char garbage[] = "hello, I am not co-op";
        CHECK(coop_tcp_send(x, garbage, sizeof garbage), "the garbage would not go out");
        coop_sleep_ms(400);
        view_of(a, &va);
        CHECK(va.state == COOP_JOINED && va.hosting, "the room stopped being a room (%d)", (int)va.state);
        CHECK(WAIT_FOR(coop_tcp_readable(x, 100) != 0, 3000), "the room left a join that spoke nonsense hanging open");
        coop_tcp_close(x);
    }
    x = client_to(port);
    if (x) {
        uint8_t lie[COOP_FRAME_HDR] = {COOP_F_DATA, 1, 2, 0, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(coop_tcp_send(x, lie, sizeof lie), "the lie would not go out");
        coop_sleep_ms(600);
        view_of(a, &va);
        CHECK(va.state == COOP_JOINED, "the room stopped being a room after a frame length no reader can hold (%d)", (int)va.state);
        CHECK(WAIT_FOR(coop_tcp_readable(x, 100) < 0, 3000), "the room kept a stream that is not co-op's open");
        coop_tcp_close(x);
    }

    // A join that never says who it is: the room stops waiting for it.
    x = client_to(port);
    if (x) {
        coop_sleep_ms(4000);   // (host/coop.c: HELLO_WAIT_MS)
        view_of(a, &va);
        CHECK(va.state == COOP_JOINED && va.n == 1, "a silent connection became a member (%d players)", va.n);
        coop_tcp_close(x);
    }

    // A full room: the ninth game is told the room is full.
    CoopTcp *fill[COOP_MAX_MEMBERS + 1];
    fill[0] = NULL;   // (the host itself is member 1: COOP_MAX_MEMBERS - 1 more joins fill it)
    for (int i = 2; i <= COOP_MAX_MEMBERS; i++) {
        CoopTcp *t = client_to(port);
        if (!t) break;
        CoopHello h;
        memset(&h, 0, sizeof h);
        h.proto = COOP_PROTO, h.session = (uint64_t)(9000 + i);
        strcpy(h.name, "abc");
        uint8_t body[COOP_MAX_PAYLOAD], frame[COOP_MAX_FRAME];
        size_t k = coop_hello_pack(body, sizeof body, &h);
        CoopFrame g = {COOP_F_HELLO, 0, 0, 0, 0};
        k = coop_frame_pack(frame, sizeof frame, &g, body, k);
        if (k) coop_tcp_send(t, frame, k);
        fill[i] = t;
    }
    CHECK(WAIT_FOR(({ view_of(a, &va); va.n == COOP_MAX_MEMBERS; }), 12000), "the room holds %d of %d", va.n, COOP_MAX_MEMBERS);
    CoopTcp *over = client_to(port);
    if (over) {
        CoopHello h;
        memset(&h, 0, sizeof h);
        h.proto = COOP_PROTO, h.session = 4242;
        strcpy(h.name, "too");
        uint8_t body[COOP_MAX_PAYLOAD], frame[COOP_MAX_FRAME];
        size_t k = coop_hello_pack(body, sizeof body, &h);
        CoopFrame g = {COOP_F_HELLO, 0, 0, 0, 0};
        k = coop_frame_pack(frame, sizeof frame, &g, body, k);
        CHECK(k && coop_tcp_send(over, frame, k), "the over-full HELLO would not go out");
        int full = 0;
        for (int i = 0; i < 100 && !full; i++) {
            coop_sleep_ms(20);
            if (coop_tcp_readable(over, 0) > 0 && coop_tcp_frame(over, got, sizeof got, &n) == 1 &&
                coop_frame_parse(got, n, &f, &p) && f.type == COOP_F_ERROR) {
                CoopError e;
                full = coop_error_parse(p, f.len, &e) && e.reason == COOP_E_ROOM_FULL;
            }
        }
        CHECK(full, "the ninth join was not told the room is full");
        coop_tcp_close(over);
    }
    for (int i = 2; i <= COOP_MAX_MEMBERS; i++)
        if (fill[i]) coop_tcp_close(fill[i]);

    // The room is still a room after all that, and it counts what it threw away.
    view_of(a, &va);
    CHECK(va.state == COOP_JOINED && va.hosting && va.self == 1, "the room did not survive: state %d", (int)va.state);
    CHECK(WAIT_FOR(({ view_of(a, &va); va.state == COOP_JOINED && va.n == 1; }), 8000),
          "the room did not return to itself after all of that (state %d, %d players)", (int)va.state, va.n);

    coop_sess_stop(a);
    CHECK(coop_sess_wait_off(a, 6000), "the room would not stop");
    stop_runner(&ra);
    coop_sess_free(a);
}

/* ---- the process's session, started from the settings ---- */

#ifndef _WIN32
static void test_auto(void) {
    // A port of its own, and a different one for the move: a room that held this port a moment ago has its accepted
    // links in the OS's TIME_WAIT, and a room that wants the port it just had is refused (host/coop_net.c).
    CoopTcp *probe = coop_tcp_listen(0);
    const uint16_t port = coop_tcp_port(probe);
    coop_tcp_close(probe);
    probe = coop_tcp_listen(0);
    const uint16_t moved = coop_tcp_port(probe);
    coop_tcp_close(probe);
    char port_text[16], moved_text[16], joined[64];
    snprintf(port_text, sizeof port_text, "%u", port);
    snprintf(moved_text, sizeof moved_text, "%u", moved);
    snprintf(joined, sizeof joined, "127.0.0.1:%u", port);
    setenv("HOS_COOP", "1", 1);
    setenv("HOS_COOP_AUTO", "host", 1);
    setenv("HOS_COOP_PORT", port_text, 1);
    setenv("HOS_COOP_NAME", "Link", 1);
    const unsigned threads = count_threads(), sockets = count_sockets();
    coop_init();
    CoopView v;
    CHECK(WAIT_FOR(({ coop_view_get(&v); v.hosting && v.state == COOP_JOINED && v.self == 1; }), 10000),
          "the settings did not start a room (state %d hosting %d)", (int)v.state, v.hosting);
    coop_view_get(&v);
    CHECK(v.port == port, "the room is on port %u, not %u", v.port, port);
    CHECK(!strcmp(v.name, "Link"), "the room's player is named \"%s\"", v.name);
    CHECK(count_threads() == threads + 1, "the session thread count is %u (was %u)", count_threads(), threads);
    CHECK(count_sockets() > sockets, "the room opened no socket");

    // A session of its own joins the room the settings opened, and one message goes each way.
    CoopParams pj = params("J", joined, "Saria", 0, 0);
    CoopSess *j = coop_sess_new(&pj);
    assert(coop_sess_on_message(j, COOP_K_LINE, on_message, NULL) == 1);
    Runner rj;
    start_runner(j, &rj);
    CHECK(coop_sess_command(j, COOP_CMD_JOIN, joined) == 1, "the join was not taken");
    CHECK(in_room(j), "the joiner never reached the room the settings opened");
    room.messages = 0;
    const uint8_t one[1] = {0x2b};
    CHECK(coop_send_to(2, COOP_K_LINE, one, 1) == 1, "the room could not write to member 2");
    CHECK(WAIT_FOR(room.messages >= 1, 6000), "the room's message did not arrive");
    CoopView vj;
    view_of(j, &vj);
    CHECK(vj.n == 2 && vj.maker == 1, "the room the settings opened lists %d players, maker %d", vj.n, vj.maker);

    // The menu's Port row: changing the port closes this room and opens it again on the new one.
    coop_set_port(moved);
    CHECK(WAIT_FOR(({ coop_view_get(&v); v.hosting && v.port == moved && v.state == COOP_JOINED && v.self == 1; }), 20000),
          "the room did not hold itself again on port %u (state %d, id %d)", v.port, (int)v.state, v.self);

    coop_sess_stop(j);
    CHECK(coop_sess_wait_off(j, 6000), "the joiner would not stop");
    stop_runner(&rj);
    coop_sess_free(j);

    coop_shutdown();
    CHECK(WAIT_FOR(count_threads() == threads, 6000), "coop_shutdown left %u threads (was %u)", count_threads(), threads);
    CHECK(count_sockets() == sockets, "coop_shutdown left %u sockets (was %u)", count_sockets(), sockets);
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF && !v.hosting && v.self == 0 && v.n == 0,
          "after coop_shutdown the view says state %d hosting %d self %d n %d", (int)v.state, v.hosting, v.self, v.n);
}
#endif

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s --unit | --unit-menu | --room | --hostile | --auto\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "--unit")) {
        test_net();
        test_server_parse();
        test_process_session();
    } else if (!strcmp(argv[1], "--unit-menu")) {
        test_process_session();
    } else if (!strcmp(argv[1], "--room")) {
        test_room();
    } else if (!strcmp(argv[1], "--hostile")) {
        test_hostile();
    } else if (!strcmp(argv[1], "--auto")) {
#ifndef _WIN32
        test_auto();
#else
        puts("(--auto is a Linux mode: it sets HOS_COOP in the process's own environment)");
#endif
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    if (fails) {
        fprintf(stderr, "coop-session-test: %d checks failed\n", fails);
        return 1;
    }
    puts("coop-session-test: passed");
    return 0;
}
