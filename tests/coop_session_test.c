// host/coop.c, co-op's session layer, and host/coop_net.c, its operating-system layer, without the game: compiled by
// tests/test_coop_session.py with host/coop_proto.c and Monocypher, one mode per process.
//
//   coop_session_test --unit
//       1. coop_net.c on loopback: addresses, a port one listen holds, frames over a link (a half frame, a header that
//          is not one, two frames from one read, a connection knocking on the listener), random bytes, events, a once,
//          the clock.
//       2. coop_server_parse's cases.
//       3. A stop wakes its waiter only once the view that says "off" is published.
//       4. The process's session switched off (HOS_COOP unset, HOS_COOP=0 over the options file's coop=1): no thread, no
//          socket, no command taken; then switched on by the options file alone, with no host given, which hosts on the
//          port it is given; then the game closing, which ends the thread.
//   coop_session_test --unit-menu
//       Co-op off at start and switched on and off by coop_configure, as the menu will: off, on, off cycles with a link
//       open (a host that never answers), each on one thread and one socket more and each off none left (counted in
//       /proc/self/task and /proc/self/fd); a stop the thread is too slow for, which it carries out by itself, or which
//       switching on again takes back.
//   coop_session_test --mock [--drop N] [--work DIR]
//       Sessions in this process, one of them hosting on a port of its own and the others reaching it directly: a room
//       hosted and joined, with and without its secret, a second game that cannot take the port, messages both ways, a
//       member whose link closed abruptly coming back with the member id it had (and its spent token refused), a room
//       hosted again, a host that lies (what a hostile host can do: garbage, a frame of another version, every refusal
//       it can send, a message it seals itself, a recorded message sent again, forged rosters), the process's session
//       started by coop_init from the settings (the environment over the options file) with its commands and
//       coop_shutdown, on Linux a child process in the room ended by SIGTERM, which says nothing: the host drops its
//       link within a few tens of ms (--work: where its log goes), the maker removing a member, a host that is not
//       there. --drop N: each session throws away N % of the frames it receives.
//   coop_session_test --hostile
//       Only the hostile host's scenarios, one hostile port at a time: what a host does to a player that trusts it when
//       it answers nothing, sends bytes that are not a frame, a frame of another version, every refusal it has, a
//       message it seals with a foreign key or one it has already sent, or a roster that makes the player the maker.
//   coop_session_test --signal-child   (the child of --mock: joins through coop_init and waits for the signal)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "coop_net.h"   // (first: winsock2.h before windows.h)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#ifdef _WIN32
typedef SOCKET TestSocket;
typedef int TestSocklen;
#define TEST_SOCKET_NONE INVALID_SOCKET
#define test_socket_close closesocket
#else
typedef int TestSocket;
typedef socklen_t TestSocklen;
#define TEST_SOCKET_NONE (-1)
#define test_socket_close close
#endif

#include "coop.h"
#include "coop_proto.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop_session: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)
#define FAIL_RET(...) do { CHECK(0, __VA_ARGS__); return; } while (0)
#define WAIT_FOR(cond, ms) ({ uint64_t t_ = coop_now_ms() + (ms); int ok_; while (!(ok_ = (cond)) && coop_now_ms() < t_) coop_sleep_ms(10); ok_; })
// A member learns of a join or a leave from the host's roster, and --drop N throws away N % of what a session receives:
// the host publishes a roster with every change and sends the last one again every 5 s, so this wait holds several of
// them.
#define ROSTER_WAIT_MS 40000
// With no loss a roster step has no dropped frame to wait for, so it is bounded as the code promises: a session listed
// elsewhere settles within one backoff (500 ms here) and the rosters after it, and after a roster forged a little ahead
// the host's are taken again within the silence time (1.5 s here) and 4 s, plus 1 s for the host's next roster and 1 s
// for a loaded machine.
#define SETTLE_NO_LOSS_MS 6000
#define STALE_NO_LOSS_MS (1500 + 4000 + 1000 + 1000)
// A link closed without a goodbye is noticed by the host when its wait ends (a few tens of ms), not after the silence
// time: a game that is killed is out of the room long before the 1.5 s this fixture uses for a lost link.
#define LINK_GONE_MS 2000
// With the test's loss the roster that says a link closed is one of the frames thrown away, so the room sees it on one of
// the host's repeats (its last roster again every 5 s); the bound stays under the host's own silence time, so it is still
// the closed link that ends the member, not the silence.
#define LINK_GONE_AGAIN_MS 12000
// A raw player of the fixture waits for the host's answer the way a session does: its CONNECT again every so often.
#define RAW_CONNECT_AGAIN_MS 200

static const char ROOM_SECRET[] = "H7K3WMBCDFGH234567";   // tests/coop_vectors.txt's room

static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");   // ("" removes it)
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

// ---- fixture threads whose userspace and kernel exits can both be waited for

typedef struct {
    void (*fn)(void *);
    void *arg;
#ifdef __linux__
    pid_t tid;
#endif
} Start;
#ifdef _WIN32
typedef HANDLE Thread;
static DWORD WINAPI thread_main(void *p) {
    Start s = *(Start *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}
static Thread thread_new(void (*fn)(void *), void *arg) {
    Start *s = malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    return CreateThread(NULL, 0, thread_main, s, 0, NULL);
}
static void thread_join(Thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
typedef struct { pthread_t h; Start *start; } Thread;
static void *thread_main(void *p) {
    Start *s = p;
#ifdef __linux__
    s->tid = gettid();
#endif
    s->fn(s->arg);
    return NULL;
}
static Thread thread_new(void (*fn)(void *), void *arg) {
    Start *s = malloc(sizeof *s);
    s->fn = fn, s->arg = arg;
    Thread t = {.start = s};
    if (pthread_create(&t.h, NULL, thread_main, s)) abort();
    return t;
}
static void thread_join(Thread t) {
    if (pthread_join(t.h, NULL)) abort();
#ifdef __linux__
    // pthread_join (notably under TSan) can return while this task still appears in /proc.
    // Wait for this TID, not for a total that another exiting thread could also satisfy.
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%ld", (long)t.start->tid);
    CHECK(WAIT_FOR(access(path, F_OK) < 0 && errno == ENOENT, 5000),
          "joined fixture thread %ld left /proc/self/task before the baseline count", (long)t.start->tid);
#endif
    free(t.start);
}
#endif

// ---- what host/coop.c calls of the game: the log and hookshot_options.ini

static CoopLock log_lock;
static char *log_text;            // every line, each ended by '\n'
static size_t log_len, log_cap;
static int log_echo;              // each line on stdout as it comes (COOP_TEST_VERBOSE; the child's log)

void hos_log(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof line) n = (int)sizeof line - 1;
    coop_lock(&log_lock);
    if (log_len + (size_t)n + 2 > log_cap) {
        size_t cap = log_cap ? log_cap * 2 : 1 << 16;
        while (cap < log_len + (size_t)n + 2) cap *= 2;
        char *grown = realloc(log_text, cap);
        if (!grown) abort();
        log_text = grown, log_cap = cap;
    }
    memcpy(log_text + log_len, line, (size_t)n);
    log_len += (size_t)n;
    log_text[log_len++] = '\n';
    log_text[log_len] = 0;
    if (log_echo) printf("%s\n", line), fflush(stdout);
    coop_unlock(&log_lock);
}

void hos_log_flush(void) { fflush(stdout); }

static int log_count(const char *text) {
    int n = 0;
    coop_lock(&log_lock);
    for (const char *p = log_text; p && (p = strstr(p, text)); p += strlen(text)) n++;
    coop_unlock(&log_lock);
    return n;
}

// The options file as host/nn_applet.c reads it: key=value pairs the test sets
static struct { char key[32], value[160]; } ini[8];
static int ini_n;

static void ini_set(const char *key, const char *value) {
    for (int i = 0; i < ini_n; i++)
        if (!strcmp(ini[i].key, key)) {
            if (value) snprintf(ini[i].value, sizeof ini[i].value, "%s", value);
            else ini[i] = ini[--ini_n];
            return;
        }
    if (value && ini_n < 8) {
        snprintf(ini[ini_n].key, sizeof ini[ini_n].key, "%s", key);
        snprintf(ini[ini_n].value, sizeof ini[ini_n].value, "%s", value);
        ini_n++;
    }
}

int hos_ini_text(const char *key, char *out, size_t size) {
    for (int i = 0; i < ini_n; i++)
        if (!strcmp(ini[i].key, key)) {
            snprintf(out, size, "%s", ini[i].value);
            return 1;
        }
    return 0;
}

int hos_ini_option(const char *key, int otherwise) {
    char text[160] = "";
    return hos_ini_text(key, text, sizeof text) ? atoi(text) : otherwise;
}

// The process's threads and open files (Linux; 0 and 0 elsewhere), for "one more" and "none left".
static int count_entries(const char *dir) {
#ifdef __linux__
    DIR *d = opendir(dir);
    int n = 0;
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));) n += e->d_name[0] != '.';
    closedir(d);
    return n;
#else
    (void)dir;
    return 0;
#endif
}
static int tasks(void) { return count_entries("/proc/self/task"); }
static int files(void) { return count_entries("/proc/self/fd"); }

// ---- 1. the operating-system layer

typedef struct { int calls; } OnceCount;
static void once_fn(void *arg) { ((OnceCount *)arg)->calls++; }

// Two threads at one once (the game's threads all reach the process's session through one): it runs once, and the
// callers write nothing they share (ThreadSanitizer's run watches that).
static CoopOnce shared_once = COOP_ONCE_INIT;
static OnceCount shared_count;
static void once_many(void *arg) {
    (void)arg;
    for (int i = 0; i < 2000; i++) coop_once(&shared_once, once_fn, &shared_count);
}

static uint16_t free_port(void) {
    CoopTcpListen *l = coop_tcp_listen(0);
    uint16_t port = l ? coop_tcp_listen_port(l) : 0;
    coop_tcp_listen_close(l);
    return port;
}

// The port a listen holds is its own: no socket of this test can bind it beside it, and no second listen can take it
// (a game hosting on a port another game hosts on is refused, which is what the sessions check below reads as a line
// of its log). Windows is covered by tests/test_coop_session.py, which reads the options coop_tcp_listen sets before
// it binds: SO_EXCLUSIVEADDRUSE first, so no same-user socket can share the port there either.
static void check_listen_ownership(CoopTcpListen *owner) {
    const uint16_t port = coop_tcp_listen_port(owner);
    for (int reuse = 0; reuse < 2; reuse++) {
        TestSocket probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        CHECK(probe != TEST_SOCKET_NONE, "open the IPv4 bind probe");
        if (probe == TEST_SOCKET_NONE) continue;
        if (reuse)
            CHECK(setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof reuse) == 0,
                  "set reuse on the competing socket");
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = reuse ? htonl(INADDR_ANY) : htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        CHECK(bind(probe, (struct sockaddr *)&address, sizeof address) != 0,
              "%s cannot take the port a listen holds (reuse %d)", reuse ? "a reuse wildcard bind" : "a specific IPv4 bind", reuse);
        test_socket_close(probe);
    }
    TestSocket first = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(first != TEST_SOCKET_NONE, "open the first-bound IPv4 owner");
    if (first == TEST_SOCKET_NONE) return;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int bound = bind(first, (struct sockaddr *)&address, sizeof address) == 0;
    CHECK(bound, "bind the specific IPv4 owner to a free port");
    if (bound) {
        TestSocklen size = sizeof address;
        int named = getsockname(first, (struct sockaddr *)&address, &size) == 0;
        CHECK(named && address.sin_port, "read the specific IPv4 owner's port");
        if (named) {
            CoopTcpListen *overlap = coop_tcp_listen(ntohs(address.sin_port));
            CHECK(!overlap, "a dual-stack listen cannot take a port an IPv4 socket holds");
            coop_tcp_listen_close(overlap);
        }
    }
    test_socket_close(first);
}

// A link of this test's own, so that what coop_tcp_next does with a frame is seen without a session in the way.
static void check_net(void) {
    CoopAddr a, b, c;
    char text[64];
    CHECK(coop_net_resolve("127.0.0.1", 4000, &a), "resolve 127.0.0.1");
    coop_addr_str(&a, text, sizeof text);
    CHECK(!strcmp(text, "127.0.0.1:4000"), "127.0.0.1:4000 reads %s", text);
    CHECK(coop_net_resolve("::1", 5, &b), "resolve ::1");
    coop_addr_str(&b, text, sizeof text);
    CHECK(!strcmp(text, "[::1]:5"), "[::1]:5 reads %s", text);
    CHECK(coop_net_resolve("::ffff:127.0.0.1", 4000, &c) && coop_addr_eq(&a, &c), "an IPv4-mapped address is the same address");
    CHECK(!coop_addr_eq(&a, &b), "IPv4 and IPv6 loopback differ");
    c = a, c.port++;
    CHECK(!coop_addr_eq(&a, &c), "ports differ");
    CHECK(!coop_net_resolve("", 1, &c) && !coop_net_resolve(NULL, 1, &c), "no name resolves to nothing");

    CoopTcpListen *l1 = coop_tcp_listen(0), *l2 = coop_tcp_listen(0);
    CHECK(l1 && l2, "two listens open");
    if (!l1 || !l2) return;
    const uint16_t p1 = coop_tcp_listen_port(l1), p2 = coop_tcp_listen_port(l2);
    CHECK(p1 && p2 && p1 != p2, "bound to two free ports");
    CoopTcpListen *taken = coop_tcp_listen(p1);
    CHECK(!taken, "a port a listen holds cannot be listened on again");
    coop_tcp_listen_close(taken);
    CHECK(!coop_tcp_listen(p2), "and the other listen still holds its port");
    check_listen_ownership(l1);
    coop_tcp_listen_close(l1);
    coop_tcp_listen_close(l2);
    CoopTcpListen *again = coop_tcp_listen(p1);
    CHECK(again != NULL, "the port is free again once its listen is closed");
    coop_tcp_listen_close(again);

    // a link of our own: two whole frames, then a half one, then a header that is not one
    CoopTcpListen *l = coop_tcp_listen(0);
    if (!l) FAIL_RET("a listen for the link tests");
    CoopAddr to;
    if (!coop_net_resolve("127.0.0.1", coop_tcp_listen_port(l), &to)) FAIL_RET("resolve the listen's address");
    coop_addr_str(&to, text, sizeof text);
    CHECK(!strncmp(text, "127.0.0.1:", 10), "the listen's address reads %s", text);
    uint8_t buf[COOP_MAX_DGRAM], msg[1500];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_PING, .src = 1, .dst = 2, .room = 7, .seq = 3};
    size_t n1 = coop_dgram_pack(buf, sizeof buf, &h, msg, 300, NULL);
    h.seq = 4;
    size_t n2 = coop_dgram_pack(buf + n1, sizeof buf - n1, &h, msg + 300, 20, NULL);

    CoopTcp *cli = coop_tcp_connect(&to, 2000);
    CoopTcp *srv = NULL;
    for (int i = 0; i < 200 && !srv; i++) {
        srv = coop_tcp_accept(l, NULL);
        if (!srv) coop_sleep_ms(10);
    }
    CHECK(cli && srv, "connect and accept");
    if (!cli || !srv) FAIL_RET("no link for the frame tests");
    CoopTcpSet *set = coop_tcp_set_new(NULL, 4);
    const int slot = coop_tcp_add(set, srv);
    CHECK(slot == 1, "a connection takes a slot (%d)", slot);
    CHECK(coop_tcp_send(cli, buf, n1) && coop_tcp_send(cli, buf + n1, n2), "send two frames");
    int from = 0;
    int n = coop_tcp_next(set, buf, sizeof buf, &from, 1000);
    CHECK(n == (int)n1 && from == slot, "the first frame arrives whole (%d bytes, slot %d)", n, from);
    CoopHdr rh;
    const uint8_t *pp;
    CHECK(coop_dgram_parse(buf, (size_t)n, &rh, &pp) && rh.seq == 3 && rh.len == 300, "and says what was sent (seq %u, %u bytes)", rh.seq, rh.len);
    n = coop_tcp_next(set, buf, sizeof buf, &from, 1000);
    CHECK(n == (int)n2 && from == slot, "the second follows from the same read (%d bytes, slot %d)", n, from);
    CHECK(coop_tcp_next(set, buf, 100, &from, 0) == 0, "a buffer smaller than a frame takes nothing");
    CHECK(coop_tcp_alive(coop_tcp_at(set, slot)), "the link is still up");
    coop_tcp_close(cli);
    coop_tcp_drop(set, slot);
    coop_tcp_set_free(set);

    // A frame split over the wire arrives as one frame, and the wait for the rest of it is its full length.
    CoopTcp *cli2 = coop_tcp_connect(&to, 2000);
    CoopTcp *srv2 = NULL;
    for (int i = 0; i < 200 && !srv2; i++) {
        srv2 = coop_tcp_accept(l, NULL);
        if (!srv2) coop_sleep_ms(10);
    }
    if (!cli2 || !srv2) FAIL_RET("no second link for the part-frame tests");
    CoopTcpSet *part = coop_tcp_set_new(NULL, 2);
    const int slot2 = coop_tcp_add(part, srv2);
    CoopHdr h5 = {.ver = COOP_PROTO, .type = COOP_T_PING, .src = 1, .dst = 2, .room = 7, .seq = 5};
    uint8_t frame[COOP_MAX_DGRAM];
    const size_t whole = coop_dgram_pack(frame, sizeof frame, &h5, msg, 40, NULL);
    CHECK(whole == COOP_HDR_LEN + 40, "40 payload bytes pack as %u", (unsigned)whole);
    CHECK(coop_tcp_send(cli2, frame, COOP_HDR_LEN - 1), "send the first part of a frame");
    const uint64_t part_t0 = coop_now_ms();
    CHECK(coop_tcp_next(part, buf, sizeof buf, &from, 300) == 0, "a frame that is not complete is not handed over");
    CHECK(coop_now_ms() - part_t0 >= 280, "the wait for the rest of it lasts its full 300 ms (%u)", (unsigned)(coop_now_ms() - part_t0));
    coop_tcp_send(cli2, frame + (COOP_HDR_LEN - 1), whole - (COOP_HDR_LEN - 1));
    n = coop_tcp_next(part, buf, sizeof buf, &from, 1000);
    CHECK(n == (int)whole && from == slot2, "the rest arrives as one frame (%d bytes from slot %d)", n, from);
    CHECK(coop_dgram_parse(buf, (size_t)n, &rh, &pp) && rh.seq == 5 && rh.len == 40, "and says what was sent (seq %u, %u bytes)", rh.seq, rh.len);
    CHECK(coop_tcp_alive(coop_tcp_at(part, slot2)), "the link that carried a split frame is still up");

    // A header that is not one ends that link, and nothing more comes from it.
    uint8_t junk[COOP_HDR_LEN];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(0x11 + i);
    CHECK(coop_tcp_send(cli2, junk, sizeof junk), "send what is not a frame header");
    CHECK(coop_tcp_next(part, buf, sizeof buf, &from, 0) == 0, "nothing is handed over for it");
    CHECK(!coop_tcp_alive(coop_tcp_at(part, slot2)), "and the link that sent it is done");
    coop_tcp_close(cli2);
    coop_tcp_set_free(part);

    // A connection knocking on the listener is reported before it is taken: the host answers a join in the same wait.
    CoopTcpListen *knock = coop_tcp_listen(0);
    if (!knock) FAIL_RET("a listen for the knocking test");
    CoopAddr knock_to;
    if (!coop_net_resolve("127.0.0.1", coop_tcp_listen_port(knock), &knock_to)) FAIL_RET("resolve the knocking listen");
    CoopTcpSet *waiter = coop_tcp_set_new(knock, 4);   // (the set holds the listen and closes it with itself)
    CoopTcp *knocker = coop_tcp_connect(&knock_to, 2000);
    CHECK(knocker != NULL, "a connection reaches a listen nobody has answered yet");
    from = 1;
    CHECK(coop_tcp_next(waiter, buf, sizeof buf, &from, 300) == -1, "the wait says a connection is knocking");
    CHECK(from == 0, "and names no slot for it (%d)", from);
    CoopAddr caller;
    CoopTcp *ringing = coop_tcp_accept(knock, &caller);
    CHECK(ringing != NULL, "the knocking connection is then there to take");
    CHECK(coop_tcp_add(waiter, ringing) == 1, "and takes a slot");
    coop_addr_str(&caller, text, sizeof text);
    CHECK(!strncmp(text, "127.0.0.1:", 10), "and says who called (%s)", text);
    coop_tcp_close(knocker);
    coop_tcp_set_free(waiter);

    // The same listen reaches a caller that comes over IPv6 loopback, and says so in that form.
    CoopAddr to6;
    if (coop_net_resolve("::1", coop_tcp_listen_port(l), &to6)) {
        CoopTcp *six = coop_tcp_connect(&to6, 2000);
        CoopTcp *srv6 = NULL;
        CoopAddr from6;
        for (int i = 0; i < 200 && !srv6; i++) {
            srv6 = coop_tcp_accept(l, &from6);
            if (!srv6) coop_sleep_ms(10);
        }
        if (six && srv6) {
            CoopHdr h6 = {.ver = COOP_PROTO, .type = COOP_T_PING, .src = 1, .dst = 2, .room = 7, .seq = 6};
            uint8_t f6[COOP_MAX_DGRAM];
            const size_t n6 = coop_dgram_pack(f6, sizeof f6, &h6, msg, 8, NULL);
            CoopTcpSet *set6 = coop_tcp_set_new(NULL, 2);
            const int slot6 = coop_tcp_add(set6, srv6);
            coop_tcp_send(six, f6, n6);
            n = coop_tcp_next(set6, buf, sizeof buf, &from, 1000);
            CHECK(n == (int)n6 && from == slot6, "IPv6 loopback carries a frame too (%d bytes, slot %d)", n, from);
            coop_addr_str(&from6, text, sizeof text);
            CHECK(!strncmp(text, "[::1]:", 6), "and the host reads it as an IPv6 address (%s)", text);
            coop_tcp_close(six);
            coop_tcp_set_free(set6);   // (with the connection it holds)
        } else {
            printf("coop-session-test: no IPv6 loopback here: that part was skipped\n");
            coop_tcp_close(six);
            coop_tcp_close(srv6);
        }
    } else {
        printf("coop-session-test: no IPv6 loopback here: that part was skipped\n");
    }

    uint8_t r1[32] = {0}, r2[32] = {0}, zero[32] = {0};
    CHECK(coop_random(r1, sizeof r1) && coop_random(r2, sizeof r2) && memcmp(r1, r2, 32) && memcmp(r1, zero, 32), "random bytes");
    uint64_t t0;
    CoopEvent e;
    coop_event_init(&e, 0, 0);
    t0 = coop_now_ms();
    CHECK(!coop_event_wait_ns(&e, 30000000) && coop_now_ms() - t0 >= 25, "an event not set times out");
    coop_event_set(&e);
    CHECK(coop_event_wait_ns(&e, 0) && !coop_event_wait_ns(&e, 0), "an auto-reset event is taken once");
    coop_event_init(&e, 1, 1);
    CHECK(coop_event_wait_ns(&e, 0) && coop_event_wait_ns(&e, 0), "a manual event stays set");
    coop_event_reset(&e);
    CHECK(!coop_event_wait_ns(&e, 0), "until reset");
    static CoopOnce once = COOP_ONCE_INIT;
    OnceCount oc = {0};
    for (int i = 0; i < 3; i++) coop_once(&once, once_fn, &oc);
    CHECK(oc.calls == 1, "a once runs once (%d)", oc.calls);
    Thread other = thread_new(once_many, NULL);
    once_many(NULL);
    thread_join(other);
    CHECK(shared_count.calls == 1, "a once two threads reach at the same time runs once (%d)", shared_count.calls);
    t0 = coop_now_ms();
    coop_sleep_ms(30);
    CHECK(coop_now_ms() - t0 >= 28, "a sleep of 30 ms (%u)", (unsigned)(coop_now_ms() - t0));
    coop_tcp_listen_close(l);
    printf("coop-session-test: the operating-system layer\n");
}

// ---- 2. the host's address

static void check_server_parse(void) {
    static const struct { const char *text, *host; unsigned port; int ok; } cases[] = {
        {"host.example", "host.example", 43400, 1}, {"host.example:5000", "host.example", 5000, 1},
        {"  host.example  ", "host.example", 43400, 1}, {"127.0.0.1:65535", "127.0.0.1", 65535, 1},
        {"[::1]", "::1", 43400, 1}, {"[::1]:99", "::1", 99, 1}, {"2001:db8::1", "2001:db8::1", 43400, 1},
        {"", "", 0, 0}, {"   ", "", 0, 0}, {"host:", "", 0, 0}, {"host:0", "", 0, 0}, {"host:65536", "", 0, 0},
        {"host:12x", "", 0, 0}, {"host:123456", "", 0, 0}, {"[::1", "", 0, 0}, {"[::1]x", "", 0, 0}, {":80", "", 0, 0},
        {"[]:80", "", 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char host[128] = "";
        uint16_t port = 0;
        int ok = coop_server_parse(cases[i].text, host, sizeof host, &port);
        CHECK(ok == cases[i].ok && (!ok || (!strcmp(host, cases[i].host) && port == cases[i].port)), "server \"%s\" parses as %d %s:%u",
              cases[i].text, ok, host, port);
    }
    char small[4];
    uint16_t port;
    CHECK(!coop_server_parse("host.example", small, sizeof small, &port), "a host that does not fit is refused");
    printf("coop-session-test: coop_server_parse\n");
}

// ---- 3. a stop's order: the stop is signalled only once the view that says the session is off is published, so whoever
// waited for it (coop_shutdown's caller, a menu that switched co-op off) cannot read the view of the step before. One
// session stepped on this thread, no host: a view hook that sees "off" notes whether the stop was signalled already.

typedef struct {
    CoopSess *sess;
    int off_seen, signalled_before;
} StopOrder;

static void on_view_stop_order(const CoopView *v, void *user) {
    StopOrder *o = user;
    if (v->state != COOP_OFF || o->off_seen) return;
    o->off_seen = 1;
    o->signalled_before = coop_sess_wait_off(o->sess, 0);
}

static void check_stop_order(void) {
    CoopParams cp = {.tag = "Stop", .server = "", .name = "Stop", .client_id = 0x5709, .on = 1};
    StopOrder o = {0};
    o.sess = coop_sess_new(&cp);
    if (!o.sess) FAIL_RET("a session did not start");
    coop_sess_on_view(o.sess, on_view_stop_order, &o);
    CoopView v;
    coop_sess_view(o.sess, &v);
    CHECK(v.state == COOP_LEFT && !coop_sess_wait_off(o.sess, 0), "stop order: a session that is on is in no room and not stopped (state %d)", v.state);
    coop_sess_stop(o.sess);
    coop_sess_step(o.sess);
    coop_sess_view(o.sess, &v);
    CHECK(o.off_seen && !o.signalled_before,
          "stop order: the stop is signalled only after the view that says the session is off is published (a view hook saw off %d, the stop already signalled then %d)",
          o.off_seen, o.signalled_before);
    CHECK(coop_sess_wait_off(o.sess, 0) && v.state == COOP_OFF, "stop order: after the step the stop is signalled and the view says off (state %d)", v.state);
    coop_sess_free(o.sess);
    printf("coop-session-test: a stop wakes its waiter only once the view says the session is off\n");
}

// ---- 4. the process's session off, then on by the options file

static void check_off_then_on(void) {
    set_env("HOS_COOP", NULL), set_env("HOS_COOP_AUTO", NULL), set_env("HOS_COOP_SERVER", NULL);
    set_env("HOS_COOP_NAME", NULL), set_env("HOS_COOP_PORT", NULL);
    CoopView v;
    coop_view_get(&v);   // (makes the session's memory, nothing more)
    const int threads = tasks(), fds = files();
    coop_init();
    set_env("HOS_COOP", "0");
    ini_set("coop", "1");
    coop_init();         // the environment over the options file
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "co-op is off unless switched on (state %d)", v.state);
    CHECK(tasks() == threads && files() == fds,
          "off, coop_init starts no thread and opens no socket (threads %d -> %d, files %d -> %d)", threads, tasks(), fds, files());
    CHECK(!coop_command(COOP_CMD_HOST, NULL) && !coop_send(COOP_K_TEST, "x", 1), "off, no command is taken and nothing sent");
    CHECK(!log_count("coop:"), "off, nothing is logged");
    coop_shutdown();   // (nothing to end)

    // the options file alone switches it on: coop=1, the name from coop_name=, the port from coop_port=, no host given
    const uint16_t own = free_port();
    char port_text[8];
    snprintf(port_text, sizeof port_text, "%u", (unsigned)own);
    ini_set("coop_port", port_text);
    set_env("HOS_COOP", NULL);
    ini_set("coop_name", "From the file");
    coop_init();
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT), 2000), "coop=1 in the options file switches co-op on (state %d)", v.state);
    CHECK(!strcmp(v.name, "From the file"), "with the options file's coop_name (%s)", v.name);
    char line[96];
    snprintf(line, sizeof line, "coop: on, as From the file; hosting on port %u until a host is given", (unsigned)own);
    CHECK(log_count(line) == 1, "the log says co-op is on, as whom, and on the port coop_port= gave: %s", line);
    const int hosted = log_count("coop: hosting room ");
    const uint64_t host_t0 = coop_now_ms();
    CHECK(coop_command(COOP_CMD_HOST, NULL), "HOST is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED && v.made && v.self == 1 && v.maker == 1 && v.n == 1), 4000),
          "and this game is the room on the port it was given (state %d, self %d, maker %d, %d members, %s)", v.state, v.self, v.maker, v.n, v.status);
    const unsigned host_took = (unsigned)(coop_now_ms() - host_t0);
    CHECK(log_count("coop: hosting room ") == hosted + 1, "the log says which room and on which port");
    CHECK(files() == fds + 1, "one socket more, the port it listens on (files %d -> %d)", fds, files());
    char canon[COOP_SECRET_LEN + 1];
    CHECK(strlen(v.code) == COOP_SECRET_TEXT && v.code[6] == '-' && v.code[13] == '-' && coop_secret_normalize(v.code, canon),
          "the view shows the room's secret as XXXXXX-XXXXXX-XXXXXX (%s)", v.code);
    CHECK(strcmp(canon, ROOM_SECRET) != 0, "and HOST without a secret makes one of its own, not the one in tests/coop_vectors.txt");
    const int lan = log_count("coop: players on this network can join ");
    CHECK(coop_command(COOP_CMD_JOIN, ""), "JOIN with no host address is queued");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT && strstr(v.status, "no host address")), 2000),
          "and says there is no host to join: %s", v.status);
    CHECK(log_count("coop: no host address given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)") == 1, "in the log too");
    CHECK(files() == fds, "and the room it hosted is closed with it (files %d -> %d)", fds, files());
    // the game closes: no room to say goodbye to, and the thread ends
    const uint64_t t0 = coop_now_ms();
    coop_shutdown();
    const unsigned took = (unsigned)(coop_now_ms() - t0);
    coop_view_get(&v);
    CHECK(took < 1500 && v.state == COOP_OFF, "coop_shutdown returns within a second and a half and leaves the session off (%u ms, state %d)", took, v.state);
    CHECK(WAIT_FOR(tasks() == threads, 5000) && files() == fds,
          "the game closing ends the co-op thread: no thread and no file more than before co-op was on (threads %d -> %d, files %d -> %d)", threads,
          tasks(), fds, files());
    CHECK(!coop_command(COOP_CMD_HOST, NULL), "and no command is taken");
    coop_shutdown();   // (twice is the same as once)
    ini_set("coop_port", NULL);
    printf("coop-session-test: the process's session: off without HOS_COOP or coop=1 (no thread, no socket), on by the options file, HOST on its port "
           "in %u ms; %d line%s naming an address a player on this network could type; the game closing took %u ms\n",
           host_took, lan, lan == 1 ? "" : "s", took);
}

// A view hook that holds the coop thread up when it sees the session go off, as a name lookup that does not return would.
static int hold_off_ms;
static void on_view_hold(const CoopView *v, void *user) {
    (void)user;
    const int ms = __atomic_load_n(&hold_off_ms, __ATOMIC_ACQUIRE);
    if (v->state == COOP_OFF && ms) coop_sleep_ms((uint32_t)ms);
}

// The menu switches co-op on while the game runs (it was off at start): coop_configure starts the thread, a command
// right after it is queued, and off ends the thread again. Then off, on, off with a player's port that never answers, so
// that a socket is open each time; and stops the thread is too slow for.
static void check_menu_on(void) {
    set_env("HOS_COOP", NULL), set_env("HOS_COOP_AUTO", NULL), set_env("HOS_COOP_SERVER", NULL);
    set_env("HOS_COOP_NAME", NULL);
    // a port of its own: two runs of this scenario (the plain one and the ThreadSanitizer one, which start together)
    // would otherwise reach for the same default port and one of them could not host
    char own[8];
    snprintf(own, sizeof own, "%u", (unsigned)free_port());
    set_env("HOS_COOP_PORT", own);
    coop_init();   // (the settings were read once already; co-op is off, nothing to start)
    CoopView v;
    coop_view_get(&v);
    const int threads = tasks();
    int fds = files();
    CHECK(v.state == COOP_OFF && !coop_command(COOP_CMD_HOST, NULL), "off, no command is taken (state %d)", v.state);
    CHECK(coop_configure(0, "Menu", "") && tasks() == threads, "the menu switching it off starts nothing");
    CHECK(coop_configure(1, "  Menu\x01name ", ""), "the menu switches co-op on");
    CHECK(coop_command(COOP_CMD_HOST, NULL), "a HOST right after it is queued, before the thread has applied it");
    const int hosted = log_count("coop: hosting room ");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED && v.made && v.self == 1), 4000),
          "the thread started, applied the menu's settings, then the HOST, which makes this game the room (state %d, %s)", v.state, v.status);
    CHECK(!strcmp(v.name, "  Menu?name "), "the view's name is the cleaned name in use (\"%s\")", v.name);
    CHECK(tasks() <= 0 || tasks() == threads + 1, "one thread more");
    CHECK(files() <= 0 || files() == fds + 1, "and one socket more, the port the room is on (files %d -> %d)", fds, files());
    CHECK(log_count("coop: hosting room ") == hosted + 1, "the log says the room is hosted");
    CHECK(coop_configure(1, "", "host.invalid") && WAIT_FOR((coop_view_get(&v), !strncmp(v.name, "Player-", 7)), 3000),
          "an empty name is the default Player-xxxx (%s)", v.name);
    CHECK(coop_configure(0, "", "") && WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF), 3000), "the menu switches it off again (state %d)", v.state);
    CHECK(!coop_command(COOP_CMD_HOST, NULL), "and no command is taken");
    CHECK(WAIT_FOR(tasks() == threads, 5000), "and the co-op thread has ended (threads %d, before co-op was on %d)", tasks(), threads);
    printf("coop-session-test: the menu switches co-op on and off while the game runs\n");

    // off, on, off: each on starts a thread, and its JOIN opens a link to a port that never answers (a listen of this
    // test on loopback that is never accepted); each off leaves neither
    CoopTcpListen *silent = coop_tcp_listen(0);
    if (!silent) FAIL_RET("a port for the host that never answers");
    fds = files();   // from here the counts include this test's own listening socket
    char server[32];
    snprintf(server, sizeof server, "127.0.0.1:%u", (unsigned)coop_tcp_listen_port(silent));
    const int more = threads > 0;   // (more: 1 where threads and files can be counted)
    unsigned slowest = 0;
    for (int i = 1; i <= 3; i++) {
        CHECK(coop_configure(1, "Cycle", server) && coop_command(COOP_CMD_JOIN, ""), "cycle %d: on, and a JOIN is taken", i);
        CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_CONNECTING) && files() == fds + more, 5000),
              "cycle %d: the session reaches for the host, with one socket more (state %d, %s; files %d -> %d)", i, v.state, v.status, fds, files());
        CHECK(tasks() == threads + more, "cycle %d: and one thread more (%d -> %d)", i, threads, tasks());
        const uint64_t t0 = coop_now_ms();
        CHECK(coop_configure(0, "Cycle", server), "cycle %d: off", i);
        const unsigned took = (unsigned)(coop_now_ms() - t0);
        if (took > slowest) slowest = took;
        CHECK(took < 1500, "cycle %d: switching co-op off returns within a second and a half (%u ms)", i, took);
        CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && tasks() == threads && files() == fds, 5000),
              "cycle %d: off leaves no thread and no socket (state %d; threads %d -> %d, files %d -> %d)", i, v.state, threads, tasks(), fds, files());
        CHECK(!coop_command(COOP_CMD_JOIN, ""), "cycle %d: and no command is taken", i);
    }
    printf("coop-session-test: off, on, off three times: one thread and one socket while on, none left when off (off took at most %u ms)\n", slowest);

    // a stop the thread does not carry out in time: coop_configure waits 0.4 s for it and returns; the thread ends by
    // itself when it gets there, and the next on starts another
    CHECK(coop_on_view(on_view_hold, NULL), "a view hook registers");
    CHECK(coop_configure(1, "Held", server) && WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT) && tasks() == threads + more, 3000),
          "on again (state %d, threads %d -> %d)", v.state, threads, tasks());
    __atomic_store_n(&hold_off_ms, 2500, __ATOMIC_RELEASE);
    uint64_t t0 = coop_now_ms();
    CHECK(coop_configure(0, "Held", server), "off, with the thread held up for 2.5 s");
    unsigned took = (unsigned)(coop_now_ms() - t0);
    __atomic_store_n(&hold_off_ms, 0, __ATOMIC_RELEASE);
    CHECK(took >= 350 && took < 2000, "a stop that is held up is waited for 0.4 s, not until it is done (%u ms)", took);
    CHECK(tasks() == threads + more, "the held-up thread is still there (threads %d -> %d)", threads, tasks());
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && tasks() == threads, 8000), "and ends by itself (state %d, threads %d -> %d)", v.state,
          threads, tasks());
    CHECK(coop_configure(1, "Held", server) && coop_command(COOP_CMD_JOIN, "") &&
              WAIT_FOR((coop_view_get(&v), v.state == COOP_CONNECTING) && tasks() == threads + more && files() == fds + more, 5000),
          "the next on starts another thread (state %d, threads %d -> %d, files %d -> %d)", v.state, threads, tasks(), fds, files());
    // held up again, and switched on again before the thread got to its end: the stop is taken back, the same thread goes on
    __atomic_store_n(&hold_off_ms, 2500, __ATOMIC_RELEASE);
    CHECK(coop_configure(0, "Held", server), "off again, held up again");
    __atomic_store_n(&hold_off_ms, 0, __ATOMIC_RELEASE);
    CHECK(coop_configure(1, "Back", server) && coop_command(COOP_CMD_JOIN, ""), "on again before the thread has ended");
    CHECK(tasks() == threads + more, "no second thread (threads %d -> %d)", threads, tasks());
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_CONNECTING && !strcmp(v.name, "Back")) && files() == fds + more, 8000),
          "the same thread goes on: on under the new name, the JOIN carried out (state %d, %s)", v.state, v.name);
    CHECK(tasks() == threads + more, "still one thread (threads %d -> %d)", threads, tasks());
    t0 = coop_now_ms();
    coop_shutdown();   // the game closes while the session reaches for the host's port
    took = (unsigned)(coop_now_ms() - t0);
    CHECK(took < 1500 && WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && tasks() == threads && files() == fds, 5000),
          "the game closing ends it: no thread and no socket left (%u ms, state %d; threads %d -> %d, files %d -> %d)", took, v.state, threads, tasks(),
          fds, files());
    coop_tcp_listen_close(silent);
    printf("coop-session-test: a stop that is held up is waited for 0.4 s; the thread then ends by itself, or goes on when co-op is switched on again\n");
}

// ---- 5. sessions of this process: a room on a port of its own, players that reach it directly

typedef struct {
    const char *name;
    CoopSess *sess;
    Thread thread;
    volatile int stop;
    volatile unsigned views, saw_reconnect;   // saw_reconnect: times the session went into RECONNECTING (rejoins)
    int last_state;                           // the state of the last view (the view hook's thread only)
    volatile unsigned got, dups, byes, from_wrong;   // test messages received, delivered twice, BYEs, from a member other than the expected
    uint32_t seen[512];
    int expect_from;
} Player;

static void player_loop(void *arg) {
    Player *p = arg;
    while (!p->stop) coop_sess_step(p->sess);
}

static void on_test_message(int from, const uint8_t *bytes, size_t n, void *user) {
    Player *p = user;
    uint32_t seq;
    if (n != 9 || memcmp(bytes, "probe", 4) || !coop_u32_parse(bytes + 5, 4, &seq) || seq >= 512 * 32) return;
    if (p->expect_from && from != p->expect_from) __atomic_add_fetch(&p->from_wrong, 1, __ATOMIC_RELAXED);
    uint32_t bit = 1u << (seq % 32);
    if (p->seen[seq / 32] & bit) __atomic_add_fetch(&p->dups, 1, __ATOMIC_RELAXED);
    p->seen[seq / 32] |= bit;
    __atomic_add_fetch(&p->got, 1, __ATOMIC_RELAXED);
}

static void on_bye(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)from, (void)bytes, (void)n;
    __atomic_add_fetch(&((Player *)user)->byes, 1, __ATOMIC_RELAXED);
}

static void on_view_change(const CoopView *v, void *user) {
    Player *p = user;
    __atomic_add_fetch(&p->views, 1, __ATOMIC_RELAXED);
    // a rejoin publishes a view at each change while it lasts (its status text, the roster): count the step into
    // RECONNECTING, not every view in it
    if (v->state == COOP_RECONNECTING && p->last_state != COOP_RECONNECTING) __atomic_add_fetch(&p->saw_reconnect, 1, __ATOMIC_RELAXED);
    p->last_state = v->state;
}

static void player_start(Player *p, const char *name, uint64_t cid, const char *server, unsigned drop, int retry, uint16_t port) {
    CoopParams cp = {.tag = name, .server = server, .name = name, .client_id = cid, .on = 1, .drop_pct = drop, .retry = retry,
                     .port = port, .ping_ms = 200, .silence_ms = 1500, .burst_gap_ms = 100, .backoff_first_ms = 150,
                     .backoff_max_ms = 500};
    memset(p, 0, sizeof *p);
    p->name = name;
    p->sess = coop_sess_new(&cp);
    if (!p->sess) return;
    coop_sess_on_message(p->sess, COOP_K_TEST, on_test_message, p);
    coop_sess_on_message(p->sess, COOP_K_BYE, on_bye, p);
    coop_sess_on_view(p->sess, on_view_change, p);
    p->thread = thread_new(player_loop, p);
}

static void player_end(Player *p) {
    if (!p->sess) return;
    coop_sess_stop(p->sess);
    coop_sess_wait_off(p->sess, 1000);
    p->stop = 1;
    thread_join(p->thread);
    coop_sess_free(p->sess);
    p->sess = NULL;
}

static CoopView view_of(Player *p) {
    CoopView v;
    coop_sess_view(p->sess, &v);
    return v;
}

static int in_room(Player *p, int members) {
    CoopView v = view_of(p);
    return v.state == COOP_JOINED && v.n == members;
}

// p's roster names q's present session, both in the room: p takes q's messages. With q = p: the last roster p took
// lists its own session, so no rejoin of p waits for its turn (roster_rejoin in coop.c).
static int names_session(Player *p, Player *q) {
    CoopView vp = view_of(p), vq = view_of(q);
    if (vp.state != COOP_JOINED || vq.state != COOP_JOINED) return 0;
    for (int i = 0; i < vp.n; i++)
        if (vp.member[i].id == vq.self) return vp.member[i].session == vq.session;
    return 0;
}

// The host lists each player's present session and both rosters name both, so neither throws the other's messages away
// nor joins again until something else changes the room.
static int settled(Player *a, Player *b) {
    return names_session(a, a) && names_session(a, b) && names_session(b, a) && names_session(b, b);
}

// n numbered messages from a to b, 15 ms apart, then what b got of them. to: the member id they are addressed to, or 0
// for all.
static unsigned send_messages(Player *a, Player *b, int to, int n, uint32_t first) {
    unsigned before = b->got;
    for (int i = 0; i < n; i++) {
        uint8_t m[9];
        memcpy(m, "probe", 5);
        coop_u32_pack(m + 5, 4, first + (uint32_t)i);
        m[4] = 0;
        coop_sess_send(a->sess, to, COOP_K_TEST, m, sizeof m);
        coop_sleep_ms(15);
    }
    coop_sleep_ms(300);
    return b->got - before;
}

static void exchange(Player *a, Player *b, unsigned drop, const char *what, uint32_t *first) {
    int ida = view_of(a).self, idb = view_of(b).self;
    a->expect_from = idb, b->expect_from = ida;
    unsigned min = drop ? 20 : 55;
    unsigned ab = send_messages(a, b, 0, 60, *first);
    *first += 60;
    unsigned ba = send_messages(b, a, ida, 60, *first);   // addressed to one member
    *first += 60;
    CHECK(ab >= min, "%s: Alice's messages reached Bob: %u of 60 (at least %u)", what, ab, min);
    CHECK(ba >= min, "%s: Bob's messages reached Alice: %u of 60 (at least %u)", what, ba, min);
    CHECK(!a->dups && !b->dups, "%s: no message was delivered twice (%u, %u)", what, a->dups, b->dups);
    CHECK(!a->from_wrong && !b->from_wrong, "%s: every message came from the right member", what);
    printf("coop-session-test: %s: Alice -> Bob %u of 60, Bob -> Alice %u of 60\n", what, ab, ba);
}

// ---- 5b. a player made of wire bytes only: what a hostile host meets is the same CONNECT a game sends
//
// It is deliberately a second implementation of the join, not a hook into coop.c: the answers it takes are the ones a
// game takes (a CHALLENGE's cookie, the ROOM's secret, which replaces whatever secret it started with), so the hostile
// scenarios below say what a player of another build, or one that reads the room's port, really gets.

typedef struct {
    CoopTcpSet *set;
    int slot;
    uint64_t session, nonce;
    char secret[COOP_SECRET_LEN + 1];        // the host's, once its ROOM was taken
    uint8_t locator[COOP_KEY_LEN], msg_key[COOP_MSG_KEY_LEN];
    uint8_t cookie[COOP_COOKIE_LEN], token[COOP_COOKIE_LEN];
    int member;                              // the id it was given, 0 when it got none
    int reason;                              // the ERROR it was answered with, 0 when none
    CoopRoster roster;
} CoopRaw;

static void raw_close(CoopRaw *r) {
    coop_tcp_set_free(r->set);
    memset(r, 0, sizeof *r);
}

static uint64_t raw_serial;

static int raw_join(CoopRaw *r, uint16_t port, const char *name, uint64_t cid, const char *secret,
                    const uint8_t token_in[COOP_COOKIE_LEN]) {
    memset(r, 0, sizeof *r);
    if (!coop_secret_normalize(secret, r->secret)) return 0;
    coop_room_keys(r->secret, r->locator, r->msg_key);
    CoopAddr to;
    if (!coop_net_resolve("127.0.0.1", port, &to)) return 0;
    CoopTcp *t = coop_tcp_connect(&to, 2000);
    if (!t) return 0;
    r->set = coop_tcp_set_new(NULL, 2);
    if (!r->set) { coop_tcp_close(t); return 0; }
    r->slot = coop_tcp_add(r->set, t);
    if (!r->slot) { raw_close(r); return 0; }
    raw_serial++;
    r->session = 0x5100000000000000ull + raw_serial * 0x1000u;
    r->nonce = 0x6200000000000000ull + raw_serial * 0x1000u;
    CoopConnect c = {.intent = COOP_INTENT_JOIN, .client_id = cid, .session = r->session, .nonce = r->nonce};
    memcpy(c.locator, r->locator, COOP_KEY_LEN);
    snprintf(c.name, sizeof c.name, "%s", name);
    if (token_in) memcpy(c.token, token_in, COOP_COOKIE_LEN);
    uint8_t buf[COOP_MAX_DGRAM], payload[COOP_MAX_PAYLOAD];
    int sent = 0, keyed = 0, got_cookie = 0, got_room = 0;
    uint64_t last_send = 0;
    for (int i = 0; i < 100 && !r->member && !r->reason; i++) {
        // The CONNECT, and this player's turn to send it again: a host answers an unproven CONNECT rather than
        // refusing it, so one that is said once and waited for is a player that never gets in.
        if (!sent || coop_now_ms() - last_send >= RAW_CONNECT_AGAIN_MS) {
            if (got_cookie && got_room) memcpy(c.cookie, r->cookie, COOP_COOKIE_LEN);   // the CONNECT that proves the keys
            size_t k = coop_connect_pack(payload, sizeof payload, &c);
            CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_CONNECT, .seq = (uint32_t)(keyed ? 2 : 1)};
            size_t f = coop_dgram_pack(buf, sizeof buf, &h, payload, k, r->locator);
            if (!f || !coop_tcp_send(t, buf, f)) break;
            keyed = keyed || (got_cookie && got_room);
            last_send = coop_now_ms();
            sent = 1;
            continue;
        }
        int n = coop_tcp_next(r->set, buf, sizeof buf, NULL, 100);
        if (n <= 0) {
            if (!coop_tcp_alive(t)) break;   // the host said its last word on the link and closed it
            continue;
        }
        CoopHdr h;
        const uint8_t *p;
        if (!coop_dgram_parse(buf, (size_t)n, &h, &p)) continue;
        if (h.type == COOP_T_CHALLENGE) {
            uint64_t kn;
            if (coop_challenge_parse(p, h.len, &kn, r->cookie) && kn == r->nonce) got_cookie = r->cookie[0] != 0;
        } else if (h.type == COOP_T_ROOM) {
            uint64_t rn;
            char given[COOP_SECRET_LEN + 1];
            if (coop_room_parse(p, h.len, &rn, given) && rn == r->nonce) {
                memcpy(r->secret, given, sizeof r->secret);
                coop_room_keys(r->secret, r->locator, r->msg_key);
                memcpy(c.locator, r->locator, COOP_KEY_LEN);
                got_room = 1;
            }
        } else if (h.type == COOP_T_JOINED) {
            CoopJoined j;
            if (coop_joined_parse(p, h.len, &j) && j.nonce == r->nonce) {
                r->member = j.member;
                memcpy(r->token, j.token, COOP_COOKIE_LEN);
                r->roster = j.roster;
            }
        } else if (h.type == COOP_T_ERROR) {
            CoopError e;
            if (coop_error_parse(p, h.len, &e)) r->reason = e.reason;
        }
    }
    if (!r->member) {   // a join that got nowhere leaves no link behind; the reason it was told stays for the test
        coop_tcp_set_free(r->set);
        r->set = NULL;
    }
    return r->member != 0;
}

// A player that is in the room with a name of its own, for a host that has to answer it: 1 while its link is open.
static int raw_present(const CoopRaw *r) {
    return r->member && coop_tcp_alive(coop_tcp_at(r->set, r->slot));
}

// ---- 5c. the member id a player gets back, and a link that dies while the host is answering it

static void server_of(uint16_t port, char *out, size_t cap) {
    snprintf(out, cap, "127.0.0.1:%u", port);
}

// One CONNECT as the wire carries it, keyed with key (NULL: no room yet).
static size_t connect_frame(uint8_t *out, size_t cap, const CoopConnect *c, uint32_t seq, const uint8_t key[COOP_KEY_LEN]) {
    uint8_t payload[COOP_MAX_PAYLOAD];
    size_t k = coop_connect_pack(payload, sizeof payload, c);
    if (!k) return 0;
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_CONNECT, .seq = seq};
    return coop_dgram_pack(out, cap, &h, payload, k, key);
}

// The JOINED's token is how a player that drops for a moment comes back as the member it was instead of a new one; it is
// kept for a minute and spent on the way in (coop.c: gone_of, take_member).
static void check_rejoin_token(Player *alice, Player *bob, uint16_t port, uint32_t *first, unsigned drop) {
    CoopView av = view_of(alice);
    if (!(av.self == 1 && av.made && in_room(alice, 2) && in_room(bob, 2)))
        FAIL_RET("the room is Alice (member 1) and Bob (member 2) before the token test");
    const unsigned byes = bob->byes;

    CoopRaw rex;
    if (!raw_join(&rex, port, "Rex", 0x44u, ROOM_SECRET, NULL))
        FAIL_RET("a player of its own bytes gets into the room (reason %d)", rex.reason);
    CHECK(rex.member == 3, "the third player is member 3, not %d", rex.member);
    if (!WAIT_FOR(in_room(alice, 3) && in_room(bob, 3), ROSTER_WAIT_MS))
        FAIL_RET("everyone sees Rex in the room (%s)", view_of(alice).status);
    CHECK(log_count("coop[Alice]: Rex (member 3) joined the room") == 1, "the log says who joined");
    CHECK(log_count("coop[Bob]: Rex (member 3) joined the room") == 1, "the other player hears who joined too");

    coop_tcp_drop(rex.set, rex.slot);   // it goes without a word: no BYE, no LEAVE
    if (!WAIT_FOR(in_room(alice, 2), LINK_GONE_MS))
        FAIL_RET("the host takes a link that closed without a word out of its room at once (%s)", view_of(alice).status);
    if (!WAIT_FOR(in_room(bob, 2), drop ? LINK_GONE_AGAIN_MS : LINK_GONE_MS))
        FAIL_RET("the other player's roster says the same (%s)", view_of(bob).status);
    CHECK(log_count("coop[Alice]: Rex (member 3) left the room") == 1, "the room says who left");
    CHECK(bob->byes == byes, "a member whose link closed sent no BYE");

    uint8_t token[COOP_COOKIE_LEN];
    memcpy(token, rex.token, sizeof token);
    raw_close(&rex);

    CoopRaw back;
    if (!raw_join(&back, port, "Rex", 0x44u, ROOM_SECRET, token))
        FAIL_RET("the token of a member that just left gets it back in (reason %d)", back.reason);
    CHECK(back.member == 3, "the rejoin keeps member 3, not %d", back.member);
    if (!WAIT_FOR(in_room(alice, 3) && in_room(bob, 3), ROSTER_WAIT_MS))
        FAIL_RET("everyone sees Rex back in the room (%s)", view_of(alice).status);
    CHECK(log_count("coop[Alice]: Rex (member 3) joined the room") == 2, "and the log says it came back");
    CHECK(!in_room(alice, 4), "coming back is one player, not two");

    // What lets a link into a room is the room's keys, not the token: a spent token proves nothing, so the host treats
    // the link as a player of its own and gives it the next free id (coop.c: gone_of finds nobody, take_member picks an id).
    CoopRaw twice;
    if (!raw_join(&twice, port, "Rex", 0x44u, ROOM_SECRET, token))
        FAIL_RET("a link with the room's keys gets in with a spent token too (reason %d)", twice.reason);
    CHECK(twice.member == 4, "a spent token makes a new member, not the id it once held (%d)", twice.member);
    raw_close(&twice);
    if (!WAIT_FOR(in_room(alice, 3), LINK_GONE_MS))
        FAIL_RET("the new member is out of the host's room when its link closes (%s)", view_of(alice).status);
    if (!WAIT_FOR(in_room(bob, 3), drop ? LINK_GONE_AGAIN_MS : LINK_GONE_MS))
        FAIL_RET("and out of the other player's roster (%s)", view_of(bob).status);

    exchange(alice, bob, drop, "a member id that came back", first);

    raw_close(&back);
    if (!WAIT_FOR(in_room(alice, 2), LINK_GONE_MS))
        FAIL_RET("Rex is out of the host's room again (%s)", view_of(alice).status);
    if (!WAIT_FOR(in_room(bob, 2), drop ? LINK_GONE_AGAIN_MS : LINK_GONE_MS))
        FAIL_RET("and out of the other player's roster (%s)", view_of(bob).status);
}

// What the host does when it writes its answer to a link the other end has already closed. On a POSIX socket such a
// write fails with EPIPE and the process gets SIGPIPE, which is what took a whole game down before coop_net.c writes
// with MSG_NOSIGNAL (tests/test_coop_session.py checks the flag is there). Nothing here asserts that the host notices:
// the guard is that this process is still here at the end of it, with its room intact.
static void check_dead_link(Player *alice, Player *bob, uint16_t port, uint32_t *first, unsigned drop) {
    uint8_t locator[COOP_KEY_LEN], msg_key[COOP_MSG_KEY_LEN];
    coop_room_keys(ROOM_SECRET, locator, msg_key);
    (void)msg_key;
    if (!WAIT_FOR(in_room(alice, 2) && in_room(bob, 2), drop ? LINK_GONE_AGAIN_MS : LINK_GONE_MS))
        FAIL_RET("the room is Alice and Bob before the link that dies (%d, %d)", view_of(alice).n, view_of(bob).n);
    CoopAddr to;
    if (!coop_net_resolve("127.0.0.1", port, &to)) FAIL_RET("resolve the host address");
    CoopTcp *t = coop_tcp_connect(&to, 2000);
    if (!t) FAIL_RET("open a link to the host's port");
    CoopTcpSet *set = coop_tcp_set_new(NULL, 2);
    if (!set) { coop_tcp_close(t); FAIL_RET("make a link set"); }
    const int slot = coop_tcp_add(set, t);
    if (!slot) { coop_tcp_set_free(set); FAIL_RET("add the link to the set"); }
    raw_serial++;
    CoopConnect c = {.intent = COOP_INTENT_JOIN, .client_id = 0x55u,
                     .session = 0x5100000000000000ull + raw_serial * 0x1000u,
                     .nonce = 0x6200000000000000ull + raw_serial * 0x1000u};
    memcpy(c.locator, locator, COOP_KEY_LEN);
    snprintf(c.name, sizeof c.name, "Ghost");
    uint8_t buf[COOP_MAX_DGRAM];
    for (int i = 0; i < 3; i++) {   // a burst of the same CONNECT, and never the cookie back
        size_t f = connect_frame(buf, sizeof buf, &c, (uint32_t)(1 + i), locator);
        if (!f || !coop_tcp_send(t, buf, f)) break;
    }
    const int refused = log_count("coop[Alice]: refused a player");
    coop_tcp_drop(set, slot);   // gone before the six answers the host owes it are written
    coop_sleep_ms(400);
    coop_tcp_set_free(set);
    CHECK(in_room(alice, 2) && in_room(bob, 2), "a host that writes to a closed link keeps its room (%s)", view_of(alice).status);
    CHECK(log_count("coop[Alice]: refused a player") == refused, "a handshaking link that died is not a player the host refused");
    exchange(alice, bob, drop, "a link that died under the host's answers", first);
}

// ---- 5d. a host that lies: what a session makes of what the other end of its link sends it
//
// The victim is an ordinary session of this fixture. Its host is made of wire bytes only, so every answer it sends is
// one any program that reaches the port could send — including one that holds the room's real keys, because it is the
// one that made the secret the victim is told on the link. That is what these scenarios measure: reaching the port is
// enough to become the room's host and a member of it (docs/coop.md), so what matters is what a session takes from
// that member and what it throws away.

enum { EV_OK, EV_GARBAGE, EV_SILENT, EV_PROTO, EV_ERROR, EV_DATA_BAD_KEY, EV_DATA_OLD_SESSION,
       EV_DATA_REPLAY, EV_MAKER_ROSTER, EV_FOREIGN_ROSTER };

typedef struct {
    int challenged;                  // this link's first CONNECT was answered
    uint64_t session;                // the last one it carried in a CONNECT
    uint8_t cookie[COOP_COOKIE_LEN];
} EvLink;

typedef struct {
    CoopTcpListen *listen;
    CoopTcpSet *set;                 // it owns the listener, so one free closes both
    int mode, error;                 // EV_* and, for EV_ERROR, the reason to answer with
    uint32_t step;                   // mischief frames of this mode sent so far
    uint64_t at;                     // when the last one went
    char secret[COOP_SECRET_LEN + 1];
    uint8_t locator[COOP_KEY_LEN], msg_key[COOP_MSG_KEY_LEN], foreign[COOP_MSG_KEY_LEN];
    uint64_t session;                // its own: member 1 of the roster it publishes
    uint32_t seq;                    // its CHALLENGE / ROOM / ERROR / PONG / DATA
    uint32_t rseq;                   // its JOINED / ROSTER only: the number a player compares rosters by
    uint32_t room;                   // non-zero, as the header of every frame of its room carries it
    char name[COOP_NAME_MAX + 1];    // the joiner's, as its CONNECT names it
    int joined_slot;                 // 0 while nobody has been let in
    uint64_t joined_session;
    int junked;                          // EV_GARBAGE: its one link's worth of bytes that are not a frame
    volatile uint64_t first_session; // EV_ERROR with COOP_E_SESSION_USED: the attempt it refused
    EvLink link[8];
} Evil;

static void evil_free(Evil *e) {
    if (e->set) coop_tcp_set_free(e->set);
    else if (e->listen) coop_tcp_listen_close(e->listen);
    memset(e, 0, sizeof *e);
}

static int evil_new(Evil *e, uint16_t port, int mode, int error) {
    memset(e, 0, sizeof *e);
    e->mode = mode;
    e->error = error;
    memcpy(e->secret, ROOM_SECRET, sizeof e->secret);   // the room the rest of this fixture plays in
    coop_room_keys(e->secret, e->locator, e->msg_key);
    uint8_t unused[COOP_KEY_LEN];
    coop_room_keys("234567234567234567", unused, e->foreign);   // another room's message key
    e->session = 0x3100000000000000ull;
    e->room = 0x51A3u;
    e->seq = 1;
    e->rseq = 1;
    e->listen = coop_tcp_listen(port);
    if (!e->listen) return 0;
    e->set = coop_tcp_set_new(e->listen, 8);
    if (!e->set) { coop_tcp_listen_close(e->listen); e->listen = NULL; return 0; }
    return 1;
}

// A host's cookie and its rejoin token are both known by a byte that is not zero, so a draw of that shape is drawn
// again: a zero one would never be taken for the proof it hands out.
static int evil_proof(uint8_t out[COOP_COOKIE_LEN]) {
    for (int again = 0; again < 16; again++)
        if (coop_random(out, COOP_COOKIE_LEN) && out[0]) return 1;
    return 0;
}

static void evil_send(Evil *e, int slot, const CoopHdr *h, const void *payload, size_t n, const uint8_t *key) {
    CoopTcp *t = coop_tcp_at(e->set, slot);
    if (!t || !n) return;
    uint8_t buf[COOP_MAX_DGRAM];
    const size_t f = coop_dgram_pack(buf, sizeof buf, h, payload, n, key);
    if (f) coop_tcp_send(t, buf, f);
}

// The two-line roster of its room: itself as member 1, the joiner as member 2 — or the joiner as the maker, which is
// what a roster can do to a player that is not hosting anything.
static void evil_roster(const Evil *e, uint64_t victim_session, int victim_maker, CoopRoster *r) {
    memset(r, 0, sizeof *r);
    r->n = 2;
    r->m[0].id = 1;
    r->m[0].flags = (uint8_t)(victim_maker ? 0 : COOP_FLAG_MAKER);
    r->m[0].session = e->session;
    snprintf(r->m[0].name, sizeof r->m[0].name, "Evil");
    r->m[1].id = 2;
    r->m[1].flags = (uint8_t)(victim_maker ? COOP_FLAG_MAKER : 0);
    r->m[1].session = victim_session;
    snprintf(r->m[1].name, sizeof r->m[1].name, "%s", e->name[0] ? e->name : "Vic");
}

static void evil_publish(Evil *e, int slot, const CoopRoster *r, uint32_t ahead) {
    uint8_t pl[COOP_MAX_PAYLOAD];
    const size_t n = coop_roster_pack(pl, sizeof pl, r);
    if (!n) return;
    const uint32_t seq = e->rseq + ahead;
    e->rseq = seq;
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_ROSTER, .src = 1, .dst = COOP_DST_ALL, .room = e->room, .seq = seq};
    evil_send(e, slot, &h, pl, n, e->locator);
}

// A player message of member 1 (the host itself), sealed with key under envelope session and counter. The body is the
// shape on_test_message takes: "prob", a zero, and the number it counts by.
static void evil_data(Evil *e, int slot, const uint8_t key[COOP_MSG_KEY_LEN], uint64_t session, uint32_t counter) {
    uint8_t body[9], pl[COOP_MAX_PAYLOAD];
    memcpy(body, "prob", 4);
    body[4] = 0;
    coop_u32_pack(body + 5, 4, counter);
    const size_t n = coop_seal(pl, sizeof pl, key, 1, COOP_DST_ALL, session, counter, COOP_K_TEST, body, sizeof body);
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_DATA, .src = 1, .dst = COOP_DST_ALL, .room = e->room, .seq = e->seq++};
    evil_send(e, slot, &h, pl, n, NULL);   // (a DATA frame's tag is never looked at: its seal is what counts)
}

// What only a member of the room could have said, and what anybody on the link can say: the JOINED that hands out the
// member id carries the room's tag, so it is taken; a refusal on a link that has proved nothing carries no tag at all.
static void evil_accept(Evil *e, int slot, const CoopConnect *c) {
    uint8_t pl[COOP_MAX_PAYLOAD];
    CoopJoined j = {.member = 2, .nonce = c->nonce};
    if (!evil_proof(j.token)) return;
    evil_roster(e, c->session, 0, &j.roster);
    const size_t n = coop_joined_pack(pl, sizeof pl, &j);
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_JOINED, .src = 1, .dst = 2, .room = e->room, .seq = e->rseq++};
    evil_send(e, slot, &h, pl, n, e->locator);
    e->joined_slot = slot;
    e->joined_session = c->session;
}

static void evil_handle(Evil *e, int slot, const CoopHdr *h, const uint8_t *p) {
    EvLink *L = &e->link[slot - 1];
    uint8_t pl[COOP_MAX_PAYLOAD];
    if (h->type == COOP_T_CONNECT) {
        CoopConnect c;
        if (!coop_connect_parse(p, h->len, &c)) return;
        if (!e->name[0]) snprintf(e->name, sizeof e->name, "%s", c.name);
        if (!L->challenged) {
            L->challenged = 1;
            L->session = c.session;
            if (e->mode == EV_GARBAGE && !e->junked) {   // not a frame at all: a header's worth of bytes that are not
                                         // one, so the link ends in the reading, before any frame is looked at
                e->junked = 1;
                CoopTcp *t = coop_tcp_at(e->set, slot);
                uint8_t junk[COOP_HDR_LEN + 8];
                memset(junk, 0xff, sizeof junk);
                if (t) coop_tcp_send(t, junk, sizeof junk);
                return;
            }
            if (e->mode == EV_SILENT) return;
            if (e->mode == EV_PROTO) {   // a game of another co-op version, answering a CONNECT with an ERROR
                CoopError er = {.reason = COOP_E_PROTO, .nonce = c.nonce};
                snprintf(er.text, sizeof er.text, "another version");
                const size_t n = coop_error_pack(pl, sizeof pl, &er);
                CoopHdr x = {.ver = COOP_PROTO + 1, .type = COOP_T_ERROR, .src = 0, .dst = 0, .room = 0, .seq = e->seq++};
                evil_send(e, slot, &x, pl, n, NULL);
                return;
            }
            if (!evil_proof(L->cookie)) return;
            size_t n = coop_challenge_pack(pl, sizeof pl, c.nonce, L->cookie);
            CoopHdr a = {.ver = COOP_PROTO, .type = COOP_T_CHALLENGE, .src = 0, .dst = 0, .room = 0, .seq = e->seq++};
            evil_send(e, slot, &a, pl, n, NULL);
            n = coop_room_pack(pl, sizeof pl, c.nonce, e->secret);
            CoopHdr b = {.ver = COOP_PROTO, .type = COOP_T_ROOM, .src = 0, .dst = 0, .room = 0, .seq = e->seq++};
            evil_send(e, slot, &b, pl, n, NULL);
            return;
        }
        if (!c.cookie[0]) return;   // a CONNECT that carries no cookie back says nothing new
        if (e->mode == EV_ERROR) {
            const int used = e->error == COOP_E_SESSION_USED;
            if (!used || !e->first_session) {
                if (used) e->first_session = c.session;
                CoopError er = {.reason = (uint8_t)e->error, .nonce = c.nonce};
                snprintf(er.text, sizeof er.text, "no room for you");
                const size_t n = coop_error_pack(pl, sizeof pl, &er);
                CoopHdr a = {.ver = COOP_PROTO, .type = COOP_T_ERROR, .src = 0, .dst = 0, .room = 0, .seq = e->seq++};
                evil_send(e, slot, &a, pl, n, NULL);
                return;
            }
        }
        if (!e->joined_slot) evil_accept(e, slot, &c);
        L->session = c.session;
        return;
    }
    if (h->type == COOP_T_PING && slot == e->joined_slot) {   // a host that answers nothing goes quiet
        uint32_t token;
        if (!coop_u32_parse(p, h->len, &token)) return;
        const size_t n = coop_u32_pack(pl, sizeof pl, token);
        CoopHdr a = {.ver = COOP_PROTO, .type = COOP_T_PONG, .src = 1, .dst = 2, .room = e->room, .seq = e->seq++};
        evil_send(e, slot, &a, pl, n, e->locator);
        return;
    }
}

// One turn of the hostile host: whoever is knocking, the frames its links carry, the links that ended, and one step of
// the mischief this mode is about (at most one every 250 ms, so a player has time to answer it).
static void evil_mischief(Evil *e) {
    if (e->mode < EV_DATA_BAD_KEY || !e->joined_slot) return;
    const uint64_t now = coop_now_ms();
    if (e->at && now - e->at < 250) return;
    e->at = now;
    e->step++;
    const int slot = e->joined_slot;
    CoopRoster r;
    if (e->mode == EV_DATA_BAD_KEY && e->step == 1) { evil_data(e, slot, e->foreign, e->session, 9); return; }
    if (e->mode == EV_DATA_OLD_SESSION && e->step == 1) { evil_data(e, slot, e->msg_key, e->session ^ 1u, 11); return; }
    if (e->mode == EV_DATA_REPLAY && e->step <= 2) { evil_data(e, slot, e->msg_key, e->session, 13); return; }
    if (e->mode == EV_MAKER_ROSTER && e->step == 1) {
        evil_roster(e, e->joined_session, 1, &r);
        evil_publish(e, slot, &r, 1);
        return;
    }
    if (e->mode == EV_FOREIGN_ROSTER) {
        if (e->step == 1) {   // a session that is not the victim's, one roster past the last it took
            evil_roster(e, e->joined_session ^ 0x5555u, 0, &r);
            evil_publish(e, slot, &r, 1);
        } else if (e->step == 2) {   // a roster numbered far past any host would reach: not taken
            evil_roster(e, e->joined_session, 0, &r);
            evil_publish(e, slot, &r, 1u << 30);
        }
    }
}

static void evil_pump(Evil *e, uint32_t ms) {
    uint8_t buf[COOP_MAX_DGRAM];
    for (;;) {
        int slot = 0;
        const int n = coop_tcp_next(e->set, buf, sizeof buf, &slot, ms);
        ms = 0;
        if (n < 0) {
            CoopAddr from;
            CoopTcp *t = coop_tcp_accept(e->listen, &from);
            (void)from;
            if (!t) continue;
            if (!coop_tcp_add(e->set, t)) coop_tcp_close(t);
            continue;
        }
        if (n == 0) break;
        CoopHdr h;
        const uint8_t *p;
        if (coop_dgram_parse(buf, (size_t)n, &h, &p) && slot >= 1 && slot <= 8) evil_handle(e, slot, &h, p);
    }
    for (int i = 1; i <= 8; i++) {
        CoopTcp *t = coop_tcp_at(e->set, i);
        if (t && !coop_tcp_alive(t)) {
            if (e->joined_slot == i) { e->joined_slot = 0; e->joined_session = 0; }
            coop_tcp_drop(e->set, i);
            memset(&e->link[i - 1], 0, sizeof e->link[i - 1]);
        }
    }
    evil_mischief(e);
}

// Wait for the victim, serving its host while waiting: it is the host's answers that move the victim along.
#define EV_WAIT(e, cond, ms) ({ uint64_t t_ = coop_now_ms() + (ms); int ok_; \
    while (!(ok_ = (cond)) && coop_now_ms() < t_) { evil_pump((e), 25); } \
    ok_; })

static int vic_no;
static void vic_tag(char *tag, size_t cap) { snprintf(tag, cap, "Vic%d", ++vic_no); }
static int log_of(const char *tag, const char *what) {
    char needle[160];
    snprintf(needle, sizeof needle, "coop[%s]: %s", tag, what);
    return log_count(needle);
}

// One scenario: a fresh victim session and a fresh hostile host on a port of its own — a port is not reused between
// scenarios, so a link of the one before cannot be taken for this one's.
static int hostile(Player *v, Evil *e, const char *tag, int mode, int error, int retry) {
    char server[64];
    const uint16_t port = free_port();
    server_of(port, server, sizeof server);
    if (!evil_new(e, port, mode, error)) { CHECK(0, "open the hostile host's port %u", (unsigned)port); return 0; }
    player_start(v, tag, 0x77u, server, 0, retry, 0);
    coop_sess_command(v->sess, COOP_CMD_JOIN, "");
    // Its first state is the one a session that has not started anything holds, so a wait for what the hostile host
    // does to it starts by serving the attempt until it is really under way.
    return EV_WAIT(e, (log_of(tag, "connecting to the host") > 0), 6000);
}

static void check_hostile_host(void) {
    Player v;
    Evil e;
    char tag[16];

    // The baseline: a host made of wire bytes only gets a player in, and the player takes the room it is told.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_OK, 0, 1)) return;
    CoopView ev = view_of(&v);
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), ev.state == COOP_JOINED), 8000), "the wire-only host lets its player in (%s)", ev.status);
    CHECK(ev.self == 2 && ev.n == 2 && ev.maker == 1 && !ev.made, "the player is member 2 of a room whose maker is the host (self %d, n %d, maker %d, made %d)",
          ev.self, ev.n, ev.maker, ev.made);
    CHECK(!strcmp(ev.label, "H7K\xe2\x80\xa6"), "it took the room its host is in (%s)", ev.label);
    CHECK(ev.session && ev.member[1].session == ev.session, "the host lists this player's own session");
    CHECK(log_of(tag, "joined room") == 1, "it says it joined");
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), ev.ping_ms > 0), 4000), "its PINGs are answered (%u ms)", ev.ping_ms);
    player_end(&v);
    evil_free(&e);

    // Something that is not a frame at all: the link ends where the bytes are read, and the player starts a new one and
    // gets in. Nothing is logged of the junk — coop_net.c ends the link before a frame is looked at.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_GARBAGE, 0, 1)) return;
    ev = view_of(&v);
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), ev.state == COOP_JOINED), 12000), "the player comes in on the link after the junk (%s)", ev.status);
    CHECK(log_of(tag, "joined room") == 1, "one room, one joining, whatever the junk was");
    player_end(&v);
    evil_free(&e);

    // A port that takes the connection and says nothing.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_SILENT, 0, 1)) return;
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), strstr(view_of(&v).status, "no answer from the host") != 0), 8000),
          "a host that never answers says so (%s)", view_of(&v).status);
    CHECK(view_of(&v).state == COOP_CONNECTING, "the player keeps waiting for it, and is not in a room");
    player_end(&v);
    evil_free(&e);

    // A game of another co-op version answers the first CONNECT with an ERROR it cannot word in this format.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_PROTO, 0, 1)) return;
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), view_of(&v).state == COOP_LEFT), 8000),
          "another co-op version ends the attempt (%s)", view_of(&v).status);
    ev = view_of(&v);
    CHECK(strstr(ev.status, "update required: that game speaks another co-op version") != 0, "it says what is needed (%s)", ev.status);
    CHECK(log_of(tag, "no room: update required") == 1, "and logs it");
    player_end(&v);
    evil_free(&e);

    // Every reason a host can refuse a player for.
    static const struct { int reason; const char *status; const char *log; int retry; } REFUSE[] = {
        {COOP_E_NO_ROOM, "that game is not hosting a room; waiting for it", "", 1},
        {COOP_E_NO_ROOM, "that game is not hosting a room", "no room: that game is not hosting a room", 0},
        {COOP_E_ROOM_FULL, "the room is full", "no room: the room is full", 0},
        {COOP_E_BAD_TAG, "the host does not know this room key", "no room: the host does not know this room key", 0},
        {COOP_E_SERVER_FULL, "that game cannot take another player", "no room: that game cannot take another player", 0},
    };
    for (unsigned i = 0; i < sizeof REFUSE / sizeof REFUSE[0]; i++) {
        vic_tag(tag, sizeof tag);
        if (!hostile(&v, &e, tag, EV_ERROR, REFUSE[i].reason, REFUSE[i].retry)) return;
        const CoopState want = REFUSE[i].retry ? COOP_CONNECTING : COOP_LEFT;
        CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), strstr(ev.status, REFUSE[i].status) != 0), 8000),
              "refused with reason %d says %s (got %s)", REFUSE[i].reason, REFUSE[i].status, view_of(&v).status);
        ev = view_of(&v);
        CHECK(ev.state == want, "refused with reason %d shows it as state %d (got %d, %s)", REFUSE[i].reason, want, ev.state, ev.status);
        if (REFUSE[i].log[0]) CHECK(log_of(tag, REFUSE[i].log) == 1, "refused with reason %d logs it", REFUSE[i].reason);
        CHECK(in_room(&v, 2) == 0, "refused with reason %d is not in the room", REFUSE[i].reason);
        player_end(&v);
        evil_free(&e);
    }

    // A session the room had before: the player is told, tries again with a new one, and gets in.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_ERROR, COOP_E_SESSION_USED, 1)) return;
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), view_of(&v).state == COOP_JOINED), 12000),
          "a new session gets in (state %d, %s)", view_of(&v).state, view_of(&v).status);
    ev = view_of(&v);
    CHECK(log_of(tag, "the host saw this session in the room before; trying again with a new one") == 1, "it says why it tried again");
    CHECK(e.first_session && ev.session != e.first_session, "the attempt it is in now is a different session (%llu vs %llu)",
          (unsigned long long) ev.session, (unsigned long long) e.first_session);
    player_end(&v);
    evil_free(&e);

    // What a member of the room can send on the link, and what a player refuses to take.
    static const struct { int mode; const char *log; int got; } MSG[] = {
        {EV_DATA_BAD_KEY, "a message that does not open with the room's key was thrown away", 0},
        {EV_DATA_OLD_SESSION, "a message of member 1 that is not of its present session, or seen before, was thrown away", 0},
        {EV_DATA_REPLAY, "a message of member 1 that is not of its present session, or seen before, was thrown away", 1},
    };
    for (unsigned i = 0; i < sizeof MSG / sizeof MSG[0]; i++) {
        vic_tag(tag, sizeof tag);
        if (!hostile(&v, &e, tag, MSG[i].mode, 0, 1)) return;
        CHECK(EV_WAIT(&e, (log_of(tag, MSG[i].log) > 0), 12000), "%s: the message is refused (log %d)", MSG[i].log, log_of(tag, MSG[i].log));
        CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), in_room(&v, 2)), 4000), "and it is still in the room (%s)", view_of(&v).status);
        coop_sleep_ms(400);
        CHECK(v.got == (unsigned) MSG[i].got, "%s: it delivered %u message(s), wanted %d", MSG[i].log, v.got, MSG[i].got);
        CHECK(!v.dups, "%s: nothing was delivered twice", MSG[i].log);
        player_end(&v);
        evil_free(&e);
    }

    // A roster that makes the player the room's maker: it is taken for what the view says, and nothing else about the
    // session changes — the player never hosted anything and does not become the room's rules.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_MAKER_ROSTER, 0, 1)) return;
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), in_room(&v, 2)), 8000), "the hostile host lets its player in (%s)",
          view_of(&v).status);
    CHECK(EV_WAIT(&e, (e.step >= 1), 4000), "the roster that marks its player as the maker was published");
    coop_sleep_ms(600);
    CHECK(log_of(tag, "joining again") == 0, "a maker mark of its own does not send the player back to the host (%d)",
          log_of(tag, "joining again"));
    CHECK(in_room(&v, 2) && view_of(&v).maker == 2, "the roster's maker mark shows in the view (maker %d)", view_of(&v).maker);
    ev = view_of(&v);
    CHECK(ev.maker == ev.self && ev.self == 2, "the player is listed as the maker of a room it does not host (maker %d, self %d)", ev.maker, ev.self);
    CHECK(!ev.made, "and it knows it is not the one that made the room");
    CHECK(ev.session && ev.member[1].session == ev.session, "its own session is still the one the roster names");
    player_end(&v);
    evil_free(&e);

    // A roster that lists another session for the player: someone reached the host with its rejoin token. It joins
    // again to take its place back, once. A roster numbered past any host's is not taken at all.
    vic_tag(tag, sizeof tag);
    if (!hostile(&v, &e, tag, EV_FOREIGN_ROSTER, 0, 1)) return;
    CHECK(EV_WAIT(&e, (log_of(tag, "the host lists another session for this player; joining again") > 0), 12000),
          "a roster that lists another session for this player is not sat out");
    // The log line is written before the step that publishes the RECONNECTING view finishes, so the count is taken
    // from the view, not from the log.
    CHECK(EV_WAIT(&e, (v.saw_reconnect >= 1), 8000), "the rejoin shows in the view (%u)", v.saw_reconnect);
    const unsigned rejoins = v.saw_reconnect;
    CHECK(rejoins == 1, "it joins again once (%u)", rejoins);
    CHECK(EV_WAIT(&e, (coop_sess_view(v.sess, &ev), in_room(&v, 2)), 12000), "and gets back in (%s)", view_of(&v).status);
    ev = view_of(&v);
    CHECK(ev.session && ev.member[1].session == ev.session, "its own session is listed again (%llu vs %llu)",
          (unsigned long long) ev.session, (unsigned long long) ev.member[1].session);
    CHECK(EV_WAIT(&e, (e.step >= 2), 4000), "the far-ahead roster was sent");
    coop_sleep_ms(700);
    CHECK(in_room(&v, 2), "a roster numbered 1u<<30 past the last one changes nothing (%s)", view_of(&v).status);
    CHECK(log_of(tag, "the host lists another session for this player; joining again") == 1, "and is not taken as a roster");
    CHECK(v.saw_reconnect == 1, "no second rejoin (%u)", v.saw_reconnect);
    player_end(&v);
    evil_free(&e);

    printf("coop-session-test: a hostile host: the room it names, every refusal it sends, the messages and rosters a player will not take\n");
}

// ---- 3d. the process's own session, started the way the game starts it

// The process's session, as a game starts it: the settings in the environment and the options file, then coop_init, the
// interface the other layers use, the commands, and coop_shutdown as the window closes. Alice hosts ROOM_SECRET on a
// port of her own, Bob is in her room, and this session joins as member 3.
static void check_process(const char *server, unsigned drop, Player *alice, uint32_t *first) {
    char drop_text[16];
    snprintf(drop_text, sizeof drop_text, "%u", drop);
    set_env("HOS_COOP", "1");
    set_env("HOS_COOP_NAME", NULL);
    set_env("HOS_COOP_SERVER", server);
    set_env("HOS_COOP_PORT", NULL);
    set_env("HOS_COOP_AUTO", "join");
    set_env("HOS_COOP_ROOM", "h7k3wm-bcdfgh 234567");   // a secret as a player types it
    set_env("HOS_COOP_DROP", drop_text);
    ini_set("coop_name", "From the file");
    ini_set("coop_server", "127.0.0.1:9");   // (the environment's is taken)
    CoopView v;
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "the process's session is off until coop_init (state %d)", v.state);
    CHECK(!coop_send(COOP_K_TEST, "x", 1) && !coop_command(COOP_CMD_JOIN, ""), "nothing is sent and no command taken while off");
    static Player proc;   // (static: the process's session keeps its handlers until the process ends)
    memset(&proc, 0, sizeof proc);
    proc.name = "process";
    CHECK(coop_on_message(COOP_K_TEST, on_test_message, &proc) && coop_on_view(on_view_change, &proc), "handlers register");
    uint64_t id1 = coop_client_id();
    CHECK(id1 && id1 == coop_client_id(), "the client id stays the same within a start of the game");
    const int threads = tasks(), fds = files();
    coop_init();
    coop_init();   // twice is the same as once
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 30000), "HOS_COOP_AUTO=join joins the room of HOS_COOP_ROOM (state %d, %s)", v.state, v.status);
    CHECK(v.self == 3 && v.n == 3 && !strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.maker == 1 && !v.made,
          "as member 3 of H7K3WM-BCDFGH-234567, made by member 1 (self %d, %d members, %s, maker %d)", v.self, v.n, v.code, v.maker);
    CHECK(!strcmp(v.name, "From the file"), "under the options file's coop_name (%s)", v.name);
    char line[220];
    snprintf(line, sizeof line, "coop: on, as From the file; joining %s (HOS_COOP_SERVER)", server);
    CHECK(log_count(line) == 1, "the log says co-op is on, as whom, and which host the environment named: %s", line);
    CHECK(log_count("coop: HOS_COOP_AUTO=join: room H7K\xe2\x80\xa6 at start") == 1, "and the room at start, by its first characters");
    CHECK(log_count("coop: joined room H7K\xe2\x80\xa6 as member 3") >= 1, "and the join");
    CHECK(!log_count(ROOM_SECRET) && !log_count("H7K3WM-BCDFGH"), "the log has neither the room's secret nor its whole code");
    CHECK(WAIT_FOR(in_room(alice, 3), ROSTER_WAIT_MS), "Alice sees three members");
    CHECK(proc.views > 0, "the view hook ran");

    // messages both ways through the interface other layers use
    alice->expect_from = 3, proc.expect_from = 1;
    unsigned got_before = proc.got, alice_before = alice->got;
    for (int i = 0; i < 40; i++) {
        uint8_t m[9];
        memcpy(m, "probe", 5);
        m[4] = 0;
        coop_u32_pack(m + 5, 4, *first + (uint32_t)i);
        coop_sess_send(alice->sess, 3, COOP_K_TEST, m, sizeof m);
        coop_u32_pack(m + 5, 4, *first + 100 + (uint32_t)i);
        coop_send(COOP_K_TEST, m, sizeof m);   // to everyone: Alice and Bob
        coop_sleep_ms(15);
    }
    coop_sleep_ms(300);
    unsigned min = drop ? 14 : 37;
    CHECK(proc.got - got_before >= min && !proc.dups && !proc.from_wrong, "the process got %u of 40 from Alice (%u twice, %u from the wrong member)",
          proc.got - got_before, proc.dups, proc.from_wrong);
    CHECK(alice->got - alice_before >= min, "Alice got messages from the process: %u", alice->got - alice_before);
    *first += 200;
    static uint8_t too_long[COOP_MAX_MESSAGE + 1];
    CHECK(coop_send_to(1, COOP_K_TEST, "abc", 3) && !coop_send_to(0, COOP_K_TEST, "abc", 3) && !coop_send(COOP_K_TEST, too_long, sizeof too_long),
          "coop_send_to takes a member, a message that is too long is refused");
    CHECK(!coop_remove_member(1) || WAIT_FOR(in_room(alice, 3), 3000), "a member that did not make the room removes nobody");

    // commands
    CHECK(!coop_command(COOP_CMD_JOIN, "not a secret") && !coop_command(COOP_CMD_HOST, "H7K3WM"), "a bad room secret is refused");
    CHECK(coop_command(COOP_CMD_LEAVE, NULL), "leave is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT) && in_room(alice, 2), ROSTER_WAIT_MS), "the process left (state %d)", v.state);
    CHECK(log_count("coop: left room (leave requested)") == 1, "the log says so");
    CHECK(coop_command(COOP_CMD_JOIN, "H7K3WM BCDFGH 234567"), "join is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED) && in_room(alice, 3), ROSTER_WAIT_MS), "and the process joined again (state %d, %s)", v.state, v.status);

    // the menu's settings while the game runs: a new name joins the room again under it; off leaves; on again with a
    // command at once
    CHECK(coop_configure(1, "Renamed", server), "coop_configure takes a new name");
    CHECK(WAIT_FOR(in_room(alice, 3) && !strcmp(view_of(alice).member[2].name, "Renamed"), ROSTER_WAIT_MS), "the new name shows in Alice's roster (%s)",
          view_of(alice).member[2].name);
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED && !strcmp(v.name, "Renamed")), 15000), "and the process is in the room under it (state %d, %s)",
          v.state, v.name);
    CHECK(log_count("coop: joining the room again as Renamed") == 1, "the log says so");
    CHECK(coop_configure(0, "Renamed", server), "coop_configure switches co-op off");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && in_room(alice, 2), ROSTER_WAIT_MS), "off leaves the room (state %d)", v.state);
    CHECK(log_count("coop: left room (co-op turned off)") == 1 && !coop_command(COOP_CMD_JOIN, ROOM_SECRET), "the log says so, and no command is taken");
    CHECK(WAIT_FOR(tasks() == threads && files() == fds, 5000),
          "off in a room ends the co-op thread and closes its link (threads %d -> %d, files %d -> %d)", threads, tasks(), fds, files());
    CHECK(coop_configure(1, "Renamed", server) && coop_command(COOP_CMD_JOIN, ROOM_SECRET), "on again, a join right after it is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED) && in_room(alice, 3), ROSTER_WAIT_MS), "and the process is in the room again (state %d, %s)",
          v.state, v.status);

    // the game's window closes: LEAVE and BYE
    unsigned byes = alice->byes;
    uint64_t t0 = coop_now_ms();
    coop_shutdown();
    CHECK(coop_now_ms() - t0 < 1500, "coop_shutdown returns within a second and a half (%u ms)", (unsigned)(coop_now_ms() - t0));
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "and leaves the session off (state %d)", v.state);
    CHECK(log_count("coop: left room (the game is closing)") == 1, "the log says why it left");
    CHECK(WAIT_FOR(tasks() == threads && files() == fds, 5000),
          "the game closing ends the co-op thread and closes its link (threads %d -> %d, files %d -> %d)", threads, tasks(), fds, files());
    CHECK(WAIT_FOR(in_room(alice, 2), drop ? ROSTER_WAIT_MS : 3000), "Alice sees the process go at once, when its link closes");
    CHECK(drop || WAIT_FOR(alice->byes > byes, 1000), "Alice got the process's BYE");
    set_env("HOS_COOP_AUTO", NULL);
    set_env("HOS_COOP_ROOM", NULL);
    set_env("HOS_COOP_SERVER", NULL);
    set_env("HOS_COOP", NULL);
    ini_set("coop_name", NULL);
    ini_set("coop_server", NULL);
    printf("coop-session-test: the process's session: settings, join at start, messages, commands, goodbye\n");
}

#ifndef _WIN32
// The text of the file at path (at most cap - 1 bytes) into out; "" when it cannot be read.
static const char *file_text(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(out, 1, cap - 1, f) : 0;
    if (f) fclose(f);
    out[n] = 0;
    return out;
}

// A game that is killed says nothing: co-op installs no signal handler in the host, so a game without a window ended by
// SIGTERM (`timeout`, a service manager) ends as it would without co-op. A child process of this file joins the room
// through coop_init (the default name: no setting gives one) and is sent SIGTERM. It must end by that signal at once,
// with no goodbye; the host notices the link closed within tens of ms and the others see it gone then, not after the
// silence time (docs/coop.md, "A game that ends without a goodbye").
static void check_killed(const char *self, const char *work, const char *server, unsigned drop, Player *alice) {
    if (!work || !*work) {
        printf("coop-session-test: no --work folder: the SIGTERM part is skipped\n");
        return;
    }
    char log_path[1024], drop_text[16];
    snprintf(log_path, sizeof log_path, "%s/signal-child.log", work);
    snprintf(drop_text, sizeof drop_text, "%u", drop);
    set_env("HOS_COOP", "1");
    set_env("HOS_COOP_NAME", NULL);
    set_env("HOS_COOP_SERVER", server);
    set_env("HOS_COOP_PORT", NULL);
    set_env("HOS_COOP_AUTO", "join");
    set_env("HOS_COOP_ROOM", ROOM_SECRET);
    set_env("HOS_COOP_DROP", drop_text);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    char *argv[] = {(char *)self, "--signal-child", NULL};
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    set_env("HOS_COOP_AUTO", NULL);
    set_env("HOS_COOP_ROOM", NULL);
    set_env("HOS_COOP_SERVER", NULL);
    set_env("HOS_COOP", NULL);
    if (spawned) FAIL_RET("the child did not start (%d)", spawned);
    const unsigned byes = alice->byes;
    const int away = log_count("(member 3) is away"), left = log_count("(member 3) left the room");
    // in the room as the child itself sees it (its own JOINED may have been lost while Alice's roster already lists it:
    // then it is still connecting), and as Alice sees it
    static char text[65536];
    int joined = WAIT_FOR(strstr(file_text(log_path, text, sizeof text), "coop-session-child: joined as member 3") && in_room(alice, 3),
                          ROSTER_WAIT_MS + 30000);
    CoopView va = view_of(alice);
    CHECK(joined && !strncmp(va.member[2].name, "Player-", 7) && strlen(va.member[2].name) == 11,
          "the child joined under the default name Player-xxxx (%d members, %s)", va.n, va.n > 2 ? va.member[2].name : "");
    uint64_t t0 = coop_now_ms();
    kill(pid, SIGTERM);
    int status = 0;
    pid_t w = 0;
    int ended = WAIT_FOR((w = waitpid(pid, &status, WNOHANG)) == pid, 5000);
    const unsigned took = (unsigned)(coop_now_ms() - t0);
    if (!ended) kill(pid, SIGKILL), waitpid(pid, &status, 0);
    CHECK(ended && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "the child ended by its SIGTERM (status %#x)", status);
    CHECK(took < 1500, "within a second and a half (%u ms)", took);
    file_text(log_path, text, sizeof text);
    CHECK(strstr(text, "coop: on, as Player-") && strstr(text, "coop: joined room H7K\xe2\x80\xa6 as member 3") && !strstr(text, "coop: left room") &&
              !strstr(text, "asked to end"),
          "the child's log has the join and no goodbye (co-op handles no signal):\n%s", text);
    const int gone = WAIT_FOR(in_room(alice, 2), drop ? ROSTER_WAIT_MS : LINK_GONE_MS);
    const unsigned silent = (unsigned)(coop_now_ms() - t0);
    CHECK(gone && silent < LINK_GONE_MS, "Alice sees the killed child go when its link closes, not after the silence time (gone %d, %u ms after the signal)",
          gone, silent);
    CHECK(alice->byes == byes, "and got no BYE from it (%u)", alice->byes - byes);
    CHECK(log_count("(member 3) left the room") > left && log_count("(member 3) is away") == away,
          "the others' logs say the child left the room, with no away first");
    printf("coop-session-test: a child ended by SIGTERM ended at once (%u ms) without a goodbye; Alice saw it gone %u ms later\n", took, silent);
}

__attribute__((noreturn)) static int signal_child(void) {
    log_echo = 1;   // (stdout is the child's log file)
    coop_init();
    CoopView v;
    if (WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 60000)) printf("coop-session-child: joined as member %d\n", v.self);
    fflush(stdout);
    for (;;) coop_sleep_ms(1000);   // until the parent's SIGTERM
}
#endif

// ---- 6. the whole of it: a room this process hosts, and everyone who reaches it
//
// Alice hosts on a port of this process; the others are sessions of the same process that reach her by address. Nothing
// sits between them, so what a player gets is what the game at the other end of its link answers.

static void check_sessions(const char *self, const char *work, unsigned drop) {
    const uint16_t alice_port = free_port();
    char server[64];
    server_of(alice_port, server, sizeof server);
    char text[220];
    char hosting[220];   // (its own buffer: text is reused for the other lines this check counts)
    snprintf(hosting, sizeof hosting, "coop[Alice]: hosting room H7K\xe2\x80\xa6 on port %u", (unsigned)alice_port);
    printf("coop-session-test: Alice hosts a room on %s and the others reach her directly (%u%% of what a player receives is thrown away)\n",
           server, drop);

    Player alice, bob;
    player_start(&alice, "Alice", 0xA11CE, "", drop, 1, alice_port);
    player_start(&bob, "Bob", 0xB0B, server, drop, 1, 0);
    if (!alice.sess || !bob.sess) FAIL_RET("a session did not start");
    CoopView v = view_of(&alice);
    CHECK(v.state == COOP_LEFT, "a session that is on starts in no room (state %d)", v.state);

    // --- a game makes a room on its own port; another one joins it by address
    coop_sess_command(alice.sess, COOP_CMD_HOST, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&alice).state == COOP_JOINED, 30000), "Alice is in the room she hosts (state %d, %s)", view_of(&alice).state,
          view_of(&alice).status);
    v = view_of(&alice);
    CHECK(!strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.self == 1 && v.n == 1 && v.made && v.maker == 1 && !strcmp(v.member[0].name, "Alice"),
          "Alice is member 1 and the maker of room %s (self %d, %d members, made %d)", v.code, v.self, v.n, v.made);
    CHECK(log_count(hosting) == 1, "the log says which room is on which port: %s", hosting);
    CHECK(log_count("coop[Alice]: players on this network can join ") >= 1, "and with the addresses the others can type");
    CHECK(log_count(ROOM_SECRET) == 0, "the room's secret is not in the log");

    coop_sess_command(bob.sess, COOP_CMD_JOIN, "h7k3wm-bcdfgh-234567");   // as a player types it
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "both see two members (Alice %d, Bob %d)", view_of(&alice).n,
          view_of(&bob).n);
    v = view_of(&bob);
    CHECK(v.self == 2 && v.maker == 1 && !v.made && v.n == 2 && !strcmp(v.member[0].name, "Alice") && !strcmp(v.member[1].name, "Bob"),
          "Bob is member 2 and sees Alice as the maker (self %d, maker %d, %d members)", v.self, v.maker, v.n);
    CHECK(log_count("coop[Bob]: joined room H7K\xe2\x80\xa6 as member 2") == 1 && WAIT_FOR(log_count("coop[Alice]: Bob (member 2) joined the room"), 5000),
          "the log says Bob joined, on both sides");
    CHECK(WAIT_FOR(view_of(&bob).ping_ms > 0, 8000), "the round trip to the host is measured (the keep-alive answered)");
    CHECK(WAIT_FOR(settled(&alice, &bob), drop ? ROSTER_WAIT_MS : SETTLE_NO_LOSS_MS),
          "both rosters name the other's present session, so neither throws the other's messages away");
    uint32_t first = 0;
    exchange(&alice, &bob, drop, "in the room", &first);
    CHECK(WAIT_FOR(view_of(&alice).member[1].heard_ms != UINT32_MAX, 3000), "a member whose messages opened counts as heard");

    // --- a player that is told no secret: the host's address is enough, its ROOM says the rest on the link
    Player dana;
    player_start(&dana, "Dana", 0xDA6A, server, 0, 0, 0);
    coop_sess_command(dana.sess, COOP_CMD_JOIN, "");
    CHECK(WAIT_FOR(in_room(&dana, 3) && in_room(&alice, 3), ROSTER_WAIT_MS), "Dana, joining with only an address, gets in (state %d, %s)",
          view_of(&dana).state, view_of(&dana).status);
    CHECK(log_count("coop[Dana]: joined room H7K\xe2\x80\xa6 as member 3") == 1, "and is member 3");
    coop_sess_command(dana.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&dana).state == COOP_LEFT && in_room(&alice, 2), ROSTER_WAIT_MS), "and leaves when told to (state %d)",
          view_of(&dana).state);
    CHECK(log_count("coop[Dana]: left room (leave requested)") == 1, "the log says so");
    exchange(&alice, &bob, drop, "after Dana left", &first);
    player_end(&dana);

    // --- a second game cannot host on the port another one is hosting on
    Player other;
    player_start(&other, "Other", 0x07E, "", drop, 0, alice_port);
    coop_sess_command(other.sess, COOP_CMD_HOST, ROOM_SECRET);
    snprintf(text, sizeof text, "coop[Other]: cannot listen on port %u: is another game hosting on it?", (unsigned)alice_port);
    CHECK(WAIT_FOR(view_of(&other).state == COOP_LEFT && strstr(view_of(&other).status, "cannot listen on port"), 30000),
          "a game that hosts on a port another one holds is told so (state %d, %s)", view_of(&other).state, view_of(&other).status);
    CHECK(log_count(text) == 1, "and the log says what to look for: %s", text);
    CHECK(in_room(&alice, 2) && view_of(&alice).self == 1, "the room that holds the port is untouched (%d members, self %d)", view_of(&alice).n,
          view_of(&alice).self);
    player_end(&other);

    // --- the room holds eight players: the host says so before a ninth can knock
    CoopRaw fill[7];
    int filled = 0;
    for (int i = 0; i < 7; i++) {
        char name[16];
        snprintf(name, sizeof name, "Fill%u", (unsigned)(i + 1));
        if (!raw_join(&fill[i], alice_port, name, 0xF100u + (unsigned)i, ROOM_SECRET, NULL)) break;
        filled++;
    }
    CHECK(filled == 6 && in_room(&alice, 8), "the room fills to eight players, Alice and Bob among them (%d joined, %d members)", filled,
          view_of(&alice).n);
    {
        int present = 0;
        for (int i = 0; i < filled; i++) present += raw_present(&fill[i]);
        CHECK(present == filled, "the host keeps a link open for every player in the room (%d of %d)", present, filled);
    }
    CHECK(fill[6].reason == COOP_E_ROOM_FULL && !fill[6].member, "the ninth is answered with the room being full (reason %d)", fill[6].reason);
    CHECK(log_count("coop[Alice]: refused a player: the room is full") == 1 && log_count("the room is full") >= 1,
          "and the log says who was turned away");
    for (int i = 0; i < filled; i++) raw_close(&fill[i]);
    raw_close(&fill[6]);
    CHECK(WAIT_FOR(in_room(&alice, 2) && in_room(&bob, 2), ROSTER_WAIT_MS), "and the room is two again when they go (%d, %d)",
          view_of(&alice).n, view_of(&bob).n);
    printf("coop-session-test: the room holds eight players, the ninth is told so\n");

    check_rejoin_token(&alice, &bob, alice_port, &first, drop);
    check_dead_link(&alice, &bob, alice_port, &first, drop);

    // --- the host's game starts the room again: the others find it at the same address
    const unsigned recon_b = bob.saw_reconnect;
    const int gone_before = log_count("coop[Bob]: the host is gone; rejoining");
    coop_sess_command(alice.sess, COOP_CMD_HOST, ROOM_SECRET);
    CHECK(WAIT_FOR(log_count(hosting) == 2, 10000), "Alice hosts the room again on the same port");
    CHECK(WAIT_FOR(bob.saw_reconnect > recon_b, 15000), "Bob noticed that the host's link closed (%u)", bob.saw_reconnect - recon_b);
    CHECK(WAIT_FOR(log_count("coop[Bob]: the host is gone; rejoining") > gone_before, 5000), "the log says so");
    CHECK(WAIT_FOR(in_room(&alice, 2) && in_room(&bob, 2), ROSTER_WAIT_MS), "both are in the room again (Alice state %d %s, Bob state %d %s)",
          view_of(&alice).state, view_of(&alice).status, view_of(&bob).state, view_of(&bob).status);
    v = view_of(&bob);
    CHECK(!strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.maker == view_of(&alice).self && !v.made,
          "the maker made the room again under its secret (%s, maker %d)", v.code, v.maker);
    CHECK(log_count("coop[Bob]: rejoined room H7K\xe2\x80\xa6 as member ") == 1, "and Bob's log says he joined it again");
    CHECK(WAIT_FOR(settled(&alice, &bob), drop ? ROSTER_WAIT_MS : SETTLE_NO_LOSS_MS),
          "the new room's rosters name each other's sessions, so nobody joins a second time");
    exchange(&alice, &bob, drop, "after the host started the room again", &first);

    // --- Bob leaves, and comes back
    unsigned byes = alice.byes;
    coop_sess_command(bob.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&bob).state == COOP_LEFT, 5000), "Bob left (state %d)", view_of(&bob).state);
    CHECK(WAIT_FOR(in_room(&alice, 1), drop ? ROSTER_WAIT_MS : 3000), "Alice sees the room with only her in it (%d members)", view_of(&alice).n);
    CHECK(drop || WAIT_FOR(alice.byes > byes, 2000), "Alice got Bob's BYE");   // a goodbye is two frames; the test's loss may throw both away
    coop_sess_command(bob.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "Bob joined again");

    // --- the process's own session, with the settings the game reads
    check_process(server, drop, &alice, &first);
#ifndef _WIN32
    check_killed(self, work, server, drop, &alice);
#else
    (void)self, (void)work;
#endif

    // --- the room's maker removes a member
    Player carol;
    player_start(&carol, "Carol", 0xCA201, server, 0, 0, 0);
    coop_sess_command(carol.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&carol, 3) && in_room(&alice, 3), ROSTER_WAIT_MS), "Carol joins (state %d, %s)", view_of(&carol).state,
          view_of(&carol).status);
    const int carol_id = view_of(&carol).self;
    CHECK(!coop_sess_remove(bob.sess, carol_id) || WAIT_FOR(in_room(&carol, 3), 2000), "Bob, who did not make the room, removes nobody");
    CHECK(WAIT_FOR(log_count("coop[Bob]: only the room's maker can remove a member") == 1, 5000), "and the log says who may");
    coop_sleep_ms(500);
    CHECK(in_room(&carol, 3), "Carol is still in the room");
    CHECK(coop_sess_remove(alice.sess, carol_id), "the maker's removal is taken");
    CHECK(WAIT_FOR(view_of(&carol).state == COOP_LEFT && strstr(view_of(&carol).status, "removed"), ROSTER_WAIT_MS),
          "Carol is removed by the maker: %s", view_of(&carol).status);
    CHECK(WAIT_FOR(in_room(&alice, 2), ROSTER_WAIT_MS), "and gone from Alice's roster");
    CHECK(log_count("coop[Alice]: removing Carol (member ") == 1 && log_count("coop[Carol]: left room (the room's maker removed this player)") == 1,
          "the log says who removed whom");
    coop_sess_command(carol.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&carol, 3) && in_room(&alice, 3), ROSTER_WAIT_MS),
          "and it is this link that ends, not the room's key: the same player is let in again (state %d, %s)", view_of(&carol).state,
          view_of(&carol).status);
    player_end(&carol);
    CHECK(WAIT_FOR(in_room(&alice, 2), ROSTER_WAIT_MS), "and the room is two again");

    // --- a host that is not there, one that says nothing, and no address at all
    uint16_t dead_port = free_port();
    char dead[64];
    server_of(dead_port, dead, sizeof dead);
    Player dave;
    player_start(&dave, "Dave", 0xDA7E, dead, 0, 0, 0);
    coop_sess_command(dave.sess, COOP_CMD_JOIN, ROOM_SECRET);
    snprintf(text, sizeof text, "coop[Dave]: no link to the host at %s", dead);
    CHECK(WAIT_FOR(strstr(view_of(&dave).status, "cannot reach the host at") && log_count(text) >= 1, 30000),
          "with nobody at the address the player is told it cannot reach it: %s", view_of(&dave).status);
    CHECK(view_of(&dave).state == COOP_CONNECTING, "and keeps trying (state %d)", view_of(&dave).state);
    coop_sess_command(dave.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT, 5000), "until it is told to stop (state %d)", view_of(&dave).state);
    CHECK(log_count("coop[Dave]: stopped trying to get into a room") == 1, "and the log says so");
    player_end(&dave);

    CoopTcpListen *quiet = coop_tcp_listen(free_port());
    if (!quiet) FAIL_RET("open a port that answers nothing");
    char quiet_server[64];
    server_of(coop_tcp_listen_port(quiet), quiet_server, sizeof quiet_server);
    player_start(&dave, "Dave", 0xDA7F, quiet_server, 0, 0, 0);
    coop_sess_command(dave.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(strstr(view_of(&dave).status, "no answer from the host") != NULL, 30000),
          "a host that accepts the link and says nothing shows as no answer: %s", view_of(&dave).status);
    coop_sess_command(dave.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT, 5000), "and the player leaves (state %d)", view_of(&dave).state);
    coop_sess_configure(dave.sess, &(CoopParams){.server = "", .name = "Dave", .client_id = 0xDA7F, .on = 1});
    coop_sess_command(dave.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT && strstr(view_of(&dave).status, "no host address configured"), 5000),
          "with no host address given a join says so: %s", view_of(&dave).status);
    CHECK(log_count("coop[Dave]: no host address given") == 1, "in the log too");
    player_end(&dave);
    coop_tcp_listen_close(quiet);

    check_hostile_host();

    // --- the end: BYE for everyone, and no room left
    player_end(&bob);
    CHECK(WAIT_FOR(in_room(&alice, 1), ROSTER_WAIT_MS), "Alice sees Bob go when his session ends");
    player_end(&alice);
    printf("coop-session-test: the room of %s: hosted, joined, filled, lost, made again, and ended\n", server);
}

static void no_work(void *arg) { (void)arg; }

int main(int argc, char **argv) {
    coop_lock_init(&log_lock);
    log_echo = getenv("COOP_TEST_VERBOSE") != NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);
    // The first thread starts the runtime's helpers (TSan's among them). thread_join also waits for this
    // warm-up thread's own kernel task to disappear, so it cannot inflate the later baseline count.
    thread_join(thread_new(no_work, NULL));
    const char *work = NULL;
    unsigned drop = 0;
    int unit = 0, unit_menu = 0, mock = 0, hostile_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unit")) unit = 1;
        else if (!strcmp(argv[i], "--unit-menu")) unit_menu = 1;
        else if (!strcmp(argv[i], "--mock")) mock = 1;
        else if (!strcmp(argv[i], "--hostile")) hostile_only = 1;
        else if (!strcmp(argv[i], "--drop") && i + 1 < argc) drop = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) work = argv[++i];
#ifndef _WIN32
        else if (!strcmp(argv[i], "--signal-child")) return signal_child();
#endif
        else {
            fprintf(stderr, "usage: %s --unit | --unit-menu | --hostile | --mock [--drop N] [--work DIR]\n", argv[0]);
            return 2;
        }
    }
    if (unit) {
        check_net();
        check_server_parse();
        check_stop_order();
        check_off_then_on();
    } else if (unit_menu) {
        check_menu_on();
    } else if (hostile_only) {
        check_hostile_host();
    } else if (mock) {
        check_sessions(argv[0], work, drop);
    } else {
        fprintf(stderr, "usage: %s --unit | --unit-menu | --hostile | --mock [--drop N] [--work DIR]\n", argv[0]);
        return 2;
    }
    if (fails && !log_echo) {
        coop_lock(&log_lock);
        printf("--- the sessions' log ---\n%s--- end of the log ---\n", log_text ? log_text : "");
        coop_unlock(&log_lock);
    }
    printf("coop-session-test: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
