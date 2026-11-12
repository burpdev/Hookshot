// host/coop.c, co-op's session layer, and host/coop_net.c, its operating-system layer, without the game: compiled by
// tests/test_coop_session.py with host/coop_proto.c and Monocypher, one mode per process.
//
//   coop_session_test --unit
//       1. coop_net.c on loopback: addresses, two sockets, IPv4 to a socket of both families, a datagram larger than the
//          buffer dropped, a port in use, random bytes, events, a once, the clock.
//       2. coop_server_parse's cases.
//       3. A stop wakes its waiter only once the view that says "off" is published.
//       4. The process's session switched off (HOS_COOP unset, HOS_COOP=0 over the options file's coop=1): no thread, no
//          socket, no command taken; then switched on by the options file alone, with no relay given; then the game
//          closing, which ends the thread.
//   coop_session_test --unit-menu
//       Co-op off at start and switched on and off by coop_configure, as the menu will: off, on, off cycles with a
//       socket open (a relay that never answers), each on one thread and one socket more and each off none left (counted
//       in /proc/self/task and /proc/self/fd); a stop the thread is too slow for, which it carries out by itself, or
//       which switching on again takes back.
//   coop_session_test --mock [--drop N]
//   coop_session_test --relay HOST:PORT [--drop N] [--work DIR]
//       Two sessions in this process, Alice and Bob, through a relay: the relay of this file (--mock), which also does what
//       a hostile relay can (a changed datagram, a message it sealed itself, a recorded message sent again, garbage from
//       its address, forged rosters, lies to a player who connects), or scripts/coop_relay.py (--relay), which the driver
//       restarts when this prints "coop-session-test: restart the relay now" and answers "restarted". Both: a room made
//       and joined, a create of an open room refused, messages both ways, the relay restarted and both in the room again,
//       Bob leaving (his BYE) and coming back, the process's session started by coop_init from the settings (the
//       environment over the options file) with its commands and coop_shutdown, on Linux a child process in the room
//       ended by SIGTERM, which says nothing: the others see it away, then gone once the relay drops it 15 s later
//       (--work: where its log goes), the maker removing a member who cannot come
//       back, a room and a relay that are not there. --drop N: each session throws away N % of the datagrams it receives.
//   coop_session_test --signal-child   (the child of --relay: joins through coop_init and waits for the signal)
#include "coop_net.h"   // (first: winsock2.h before windows.h)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

#include "coop.h"
#include "coop_proto.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop_session: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)
#define FAIL_RET(...) do { CHECK(0, __VA_ARGS__); return; } while (0)
#define WAIT_FOR(cond, ms) ({ uint64_t t_ = coop_now_ms() + (ms); int ok_; while (!(ok_ = (cond)) && coop_now_ms() < t_) coop_sleep_ms(10); ok_; })
// A member learns of a join or a leave from a roster datagram that may be lost; a relay sends it again within a second and
// every 5 s, so this wait holds eleven of them: at 30 % loss they are all lost once in half a million waits.
#define ROSTER_WAIT_MS 40000
// With no loss a roster step has no lost datagram to wait for, so it is bounded as the code promises: a spoofed or
// listed-elsewhere session settles within 4 s (a rejoin once per backoff, the rosters after it), and after a roster forged
// a little ahead the relay's are taken again within the silence time (1.5 s here) and 4 s, plus up to 1 s for the mock
// relay's next roster and 1 s for a loaded machine.
#define SETTLE_NO_LOSS_MS 4000
#define STALE_NO_LOSS_MS (1500 + 4000 + 1000 + 1000)

static const char ROOM_SECRET[] = "H7K3WMBCDFGH234567";   // tests/coop_vectors.txt's room

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) {
    rng_state ^= rng_state >> 12, rng_state ^= rng_state << 25, rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

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
    char text[160];
    return hos_ini_text(key, text, sizeof text) ? atoi(text) : otherwise;
}

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

    CoopUdp *u1 = coop_udp_open(0), *u2 = coop_udp_open(0);
    CHECK(u1 && u2, "two sockets open");
    if (!u1 || !u2) return;
    CHECK(coop_udp_port(u1) && coop_udp_port(u2) && coop_udp_port(u1) != coop_udp_port(u2), "bound to two free ports");
    CoopUdp *taken = coop_udp_open(coop_udp_port(u1));
    CHECK(!taken, "a port in use cannot be bound again");
    coop_udp_close(taken);
    uint8_t buf[2048], msg[1500];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
    CoopAddr to1, from, expect;
    coop_net_resolve("127.0.0.1", coop_udp_port(u1), &to1);
    uint64_t t0 = coop_now_ms();
    CHECK(coop_udp_recv(u1, buf, sizeof buf, &from, 50) == 0, "nothing to receive is a timeout");
    CHECK(coop_now_ms() - t0 >= 40, "the timeout waited (%u ms)", (unsigned)(coop_now_ms() - t0));
    CHECK(coop_udp_send(u2, &to1, msg, 300), "send to 127.0.0.1");
    int n = coop_udp_recv(u1, buf, sizeof buf, &from, 1000);
    CHECK(n == 300 && !memcmp(buf, msg, 300), "the datagram arrives whole (%d bytes)", n);
    coop_net_resolve("127.0.0.1", coop_udp_port(u2), &expect);
    coop_addr_str(&from, text, sizeof text);
    CHECK(coop_addr_eq(&from, &expect), "the sender is 127.0.0.1 and its port (%s)", text);
    CHECK(coop_udp_send(u1, &from, msg, 10) && coop_udp_recv(u2, buf, sizeof buf, &from, 1000) == 10, "and the answer reaches it");
    CHECK(coop_udp_send(u2, &to1, msg, COOP_MAX_DGRAM + 1) && coop_udp_send(u2, &to1, msg, 20), "send one too large, then one");
    n = coop_udp_recv(u1, buf, COOP_MAX_DGRAM, &from, 1000);
    CHECK(n == 0, "a datagram larger than the buffer is dropped, not cut (%d)", n);
    n = coop_udp_recv(u1, buf, COOP_MAX_DGRAM, &from, 1000);
    CHECK(n == 20, "and the next one arrives (%d)", n);
    CoopAddr to6;
    coop_net_resolve("::1", coop_udp_port(u1), &to6);
    if (coop_udp_send(u2, &to6, msg, 33)) {
        n = coop_udp_recv(u1, buf, sizeof buf, &from, 1000);
        coop_addr_str(&from, text, sizeof text);
        CHECK(n == 33 && !strncmp(text, "[::1]:", 6), "IPv6 loopback: %d bytes from %s", n, text);
    } else {
        printf("coop-session-test: no IPv6 loopback here: that part skipped\n");
    }
    coop_udp_close(u1);
    coop_udp_close(u2);
    coop_udp_close(NULL);

    uint8_t r1[32] = {0}, r2[32] = {0}, zero[32] = {0};
    CHECK(coop_random(r1, sizeof r1) && coop_random(r2, sizeof r2) && memcmp(r1, r2, 32) && memcmp(r1, zero, 32), "random bytes");
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
    printf("coop-session-test: the operating-system layer\n");
}

// ---- 2. the relay's address

static void check_server_parse(void) {
    static const struct { const char *text, *host; unsigned port; int ok; } cases[] = {
        {"relay.example", "relay.example", 24680, 1}, {"relay.example:5000", "relay.example", 5000, 1},
        {"  relay.example  ", "relay.example", 24680, 1}, {"127.0.0.1:65535", "127.0.0.1", 65535, 1},
        {"[::1]", "::1", 24680, 1}, {"[::1]:99", "::1", 99, 1}, {"2001:db8::1", "2001:db8::1", 24680, 1},
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
    CHECK(!coop_server_parse("relay.example", small, sizeof small, &port), "a host that does not fit is refused");
    printf("coop-session-test: coop_server_parse\n");
}

// ---- 3. a stop's order: the stop is signalled only once the view that says the session is off is published, so whoever
// waited for it (coop_shutdown's caller, a menu that switched co-op off) cannot read the view of the step before. One
// session stepped on this thread, no relay: a view hook that sees "off" notes whether the stop was signalled already.

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

static void check_off_then_on(void) {
    set_env("HOS_COOP", NULL), set_env("HOS_COOP_AUTO", NULL), set_env("HOS_COOP_SERVER", NULL), set_env("HOS_COOP_NAME", NULL);
    CoopView v;
    coop_view_get(&v);   // (makes the session's memory, nothing more)
    const int threads = count_entries("/proc/self/task"), fds = count_entries("/proc/self/fd");
    coop_init();
    set_env("HOS_COOP", "0");
    ini_set("coop", "1");
    coop_init();         // the environment over the options file
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "co-op is off unless switched on (state %d)", v.state);
    CHECK(count_entries("/proc/self/task") == threads && count_entries("/proc/self/fd") == fds,
          "off, coop_init starts no thread and opens no socket (threads %d -> %d, files %d -> %d)", threads,
          count_entries("/proc/self/task"), fds, count_entries("/proc/self/fd"));
    CHECK(!coop_command(COOP_CMD_CREATE, NULL) && !coop_send(COOP_K_TEST, "x", 1), "off, no command is taken and nothing sent");
    CHECK(!log_count("coop:"), "off, nothing is logged");
    coop_shutdown();   // (nothing to end)

    // the options file alone switches it on: coop=1, the name from coop_name=, no relay
    set_env("HOS_COOP", NULL);
    ini_set("coop_name", "From the file");
    coop_init();
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT), 2000), "coop=1 in the options file switches co-op on (state %d)", v.state);
    CHECK(!strcmp(v.name, "From the file"), "with the options file's coop_name (%s)", v.name);
    CHECK(log_count("coop: on, as From the file; no relay given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)") == 1,
          "the log says co-op is on, as whom, and that no relay was given");
    CHECK(coop_command(COOP_CMD_CREATE, NULL), "a create is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT && strstr(v.status, "no server")), 2000), "and says there is no relay: %s", v.status);
    CHECK(log_count("coop: no relay given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)") == 1, "in the log too");
    // the game closes: no room to say goodbye to, and the thread ends
    const uint64_t t0 = coop_now_ms();
    coop_shutdown();
    const unsigned took = (unsigned)(coop_now_ms() - t0);
    coop_view_get(&v);
    CHECK(took < 1500 && v.state == COOP_OFF, "coop_shutdown returns within a second and a half and leaves the session off (%u ms, state %d)", took,
          v.state);
    CHECK(WAIT_FOR(count_entries("/proc/self/task") == threads, 5000) && count_entries("/proc/self/fd") == fds,
          "the game closing ends the co-op thread: no thread and no file more than before co-op was on (threads %d -> %d, files %d -> %d)", threads,
          count_entries("/proc/self/task"), fds, count_entries("/proc/self/fd"));
    CHECK(!coop_command(COOP_CMD_CREATE, NULL), "and no command is taken");
    coop_shutdown();   // (twice is the same as once)
    printf("coop-session-test: the process's session: off without HOS_COOP or coop=1 (no thread, no socket), on by the options file, ended as the "
           "game closes (%u ms)\n", took);
}

// A view hook that holds the coop thread up when it sees the session go off, as a name lookup that does not return would.
static int hold_off_ms;
static void on_view_hold(const CoopView *v, void *user) {
    (void)user;
    const int ms = __atomic_load_n(&hold_off_ms, __ATOMIC_ACQUIRE);
    if (v->state == COOP_OFF && ms) coop_sleep_ms((uint32_t)ms);
}

// The process's threads and open files (Linux; 0 and 0 elsewhere), for "one more" and "none left".
static int tasks(void) { return count_entries("/proc/self/task"); }
static int files(void) { return count_entries("/proc/self/fd"); }

// The menu switches co-op on while the game runs (it was off at start): coop_configure starts the thread, a command
// right after it is queued, and off ends the thread again. No relay: the command ends in "no server". Then off, on, off
// with a relay that never answers, so that a socket is open each time; and stops the thread is too slow for.
static void check_menu_on(void) {
    set_env("HOS_COOP", NULL), set_env("HOS_COOP_AUTO", NULL), set_env("HOS_COOP_SERVER", NULL), set_env("HOS_COOP_NAME", NULL);
    coop_init();   // off at start
    CoopView v;
    coop_view_get(&v);
    const int threads = count_entries("/proc/self/task");
    CHECK(v.state == COOP_OFF && !coop_command(COOP_CMD_CREATE, NULL), "off at start, no command is taken (state %d)", v.state);
    CHECK(coop_configure(0, "Menu", "") && count_entries("/proc/self/task") == threads, "the menu switching it off starts nothing");
    CHECK(coop_configure(1, "  Menu\x01name ", ""), "the menu switches co-op on");
    CHECK(coop_command(COOP_CMD_CREATE, NULL), "a create right after it is queued, before the thread has applied it");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT && strstr(v.status, "no server")), 3000),
          "the thread started, applied the menu's settings, then the create, which says there is no relay (state %d, %s)", v.state, v.status);
    CHECK(!strcmp(v.name, "  Menu?name "), "the view's name is the cleaned name in use (\"%s\")", v.name);
    CHECK(count_entries("/proc/self/task") <= 0 || count_entries("/proc/self/task") == threads + 1, "one thread more");
    CHECK(log_count("coop: on, as   Menu?name ; no relay given") == 1, "the log says co-op is on, as whom");
    CHECK(coop_configure(1, "", "relay.invalid") && WAIT_FOR((coop_view_get(&v), !strncmp(v.name, "Player-", 7)), 3000),
          "an empty name is the default Player-xxxx (%s)", v.name);
    CHECK(coop_configure(0, "", "") && WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF), 3000), "the menu switches it off again (state %d)", v.state);
    CHECK(!coop_command(COOP_CMD_CREATE, NULL), "and no command is taken");
    CHECK(WAIT_FOR(tasks() == threads, 5000), "and the co-op thread has ended (threads %d, before co-op was on %d)", tasks(), threads);
    printf("coop-session-test: the menu switches co-op on and off while the game runs\n");

    // off, on, off: each on starts a thread, and its create opens a socket towards a relay that never answers (a UDP
    // socket of this test on loopback); each off leaves neither
    CoopUdp *silent = coop_udp_open(0);
    if (!silent) FAIL_RET("a socket for the relay that never answers");
    char server[32];
    snprintf(server, sizeof server, "127.0.0.1:%u", (unsigned)coop_udp_port(silent));
    const int fds = files(), more = threads > 0;   // (more: 1 where threads and files can be counted)
    unsigned slowest = 0;
    for (int i = 1; i <= 3; i++) {
        CHECK(coop_configure(1, "Cycle", server) && coop_command(COOP_CMD_CREATE, NULL), "cycle %d: on, and a create is taken", i);
        CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_CONNECTING) && files() == fds + more, 5000),
              "cycle %d: the session reaches for the relay, with one socket more (state %d, %s; files %d -> %d)", i, v.state, v.status, fds, files());
        CHECK(tasks() == threads + more, "cycle %d: and one thread more (%d -> %d)", i, threads, tasks());
        const uint64_t t0 = coop_now_ms();
        CHECK(coop_configure(0, "Cycle", server), "cycle %d: off", i);
        const unsigned took = (unsigned)(coop_now_ms() - t0);
        if (took > slowest) slowest = took;
        CHECK(took < 1500, "cycle %d: switching co-op off returns within a second and a half (%u ms)", i, took);
        CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && tasks() == threads && files() == fds, 5000),
              "cycle %d: off leaves no thread and no socket (state %d; threads %d -> %d, files %d -> %d)", i, v.state, threads, tasks(), fds, files());
        CHECK(!coop_command(COOP_CMD_CREATE, NULL), "cycle %d: and no command is taken", i);
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
    CHECK(coop_configure(1, "Held", server) && coop_command(COOP_CMD_CREATE, NULL) &&
              WAIT_FOR((coop_view_get(&v), v.state == COOP_CONNECTING) && tasks() == threads + more && files() == fds + more, 5000),
          "the next on starts another thread (state %d, threads %d -> %d, files %d -> %d)", v.state, threads, tasks(), fds, files());
    // held up again, and switched on again before the thread got to its end: the stop is taken back, the same thread goes on
    __atomic_store_n(&hold_off_ms, 2500, __ATOMIC_RELEASE);
    CHECK(coop_configure(0, "Held", server), "off again, held up again");
    __atomic_store_n(&hold_off_ms, 0, __ATOMIC_RELEASE);
    CHECK(coop_configure(1, "Back", server) && coop_command(COOP_CMD_CREATE, NULL), "on again before the thread has ended");
    CHECK(tasks() == threads + more, "no second thread (threads %d -> %d)", threads, tasks());
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_CONNECTING && !strcmp(v.name, "Back")) && files() == fds + more, 8000),
          "the same thread goes on: on under the new name, the create carried out (state %d, %s)", v.state, v.name);
    CHECK(tasks() == threads + more, "still one thread (threads %d -> %d)", threads, tasks());
    t0 = coop_now_ms();
    coop_shutdown();   // the game closes while the session reaches for the relay
    took = (unsigned)(coop_now_ms() - t0);
    CHECK(took < 1500 && WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && tasks() == threads && files() == fds, 5000),
          "the game closing ends it: no thread and no socket left (%u ms, state %d; threads %d -> %d, files %d -> %d)", took, v.state, threads, tasks(),
          fds, files());
    coop_udp_close(silent);
    printf("coop-session-test: a stop that is held up is waited for 0.4 s; the thread then ends by itself, or goes on when co-op is switched on again\n");
}

// ---- 5a. a relay of this file

typedef struct {
    int used, maker;
    uint64_t cid, session;
    char name[COOP_NAME_MAX + 1];
    CoopAddr addr;
    uint32_t connect_seq;
    uint8_t token[COOP_COOKIE_LEN];
} MockMember;
typedef struct { int used; uint32_t id; uint8_t locator[COOP_KEY_LEN]; uint64_t banned[4]; MockMember m[COOP_MAX_MEMBERS + 1]; } MockRoom;

typedef struct {
    CoopUdp *sock;
    Thread thread;
    uint16_t port;
    volatile int stop, restart, corrupt_next, fuzz, forge, lie, record_from, replay, steal, forge_ahead;
    volatile int mark, spoof;   // the roster names this member the maker (toggles), or lists another session for it
    volatile uint32_t forge_roster;      // a roster to member 1 this far ahead of the mock's counter, another session for member 2
    volatile uint64_t rate_cid;          // CONNECTs of this client are refused for the network's joins
    volatile uint64_t watch_cid;         // CONNECTs of this client with their proof (a cookie or token) are counted in watch_n
    volatile unsigned watch_n;
    MockRoom rooms[4];
    uint8_t secret[32];
    uint32_t seq;
    uint64_t next_roster;
    uint8_t recorded[COOP_MAX_DGRAM];   // a DATA datagram of member record_from, for a replay later
    size_t recorded_n;
    uint32_t counter[COOP_MAX_MEMBERS + 1];   // the envelope counter of each member's last DATA forwarded
} MockRelay;

static void mock_send(MockRelay *r, const CoopAddr *to, uint8_t type, uint32_t room, uint8_t dst, const void *payload, size_t n, const uint8_t *key) {
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = room, .dst = dst, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, key);
    if (k) coop_udp_send(r->sock, to, d, k);
}

static void mock_error(MockRelay *r, const CoopAddr *to, uint8_t reason, uint64_t nonce, const uint8_t *key) {
    CoopError e = {.reason = reason, .nonce = nonce};
    strcpy(e.text, "refused");
    uint8_t payload[80];
    mock_send(r, to, COOP_T_ERROR, 0, 0, payload, coop_error_pack(payload, sizeof payload, &e), key);
}

static void mock_cookie(MockRelay *r, const CoopAddr *a, uint8_t out[COOP_COOKIE_LEN]) {
    uint8_t who[18];
    memcpy(who, a->ip, 16);
    who[16] = (uint8_t)a->port, who[17] = (uint8_t)(a->port >> 8);
    coop_blake2b(out, COOP_COOKIE_LEN, r->secret, sizeof r->secret, who, sizeof who);
}

static void mock_roster_of(const MockRoom *room, CoopRoster *out) {
    memset(out, 0, sizeof *out);
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used) {
            CoopMemberInfo *m = &out->m[out->n++];
            m->id = (uint8_t)i, m->flags = room->m[i].maker ? COOP_FLAG_MAKER : 0, m->session = room->m[i].session;
            memcpy(m->name, room->m[i].name, sizeof m->name);
        }
}

static void mock_roster_to_all(MockRelay *r, MockRoom *room, int skip) {
    CoopRoster ro;
    uint8_t payload[300];
    mock_roster_of(room, &ro);
    size_t n = coop_roster_pack(payload, sizeof payload, &ro);
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used && i != skip) mock_send(r, &room->m[i].addr, COOP_T_ROSTER, room->id, (uint8_t)i, payload, n, room->locator);
}

static void mock_connect(MockRelay *r, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, const CoopAddr *from) {
    CoopConnect c;
    if (n < COOP_CONNECT_MIN || !coop_connect_parse(p, h->len, &c) || (c.intent != COOP_INTENT_CREATE && c.intent != COOP_INTENT_JOIN)) return;
    MockRoom *room = NULL;
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && !memcmp(r->rooms[i].locator, c.locator, COOP_KEY_LEN)) room = &r->rooms[i];
    int id = 0;
    // the same member with its token (its id, no cookie needed), or the same CONNECT sent again because the JOINED was lost
    for (int i = 1; room && i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used && room->m[i].cid == c.client_id &&
            (!memcmp(room->m[i].token, c.token, COOP_COOKIE_LEN) || (room->m[i].connect_seq == h->seq && coop_addr_eq(&room->m[i].addr, from))))
            id = i;
    uint8_t cookie[COOP_COOKIE_LEN];
    mock_cookie(r, from, cookie);
    if (!id && memcmp(cookie, c.cookie, sizeof cookie)) {
        uint8_t payload[32];
        mock_send(r, from, COOP_T_CHALLENGE, 0, 0, payload, coop_challenge_pack(payload, sizeof payload, c.nonce, cookie), c.locator);
        return;
    }
    if (!coop_dgram_verify(d, n, c.locator)) return;
    if (r->watch_cid && c.client_id == r->watch_cid) r->watch_n++;
    if (r->rate_cid && c.client_id == r->rate_cid) return mock_error(r, from, COOP_E_RATE, c.nonce, c.locator);
    if (r->lie) {   // answers this attempt never asked for: another nonce, no tag, another room; then the real one
        r->lie = 0;
        mock_error(r, from, COOP_E_ROOM_FULL, c.nonce ^ 1, c.locator);
        mock_error(r, from, COOP_E_REMOVED, c.nonce, NULL);
        uint8_t d2[64], other[COOP_KEY_LEN];
        CoopHdr eh = {.ver = 1, .type = COOP_T_ERROR, .seq = ++r->seq};
        size_t k = coop_dgram_pack(d2, sizeof d2, &eh, "\x03\x00", 2, NULL);   // a relay of another version
        coop_udp_send(r->sock, from, d2, k);
        coop_room_keys("ZZZZZZZZZZZZZZZZZZ", other, NULL);
        CoopJoined fake = {.member = 5, .nonce = c.nonce};
        uint8_t payload[300];
        size_t pn = coop_joined_pack(payload, sizeof payload, &fake);
        CoopHdr jh = {.ver = COOP_PROTO, .type = COOP_T_JOINED, .room = 0xBAD, .dst = 5, .seq = ++r->seq};
        uint8_t d3[400];
        k = coop_dgram_pack(d3, sizeof d3, &jh, payload, pn, other);
        coop_udp_send(r->sock, from, d3, k);
    }
    if (!room && c.intent == COOP_INTENT_JOIN) return mock_error(r, from, COOP_E_NO_ROOM, c.nonce, c.locator);
    if (!room && c.intent == COOP_INTENT_CREATE && r->steal) {   // someone else made the room first (after a restart)
        r->steal = 0;
        for (int i = 0; i < 4 && !room; i++)
            if (!r->rooms[i].used) room = &r->rooms[i];
        if (!room) return;
        memset(room, 0, sizeof *room);
        room->used = 1, room->id = 0xC0DE0000u + rnd32() % 0xFFFF + 1;
        memcpy(room->locator, c.locator, COOP_KEY_LEN);
        MockMember *thief = &room->m[1];
        thief->used = 1, thief->maker = 1, thief->cid = 0x7417, thief->session = 0x7417;
        strcpy(thief->name, "Thief");
        coop_net_resolve("127.0.0.1", r->port, &thief->addr);   // the mock's own address: what it sends there it drops
        return mock_error(r, from, COOP_E_ROOM_EXISTS, c.nonce, c.locator);
    }
    if (room && c.intent == COOP_INTENT_CREATE && !id) return mock_error(r, from, COOP_E_ROOM_EXISTS, c.nonce, c.locator);
    if (!room) {
        for (int i = 0; i < 4 && !room; i++)
            if (!r->rooms[i].used) room = &r->rooms[i];
        if (!room) return;
        memset(room, 0, sizeof *room);
        room->used = 1, room->id = 0xC0DE0000u + rnd32() % 0xFFFF + 1;
        memcpy(room->locator, c.locator, COOP_KEY_LEN);
    }
    for (int i = 0; i < 4; i++)
        if (room->banned[i] && room->banned[i] == c.client_id) return mock_error(r, from, COOP_E_REMOVED, c.nonce, room->locator);
    int fresh = !id, maker = 1;
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++) maker &= !room->m[i].used;
    for (int i = 1; i <= COOP_MAX_MEMBERS && !id; i++)
        if (!room->m[i].used) id = i;
    if (!id) return mock_error(r, from, COOP_E_ROOM_FULL, c.nonce, room->locator);
    MockMember *m = &room->m[id];
    if (fresh) {
        memset(m, 0, sizeof *m);
        m->maker = maker && c.intent == COOP_INTENT_CREATE;
        m->cid = c.client_id;
        coop_random(m->token, sizeof m->token);
    }
    fresh |= m->connect_seq != h->seq || !m->used;
    m->used = 1, m->addr = *from, m->connect_seq = h->seq, m->session = c.session;
    memcpy(m->name, c.name, sizeof m->name);
    CoopJoined j = {.member = (uint8_t)id, .nonce = c.nonce};
    memcpy(j.token, m->token, COOP_COOKIE_LEN);
    mock_roster_of(room, &j.roster);
    uint8_t payload[400];
    mock_send(r, from, COOP_T_JOINED, room->id, (uint8_t)id, payload, coop_joined_pack(payload, sizeof payload, &j), room->locator);
    if (fresh) mock_roster_to_all(r, room, id);
}

static void mock_handle(MockRelay *r, uint8_t *d, size_t n, const CoopAddr *from) {
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(d, n, &h, &p) || h.ver != COOP_PROTO) return;
    if (h.type == COOP_T_CONNECT) return mock_connect(r, d, n, &h, p, from);
    MockRoom *room = NULL;
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && r->rooms[i].id == h.room) room = &r->rooms[i];
    if (!room || h.src < 1 || h.src > COOP_MAX_MEMBERS || !room->m[h.src].used || !coop_addr_eq(&room->m[h.src].addr, from)) return;
    if (!coop_dgram_verify(d, n, room->locator)) return;
    uint8_t member;
    uint64_t session;
    if (h.type == COOP_T_PING) {
        mock_send(r, from, COOP_T_PONG, room->id, h.src, p, h.len > 4 ? 4 : h.len, room->locator);
    } else if (h.type == COOP_T_LEAVE) {
        room->m[h.src].used = 0;
        mock_roster_to_all(r, room, 0);
    } else if (h.type == COOP_T_KICK && room->m[h.src].maker && coop_kick_parse(p, h.len, &member, &session) && member >= 1 &&
               member <= COOP_MAX_MEMBERS && room->m[member].used && room->m[member].session == session) {
        for (int i = 0; i < 4; i++)
            if (!room->banned[i]) {
                room->banned[i] = room->m[member].cid;
                break;
            }
        CoopError e = {.reason = COOP_E_REMOVED};
        uint8_t payload[80];
        mock_send(r, &room->m[member].addr, COOP_T_ERROR, room->id, member, payload, coop_error_pack(payload, sizeof payload, &e), room->locator);
        room->m[member].used = 0;
        mock_roster_to_all(r, room, 0);
    } else if (h.type == COOP_T_DATA) {
        uint32_t c;
        if (coop_envelope_peek(p, h.len, &session, &c)) r->counter[h.src] = c;
        if (r->record_from == h.src && n <= sizeof r->recorded) {   // kept for a replay after its sender joined again
            memcpy(r->recorded, d, n);
            r->recorded_n = n;
            r->record_from = 0;
        }
        if (r->corrupt_next) {   // one forwarded datagram with a flipped tag bit: no receiver may deliver it
            r->corrupt_next = 0;
            d[n - 1] ^= 1;
        }
        for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
            if (room->m[i].used && i != h.src && (h.dst == COOP_DST_ALL || h.dst == i)) coop_udp_send(r->sock, &room->m[i].addr, d, n);
    }
}

static MockRoom *mock_room_of(MockRelay *r, int member) {
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && r->rooms[i].m[member].used) return &r->rooms[i];
    return NULL;
}

// What a hostile relay can do with what it knows (the locator, every member's session, every datagram it forwards): a
// message to member 1 in member 2's name and session, sealed with a key it made up, tagged right.
static void mock_forge(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (!room || !room->m[2].used) return;
    uint8_t fake_key[COOP_MSG_KEY_LEN], payload[COOP_MAX_PAYLOAD], d[COOP_MAX_DGRAM];
    coop_random(fake_key, sizeof fake_key);
    uint8_t m[9] = {'p', 'r', 'o', 'b', 'e', 0};
    coop_u32_pack(m + 5, 4, 600);
    size_t n = coop_seal(payload, sizeof payload, fake_key, 2, COOP_DST_ALL, room->m[2].session, 0x7FFFFFF0u, COOP_K_TEST, m, sizeof m);
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_DATA, .room = room->id, .src = 2, .dst = COOP_DST_ALL, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, room->locator);
    coop_udp_send(r->sock, &room->m[1].addr, d, k);
}

// What a relay run by someone who knows the room's secret (or a sender from its address on member 1's network) can do
// with the message key: one message to member 1 in member 2's name and session, sealed with that key, its counter
// 2^31 - 1 past member 2's last one, of a kind nobody handles. Three copies: the test's loss.
static void mock_forge_ahead(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (!room || !room->m[2].used) return;
    uint8_t key[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN], payload[COOP_MAX_PAYLOAD], d[COOP_MAX_DGRAM];
    coop_room_keys(ROOM_SECRET, key, mk);
    size_t n = coop_seal(payload, sizeof payload, mk, 2, COOP_DST_ALL, room->m[2].session, r->counter[2] + 0x7FFFFFFFu, 0xF1, "x", 1);
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_DATA, .room = room->id, .src = 2, .dst = COOP_DST_ALL, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, room->locator);
    for (int i = 0; i < 3; i++) coop_udp_send(r->sock, &room->m[1].addr, d, k);
}

static void mock_replay(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (room && r->recorded_n) coop_udp_send(r->sock, &room->m[1].addr, r->recorded, r->recorded_n);
}

// Datagrams of every kind to member 1 of the first room from the relay's own address, as a hostile or broken relay would:
// bytes at random, valid ones changed or cut, and ones carrying the room's tag with a random payload (stale roster
// counters, and DATA no key opens).
static void mock_fuzz(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (!room) return;
    CoopAddr to = room->m[1].addr;
    uint8_t d[1400], payload[COOP_MAX_PAYLOAD];
    for (int it = 0; it < 6000 && !r->stop; it++) {
        size_t n;
        int mode = it % 3;
        if (mode == 0) {
            n = rnd32() % 1300;
            for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(rnd32() % 4 ? rnd32() : 0);
            if (n > 12 && rnd32() % 2) d[0] = 0x4E, d[1] = 0x43, d[2] = COOP_PROTO;
        } else {
            size_t pn = rnd32() % 200;
            for (size_t i = 0; i < pn; i++) payload[i] = (uint8_t)rnd32();
            static const uint8_t types[] = {COOP_T_CHALLENGE, COOP_T_JOINED, COOP_T_ERROR, COOP_T_ROSTER, COOP_T_PONG, COOP_T_DATA, COOP_T_PING, 0x77};
            uint8_t type = types[rnd32() % sizeof types];
            if (type == COOP_T_DATA && pn > 13) payload[0] = COOP_ENV_V1, memcpy(payload + 1, &room->m[2].session, 8);
            CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = mode == 1 ? room->id : rnd32(), .src = (uint8_t)(rnd32() % 10),
                         .dst = (uint8_t)(rnd32() % 3 == 0 ? COOP_DST_ALL : 1), .seq = type == COOP_T_ROSTER ? 0 : rnd32()};
            n = coop_dgram_pack(d, sizeof d, &h, payload, pn, room->locator);
            if (mode == 2 && n) d[rnd32() % n] ^= (uint8_t)(1u << (rnd32() % 8));
        }
        coop_udp_send(r->sock, &to, d, n);
        if (it % 40 == 0) coop_sleep_ms(1);
    }
}

static void mock_main(void *arg) {
    MockRelay *r = arg;
    uint8_t buf[2048];
    CoopAddr from;
    while (!r->stop) {
        if (r->restart) {   // the process went away and came back: rooms gone, a new cookie secret, 400 ms of silence
            memset(r->rooms, 0, sizeof r->rooms);
            coop_random(r->secret, sizeof r->secret);
            uint64_t until = coop_now_ms() + 400;
            while (coop_now_ms() < until) coop_udp_recv(r->sock, buf, sizeof buf, &from, 20);
            r->restart = 0;
        }
        if (r->fuzz) mock_fuzz(r), r->fuzz = 0;
        if (r->forge) mock_forge(r), r->forge = 0;
        if (r->forge_ahead) mock_forge_ahead(r), r->forge_ahead = 0;
        for (; r->replay; r->replay--) mock_replay(r);   // several copies: the test's loss may drop some
        if (r->mark || r->spoof) {
            MockRoom *room = mock_room_of(r, r->mark ? r->mark : r->spoof);
            if (room && r->mark) room->m[r->mark].maker = !room->m[r->mark].maker;
            if (room && r->spoof) room->m[r->spoof].session ^= 0x5005;
            if (room) mock_roster_to_all(r, room, 0);
            r->mark = r->spoof = 0;
        }
        if (r->forge_roster) {   // someone on the path who saw a CONNECT: a roster with a number of its choosing
            MockRoom *room = mock_room_of(r, 1);
            if (room && room->m[2].used) {
                CoopRoster ro;
                mock_roster_of(room, &ro);
                for (int i = 0; i < ro.n; i++)
                    if (ro.m[i].id == 2) ro.m[i].session ^= 0xBAD;
                uint8_t payload[300], d[COOP_MAX_DGRAM];
                size_t pn = coop_roster_pack(payload, sizeof payload, &ro);
                CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_ROSTER, .room = room->id, .dst = 1, .seq = r->seq + r->forge_roster};
                size_t k = coop_dgram_pack(d, sizeof d, &h, payload, pn, room->locator);
                for (int i = 0; i < 3; i++) coop_udp_send(r->sock, &room->m[1].addr, d, k);   // three copies: the test's loss
            }
            r->forge_roster = 0;
        }
        int n = coop_udp_recv(r->sock, buf, sizeof buf, &from, 10);
        if (n > 0) mock_handle(r, buf, (size_t)n, &from);
        if (coop_now_ms() >= r->next_roster) {   // the roster again every second: a lost update repairs itself
            r->next_roster = coop_now_ms() + 1000;
            for (int i = 0; i < 4; i++)
                if (r->rooms[i].used) mock_roster_to_all(r, &r->rooms[i], 0);
        }
    }
}

static MockRelay *mock_start(void) {
    MockRelay *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->sock = coop_udp_open(0);
    if (!r->sock) {
        free(r);
        return NULL;
    }
    r->port = coop_udp_port(r->sock);
    coop_random(r->secret, sizeof r->secret);
    r->thread = thread_new(mock_main, r);
    return r;
}

static void mock_stop(MockRelay *r) {
    r->stop = 1;
    thread_join(r->thread);
    coop_udp_close(r->sock);
    free(r);
}

// ---- 5b. two sessions

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

static uint16_t free_port(void) {
    CoopUdp *u = coop_udp_open(0);
    uint16_t port = u ? coop_udp_port(u) : 0;
    coop_udp_close(u);
    return port;
}

static void player_start(Player *p, const char *name, uint64_t cid, const char *server, unsigned drop, int retry, uint16_t local_port) {
    CoopParams cp = {.tag = name, .server = server, .name = name, .client_id = cid, .on = 1, .drop_pct = drop, .retry = retry,
                     .local_port = local_port, .ping_ms = 200, .silence_ms = 1500, .burst_gap_ms = 100, .backoff_first_ms = 150,
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

// The mock relay lists each player's present session, and both rosters name both: no roster still to come lists
// another session for either (one sent before has a lower number and is refused), so neither throws the other's
// messages away or joins again until the relay is made to lie again.
static int settled(MockRelay *mock, Player *a, Player *b) {
    Player *both[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        CoopView v = view_of(both[i]);
        MockRoom *room = v.state == COOP_JOINED ? mock_room_of(mock, v.self) : NULL;
        if (!room || room->m[v.self].session != v.session) return 0;
    }
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

// The process's session, as the game starts it: the settings in the environment and the options file, then coop_init,
// the interface other layers use, the commands, and coop_shutdown as the game's window closes. Alice and Bob are in the
// room of ROOM_SECRET.
static void check_process(const char *server, unsigned drop, Player *alice, uint32_t *first) {
    char drop_text[16];
    snprintf(drop_text, sizeof drop_text, "%u", drop);
    set_env("HOS_COOP", "1");
    set_env("HOS_COOP_NAME", NULL);
    set_env("HOS_COOP_SERVER", server);
    set_env("HOS_COOP_AUTO", "join");
    set_env("HOS_COOP_ROOM", "h7k3wm-bcdfgh 234567");   // a secret as a player types it
    set_env("HOS_COOP_DROP", drop_text);
    ini_set("coop_name", "From the file");
    ini_set("coop_server", "127.0.0.1:9");   // (the environment's is taken)
    CoopView v;
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "the process's session is off until coop_init (state %d)", v.state);
    CHECK(!coop_send(COOP_K_TEST, "x", 1) && !coop_command(COOP_CMD_CREATE, NULL), "nothing is sent and no command taken while off");
    static Player proc;   // (static: the process's session keeps its handlers until the process ends)
    memset(&proc, 0, sizeof proc);
    proc.name = "process";
    CHECK(coop_on_message(COOP_K_TEST, on_test_message, &proc) && coop_on_view(on_view_change, &proc), "handlers register");
    uint64_t id1 = coop_client_id();
    CHECK(id1 && id1 == coop_client_id(), "the client id stays the same within a start of the game");
    const int threads = count_entries("/proc/self/task"), fds = count_entries("/proc/self/fd");
    coop_init();
    coop_init();   // twice is the same as once
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 30000), "HOS_COOP_AUTO=join joins the room of HOS_COOP_ROOM (state %d, %s)", v.state, v.status);
    CHECK(v.self == 3 && v.n == 3 && !strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.maker == 1 && !v.made,
          "as member 3 of H7K3WM-BCDFGH-234567, made by member 1 (self %d, %d members, %s, maker %d)", v.self, v.n, v.code, v.maker);
    CHECK(!strcmp(v.name, "From the file"), "under the options file's coop_name (%s)", v.name);
    CHECK(log_count("coop: on, as From the file; the relay from HOS_COOP_SERVER") == 1, "the log says co-op is on, as whom, and where the relay came from");
    CHECK(log_count("coop: HOS_COOP_AUTO=join: room H7K\xe2\x80\xa6 at start") == 1, "and the room at start, by its first characters");
    CHECK(log_count("coop: joined room H7K\xe2\x80\xa6 as member 3") >= 1, "and the join");
    CHECK(!log_count(ROOM_SECRET) && !log_count("H7K3WM-BCDFGH") && !log_count(server), "the log has neither the room's secret nor the relay's address");
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
    CHECK(!coop_command(COOP_CMD_JOIN, "not a secret") && !coop_command(COOP_CMD_JOIN, NULL) && !coop_command(COOP_CMD_CREATE, "H7K3WM"),
          "a bad room secret is refused");
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
    CHECK(WAIT_FOR(count_entries("/proc/self/task") == threads && count_entries("/proc/self/fd") == fds, 5000),
          "off in a room ends the co-op thread and closes its socket (threads %d -> %d, files %d -> %d)", threads, count_entries("/proc/self/task"), fds,
          count_entries("/proc/self/fd"));
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
    CHECK(WAIT_FOR(count_entries("/proc/self/task") == threads && count_entries("/proc/self/fd") == fds, 5000),
          "the game closing ends the co-op thread and closes its socket (threads %d -> %d, files %d -> %d)", threads, count_entries("/proc/self/task"),
          fds, count_entries("/proc/self/fd"));
    CHECK(WAIT_FOR(in_room(alice, 2), drop ? ROSTER_WAIT_MS : 3000), "Alice sees the process go at once, not when the relay drops it");
    CHECK(drop || WAIT_FOR(alice->byes > byes, 1000), "Alice got the process's BYE");
    set_env("HOS_COOP_AUTO", NULL);
    set_env("HOS_COOP_ROOM", NULL);
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
// with no goodbye; the others see it away and then gone when scripts/coop_relay.py drops it for 15 s of silence
// (docs/coop.md, "A game that ends without a goodbye"), not before.
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
    if (spawned) FAIL_RET("the child did not start (%d)", spawned);
    const unsigned byes = alice->byes;
    const int away = log_count("(member 3) is away"), left = log_count("(member 3) left the room");
    // in the room as the child itself sees it (its own JOINED may have been lost while the relay already lists it: then
    // it is still connecting), and as Alice sees it
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
    // the child's last datagram was at most one keep-alive (2 s) before the signal, and the relay drops a member after
    // 15 s without one: so not within 12 s of the signal, however late this thread looks
    const int gone = WAIT_FOR(in_room(alice, 2), 16000 + ROSTER_WAIT_MS);
    const unsigned silent = (unsigned)(coop_now_ms() - t0);
    CHECK(gone && silent >= 12000, "Alice sees the killed child go when the relay drops it for its silence, not before (gone %d, %u ms after the signal)",
          gone, silent);
    CHECK(alice->byes == byes, "and got no BYE from it (%u)", alice->byes - byes);
    CHECK(log_count("(member 3) left the room") > left && (drop || log_count("(member 3) is away") > away),
          "the others' logs say the child was away, then that it left the room");
    printf("coop-session-test: a child ended by SIGTERM ended at once (%u ms) without a goodbye; the others saw it away, then gone %u ms later\n",
           took, silent);
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

// The hostile relay of this file (the mock): what it forwards changed, a message it seals itself, a recorded message of
// Bob's sent again after Bob joined again, garbage, forged rosters and envelopes. Alice and Bob are in a room.
static void check_hostile_relay(MockRelay *mock, Player *alice, Player *bob, uint16_t alice_port, unsigned drop, uint32_t *first) {
    uint8_t m[9] = {'p', 'r', 'o', 'b', 'e', 0};
    unsigned got_before = alice->got;
    mock->corrupt_next = 1;
    coop_u32_pack(m + 5, 4, 500);
    coop_sess_send(bob->sess, 0, COOP_K_TEST, m, sizeof m);
    coop_sleep_ms(300);
    CHECK(alice->got == got_before && !(alice->seen[500 / 32] & (1u << (500 % 32))), "a message changed in transit is not delivered");

    mock->forge = 1;
    CHECK(WAIT_FOR(!mock->forge, 5000), "the relay forged a message");
    coop_sleep_ms(300);
    CHECK(!(alice->seen[600 / 32] & (1u << (600 % 32))), "a message the relay sealed itself, in Bob's name and session, is not delivered");

    // a message of Bob's recorded, then Bob joins again (a new session), then the recorded one comes again
    mock->record_from = view_of(bob).self;
    coop_u32_pack(m + 5, 4, 700);
    for (int i = 0; i < 20 && mock->record_from; i++) coop_sess_send(bob->sess, 0, COOP_K_TEST, m, sizeof m), coop_sleep_ms(50);
    CHECK(WAIT_FOR(!mock->record_from && mock->recorded_n, 3000), "the relay recorded a message of Bob's");
    CHECK(drop || WAIT_FOR(alice->seen[700 / 32] & (1u << (700 % 32)), 3000), "and Alice got it once");
    unsigned dups = alice->dups;
    mock->replay = 5;   // the same session still: a replay within the window
    CHECK(WAIT_FOR(!mock->replay, 3000), "replayed");
    coop_sleep_ms(300);
    CHECK(alice->dups == dups, "a message sent again in the same session is not delivered twice");
    // Alice joins the room again (under a new name); Bob's session has not changed, so his recorded message is still one
    // she has seen
    char srv[32];
    snprintf(srv, sizeof srv, "127.0.0.1:%u", mock->port);
    CoopParams ap = {.tag = "Alice", .server = srv, .name = "Alice-again", .client_id = 0xA11CE, .on = 1, .drop_pct = drop, .retry = 1,
                     .local_port = alice_port};
    unsigned recon = alice->saw_reconnect;
    coop_sess_configure(alice->sess, &ap);
    CHECK(WAIT_FOR(alice->saw_reconnect > recon && in_room(alice, 2), ROSTER_WAIT_MS + 3000), "Alice joined the room again under a new name");
    coop_sleep_ms(500);
    mock->replay = 5;
    CHECK(WAIT_FOR(!mock->replay, 3000), "replayed after Alice joined again");
    coop_sleep_ms(300);
    CHECK(alice->dups == dups, "a message of Bob's present session is not delivered again after Alice joined the room again (%u twice)",
          alice->dups - dups);
    ap.name = "Alice";
    recon = alice->saw_reconnect;
    coop_sess_configure(alice->sess, &ap);
    CHECK(WAIT_FOR(alice->saw_reconnect > recon && in_room(alice, 2) && !strcmp(view_of(alice).member[0].name, "Alice"), ROSTER_WAIT_MS + 3000),
          "Alice is Alice again");
    coop_sess_command(bob->sess, COOP_CMD_JOIN, ROOM_SECRET);   // Bob joins again: a new session
    CHECK(WAIT_FOR(in_room(bob, 2) && in_room(alice, 2), ROSTER_WAIT_MS), "Bob is back");
    coop_sleep_ms(1500);   // the roster with Bob's new session has reached Alice
    uint32_t lost = view_of(alice).lost;
    mock->replay = 5;
    CHECK(WAIT_FOR(!mock->replay, 3000), "replayed after the rejoin");
    coop_sleep_ms(300);
    CHECK(alice->dups == dups && view_of(alice).lost > lost, "a message recorded before Bob joined again is refused after it (%u thrown away)",
          view_of(alice).lost - lost);

    // garbage and datagrams with the room's tag from the relay's address
    lost = view_of(alice).lost;
    mock->fuzz = 1;
    CHECK(WAIT_FOR(!mock->fuzz, 60000), "the garbage was sent");
    coop_sleep_ms(300);
    CHECK(view_of(alice).lost > lost, "the garbage was counted as thrown away (%u)", view_of(alice).lost - lost);
    CHECK(WAIT_FOR(in_room(alice, 2) && in_room(bob, 2), ROSTER_WAIT_MS), "after the garbage both are still in the room");
    exchange(alice, bob, drop, "after the hostile relay", first);

    // the relay marks Bob as the room's maker: Bob's game did not make the room, so it does not take it as made
    int bob_id = view_of(bob).self;
    mock->mark = bob_id;
    CHECK(WAIT_FOR(view_of(bob).maker == bob_id, ROSTER_WAIT_MS) && !view_of(bob).made, "a member the relay marks as the maker does not take the room as made by it (made %d)",
          view_of(bob).made);
    mock->mark = bob_id;
    CHECK(WAIT_FOR(view_of(bob).maker != bob_id, ROSTER_WAIT_MS), "and the mark is gone again");
    // someone on the path rejoined Alice with a session of its own: the relay lists that one; Alice notices and rejoins,
    // and Bob takes her messages again once a roster with her new session reaches him
    recon = alice->saw_reconnect;
    mock->spoof = view_of(alice).self;
    uint64_t spoofed_at = coop_now_ms();
    CHECK(WAIT_FOR(alice->saw_reconnect > recon && settled(mock, alice, bob), ROSTER_WAIT_MS),
          "Alice joined again, and the relay and both rosters list her new session");
    double settle_s = (double)(coop_now_ms() - spoofed_at) / 1000;
    CHECK(drop || settle_s * 1000 <= SETTLE_NO_LOSS_MS, "with no loss Alice's new session settled %.1f s after the spoofed rejoin, not within %.1f s",
          settle_s, SETTLE_NO_LOSS_MS / 1000.0);
    printf("coop-session-test: Alice's new session settled %.1f s after the spoofed rejoin\n", settle_s);
    exchange(alice, bob, drop, "after a spoofed rejoin of Alice", first);

    // an outsider's datagrams to Alice's port are not the relay's: thrown away
    CoopUdp *evil = coop_udp_open(0);
    CoopAddr to;
    coop_net_resolve("127.0.0.1", alice_port, &to);
    uint8_t junk[200];
    lost = view_of(alice).lost;
    for (int i = 0; evil && i < 50; i++) {
        for (size_t k = 0; k < sizeof junk; k++) junk[k] = (uint8_t)rnd32();
        coop_udp_send(evil, &to, junk, 10 + (size_t)(rnd32() % 190));
    }
    coop_udp_close(evil);
    CHECK(WAIT_FOR(view_of(alice).lost >= lost + 20, 5000), "datagrams from another address are thrown away");
    CHECK(in_room(alice, 2), "and change nothing");

    // three rosters within 300 ms list another session for Alice: she joins again once in that time, not three times
    // (each join is one of her network's)
    if (*first < 1000) *first = 1000;   // past the numbers of the messages above
    unsigned recon2 = alice->saw_reconnect;
    for (int i = 0; i < 3; i++) {
        mock->spoof = view_of(alice).self;
        WAIT_FOR(!mock->spoof, 1000);
        coop_sleep_ms(100);
    }
    unsigned rejoins = alice->saw_reconnect - recon2;
    CHECK(rejoins <= 1, "three rosters that list another session for Alice make her join again once (%u times)", rejoins);
    uint64_t spoofs_at = coop_now_ms();
    CHECK(WAIT_FOR(in_room(alice, 2) && in_room(bob, 2) && settled(mock, alice, bob), ROSTER_WAIT_MS),
          "and both are in the room after it, the relay and both rosters listing each one's present session");
    settle_s = (double)(coop_now_ms() - spoofs_at) / 1000;
    CHECK(drop || settle_s * 1000 <= SETTLE_NO_LOSS_MS, "with no loss the rosters settled %.1f s after the three that list another session for Alice, not within %.1f s",
          settle_s, SETTLE_NO_LOSS_MS / 1000.0);
    exchange(alice, bob, drop, "after the rosters listing another session for Alice", first);
    // someone on the path sends Alice a roster far ahead of the relay's counter that lists another session for Bob: it
    // is not taken, and Bob's messages keep reaching her
    mock->forge_roster = 1u << 30;
    CHECK(WAIT_FOR(!mock->forge_roster, 2000), "a roster 2^30 ahead was sent");
    coop_sleep_ms(300);
    exchange(alice, bob, drop, "after a forged roster 2^30 ahead", first);
    // one a little ahead is taken (the relay's counter could be there), and the relay's are taken again once none was
    // for the silence time and 4 s (1.5 s and 4 s here): the next of the relay's that arrives then. The mock sends one
    // a second, so the wait holds at least ten of them after the 5.5 s; all lost at 30 % loss is one run in 170000.
    mock->forge_roster = 1000;
    CHECK(WAIT_FOR(!mock->forge_roster, 2000), "a roster 1000 ahead was sent");
    uint64_t forged_at = coop_now_ms();
    int forged_taken = WAIT_FOR(!names_session(alice, bob), 1000);   // its three copies can all be lost at 30 % loss
    CHECK(drop || forged_taken, "a roster 1000 ahead is taken");
    CHECK(WAIT_FOR(names_session(alice, bob), 1500 + 4000 + 11000), "the relay's rosters are taken again after a forged one 1000 ahead");
    const uint64_t retaken_ms = coop_now_ms() - forged_at;
    CHECK(drop || !forged_taken || retaken_ms <= STALE_NO_LOSS_MS, "with no loss the relay's rosters were taken again %.1f s after a forged one "
          "1000 ahead, not within %.1f s (the silence time and 4 s, the mock's next roster, 1 s)", (double)retaken_ms / 1000, STALE_NO_LOSS_MS / 1000.0);
    char what[96];
    snprintf(what, sizeof what, "%.1f s after a forged roster 1000 ahead%s", (double)retaken_ms / 1000, forged_taken ? "" : ", all of whose copies were lost");
    exchange(alice, bob, drop, what, first);
    // one message in Bob's name and session, sealed with the room's key (which everyone who knows the secret has), its
    // counter 2^31 - 1 ahead of Bob's: Bob's own messages reach Alice again after a few
    mock->forge_ahead = 1;
    CHECK(WAIT_FOR(!mock->forge_ahead, 2000), "an envelope 2^31 - 1 ahead was sent");
    coop_sleep_ms(300);
    exchange(alice, bob, drop, "after one envelope in Bob's name 2^31 - 1 ahead, sealed with the room's key", first);

    // a session that keeps its room waits a minute when the relay refuses it for its network's joins, and stays; and it
    // asks for a room that is not there at most every 10 s
    Player erin, fay;
    mock->watch_cid = 0xE41, mock->watch_n = 0, mock->rate_cid = 0xE41;
    player_start(&erin, "Erin", 0xE41, srv, drop, 1, 0);
    coop_sess_command(erin.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(strstr(view_of(&erin).status, "minute") != NULL, 5000), "a refusal for the network's joins says when it is tried again: %s (state %d)",
          view_of(&erin).status, view_of(&erin).state);
    unsigned asked = mock->watch_n;
    coop_sleep_ms(3000);
    CoopView ev = view_of(&erin);
    CHECK(ev.state == COOP_CONNECTING && mock->watch_n - asked <= 3, "a session that keeps its room stays after it and does not ask again at once "
          "(state %d, %s; %u more CONNECTs)", ev.state, ev.status, mock->watch_n - asked);
    player_end(&erin);
    mock->rate_cid = 0, mock->watch_cid = 0xFA1, mock->watch_n = 0;
    player_start(&fay, "Fay", 0xFA1, srv, drop, 1, 0);
    coop_sess_command(fay.sess, COOP_CMD_JOIN, "BCDFGHBCDFGHBCDFGH");
    CHECK(WAIT_FOR(strstr(view_of(&fay).status, "not found") != NULL, 5000), "a room that is not there is waited for: %s", view_of(&fay).status);
    asked = mock->watch_n;
    coop_sleep_ms(4000);
    CHECK(view_of(&fay).state == COOP_CONNECTING && mock->watch_n - asked <= 3, "a session waiting for a room that is not there asks at most "
          "every 10 s (%u questions in 4 s)", mock->watch_n - asked);
    player_end(&fay);
    mock->watch_cid = 0;
    printf("coop-session-test: the hostile relay: nothing it changed, forged or sent again was delivered\n");
}

static void check_sessions(const char *relay, unsigned drop, const char *self, const char *work) {
    MockRelay *mock = NULL;
    char server[64];
    if (relay) {
        snprintf(server, sizeof server, "%s", relay);
    } else {
        mock = mock_start();
        if (!mock) FAIL_RET("the mock relay did not start");
        snprintf(server, sizeof server, "127.0.0.1:%u", mock->port);
    }
    printf("coop-session-test: sessions through %s, %u%% of received datagrams dropped\n", relay ? "scripts/coop_relay.py" : "the relay of this file", drop);

    Player alice, bob;
    uint16_t alice_port = free_port();
    player_start(&alice, "Alice", 0xA11CE, server, drop, 1, alice_port);
    player_start(&bob, "Bob", 0xB0B, server, drop, 1, 0);
    if (!alice.sess || !bob.sess) FAIL_RET("a session did not start");
    CoopView v = view_of(&alice);
    CHECK(v.state == COOP_LEFT, "a session that is on starts in no room (state %d)", v.state);

    // --- a room is made and joined
    coop_sess_command(alice.sess, COOP_CMD_CREATE, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&alice).state == COOP_JOINED, 30000), "Alice joined her room (state %d, %s)", view_of(&alice).state, view_of(&alice).status);
    v = view_of(&alice);
    CHECK(!strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.self == 1 && v.n == 1 && v.made && v.maker == 1 && !strcmp(v.member[0].name, "Alice"),
          "Alice is member 1 and the maker of room %s (self %d, %d members, made %d)", v.code, v.self, v.n, v.made);
    CHECK(log_count("coop[Alice]: room H7K\xe2\x80\xa6 created (member 1)") == 1, "the log says the room was made");
    coop_sess_command(bob.sess, COOP_CMD_JOIN, "h7k3wm-bcdfgh-234567");
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "both see two members (Alice %d, Bob %d)", view_of(&alice).n, view_of(&bob).n);
    v = view_of(&bob);
    CHECK(v.self == 2 && v.maker == 1 && !v.made && v.n == 2 && !strcmp(v.member[0].name, "Alice") && !strcmp(v.member[1].name, "Bob"),
          "Bob is member 2 and sees Alice as the maker (self %d, maker %d, %d members)", v.self, v.maker, v.n);
    CHECK(log_count("coop[Bob]: joined room H7K\xe2\x80\xa6 as member 2") == 1 && WAIT_FOR(log_count("coop[Alice]: Bob (member 2) joined the room"), 5000),
          "the log says Bob joined, on both sides");
    CHECK(WAIT_FOR(view_of(&bob).ping_ms > 0, 5000), "the round trip to the relay is measured (the keep-alive answered)");
    Player other;
    player_start(&other, "Other", 0x07E, server, 0, 0, 0);
    coop_sess_command(other.sess, COOP_CMD_CREATE, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&other).state == COOP_LEFT && strstr(view_of(&other).status, "already open"), 30000),
          "a create of a room that is open is refused, not joined: %s", view_of(&other).status);
    player_end(&other);

    uint32_t first = 0;
    exchange(&alice, &bob, drop, "in the room", &first);
    CHECK(WAIT_FOR(view_of(&alice).member[1].heard_ms != UINT32_MAX, 3000), "a member whose messages opened counts as heard");

    if (mock) check_hostile_relay(mock, &alice, &bob, alice_port, drop, &first);

    // --- the relay is lost for good and comes back. (Counted from here: rejoins of the hostile part, or of a session
    // that heard nothing for 1.5 s on a loaded machine, came before and say nothing about this restart.)
    const unsigned recon_a = alice.saw_reconnect, recon_b = bob.saw_reconnect;
    const int lost_before = log_count("coop[Alice]: relay lost; rejoining"), made_before = log_count("coop[Alice]: creating room H7K\xe2\x80\xa6 again");
    if (mock) {
        mock->restart = 1;
        printf("coop-session-test: the mock relay restarts\n");
    } else {
        printf("coop-session-test: restart the relay now\n");
        fflush(stdout);
        char line[64] = "";
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "restarted", 9)) CHECK(0, "the relay was not restarted (got \"%s\")", line);
    }
    CHECK(WAIT_FOR(alice.saw_reconnect > recon_a && bob.saw_reconnect > recon_b, 15000), "both sessions noticed that the relay was gone (%u, %u)",
          alice.saw_reconnect - recon_a, bob.saw_reconnect - recon_b);
    CHECK(WAIT_FOR(log_count("coop[Alice]: relay lost; rejoining") > lost_before && log_count("coop[Alice]: creating room H7K\xe2\x80\xa6 again") > made_before,
                   60000),
          "the log says the relay was lost and the room made again");
    CHECK(WAIT_FOR(in_room(&alice, 2) && in_room(&bob, 2), 60000), "after a relay restart both are in a room again (Alice state %d %s, Bob state %d %s)",
          view_of(&alice).state, view_of(&alice).status, view_of(&bob).state, view_of(&bob).status);
    v = view_of(&bob);
    CHECK(!strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.maker == view_of(&alice).self, "the maker made the room again under its secret (%s, maker %d)",
          v.code, v.maker);
    exchange(&alice, &bob, drop, "after the restart", &first);

    // --- Bob leaves, and comes back
    unsigned byes = alice.byes;
    coop_sess_command(bob.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&bob).state == COOP_LEFT, 5000), "Bob left (state %d)", view_of(&bob).state);
    CHECK(WAIT_FOR(in_room(&alice, 1), drop ? ROSTER_WAIT_MS : 3000), "Alice sees the room with only her in it (%d members)", view_of(&alice).n);
    CHECK(drop || WAIT_FOR(alice.byes > byes, 1000), "Alice got Bob's BYE");
    coop_sess_command(bob.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "Bob joined again");

    // --- the process's own session, with the settings the game reads
    check_process(server, drop, &alice, &first);
#ifndef _WIN32
    if (!mock) check_killed(self, work, server, drop, &alice);
#else
    (void)self, (void)work;
#endif

    // --- a player who connects while the relay lies to it (the mock), and is then removed by the maker
    Player carol, carol2, dave;
    player_start(&carol, "Carol", 0xCA201, server, 0, 0, 0);
    if (mock) mock->lie = 1;
    coop_sess_command(carol.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&carol, 3), ROSTER_WAIT_MS), "Carol joins%s (state %d, %s)", mock ? " though the relay first answered with lies" : "",
          view_of(&carol).state, view_of(&carol).status);
    CHECK(WAIT_FOR(in_room(&alice, 3), ROSTER_WAIT_MS), "Alice sees Carol");
    int carol_id = view_of(&carol).self;
    CHECK(!coop_sess_remove(bob.sess, carol_id) || WAIT_FOR(in_room(&carol, 3), 2000), "Bob, who did not make the room, cannot remove Carol");
    coop_sleep_ms(500);
    CHECK(in_room(&carol, 3), "Carol is still in the room");
    CHECK(coop_sess_remove(alice.sess, carol_id), "the maker's removal is taken");
    CHECK(WAIT_FOR(view_of(&carol).state == COOP_LEFT && strstr(view_of(&carol).status, "removed"), ROSTER_WAIT_MS),
          "Carol is removed by the maker: %s", view_of(&carol).status);
    CHECK(WAIT_FOR(in_room(&alice, 2), ROSTER_WAIT_MS), "and gone from Alice's roster");
    CHECK(log_count("coop[Alice]: removing Carol (member ") == 1 && log_count("coop[Carol]: left room (the room's maker removed this player)") == 1,
          "the log says who removed whom");
    coop_sess_command(carol.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&carol).state == COOP_LEFT && strstr(view_of(&carol).status, "removed"), 30000) && in_room(&alice, 2),
          "and cannot come back as the same player: %s", view_of(&carol).status);
    player_end(&carol);

    // --- a room that is not there, a relay that is not there
    player_start(&carol2, "Carol", 0xCA202, server, drop, 0, 0);
    coop_sess_command(carol2.sess, COOP_CMD_JOIN, "BCDFGHBCDFGHBCDFGH");
    CHECK(WAIT_FOR(strstr(view_of(&carol2).status, "no such room") != NULL, 30000), "Carol, joining a room that does not exist, is told so: %s",
          view_of(&carol2).status);
    CHECK(view_of(&carol2).state == COOP_LEFT, "and ends in no room (state %d)", view_of(&carol2).state);
    player_end(&carol2);
    uint16_t dead_port = free_port();
    char dead[40];
    snprintf(dead, sizeof dead, "127.0.0.1:%u", dead_port);
    player_start(&dave, "Dave", 0xDA7E, dead, 0, 0, 0);
    CHECK(coop_sess_command(dave.sess, COOP_CMD_CREATE, ""), "a create without a secret makes a new one");
    coop_sleep_ms(2500);
    v = view_of(&dave);
    char secret[COOP_SECRET_LEN + 1];
    CHECK(v.state == COOP_CONNECTING && strstr(v.status, "no answer") && coop_secret_normalize(v.code, secret) && strcmp(secret, ROOM_SECRET),
          "Dave, with nobody at the relay's address, keeps trying with a new secret: state %d, %s", v.state, v.status);
    coop_sess_command(dave.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT, 5000), "and leaves when told to");
    coop_sess_configure(dave.sess, &(CoopParams){.server = "", .name = "Dave", .on = 1});
    coop_sess_command(dave.sess, COOP_CMD_CREATE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT && strstr(view_of(&dave).status, "no server"), 5000), "with no relay given a create says so: %s",
          view_of(&dave).status);
    player_end(&dave);

    // --- the end: LEAVE and BYE, state off
    player_end(&bob);
    CHECK(WAIT_FOR(in_room(&alice, 1), ROSTER_WAIT_MS), "Alice sees Bob go when his session ends");
    if (mock) {   // the relay restarts and someone else makes the room first: its maker joins it instead of leaving
        mock->steal = 1;
        mock->restart = 1;
        CHECK(WAIT_FOR(view_of(&alice).state == COOP_JOINED && !view_of(&alice).made && view_of(&alice).n == 2, 30000),
              "after a restart in which another made the room first, its maker joins it (state %d, made %d, %d members, %s)",
              view_of(&alice).state, view_of(&alice).made, view_of(&alice).n, view_of(&alice).status);
    }
    player_end(&alice);
    if (mock) mock_stop(mock);
}

static void no_work(void *arg) { (void)arg; }

int main(int argc, char **argv) {
    coop_lock_init(&log_lock);
    log_echo = getenv("COOP_TEST_VERBOSE") != NULL;
    setvbuf(stdout, NULL, _IOLBF, 0);
    // The first thread starts the runtime's helpers (TSan's among them). thread_join also waits for this
    // warm-up thread's own kernel task to disappear, so it cannot inflate the later baseline count.
    thread_join(thread_new(no_work, NULL));
    const char *relay = NULL, *work = NULL;
    unsigned drop = 0;
    int unit = 0, unit_menu = 0, mock = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unit")) unit = 1;
        else if (!strcmp(argv[i], "--unit-menu")) unit_menu = 1;
        else if (!strcmp(argv[i], "--mock")) mock = 1;
        else if (!strcmp(argv[i], "--relay") && i + 1 < argc) relay = argv[++i];
        else if (!strcmp(argv[i], "--drop") && i + 1 < argc) drop = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) work = argv[++i];
#ifndef _WIN32
        else if (!strcmp(argv[i], "--signal-child")) return signal_child();
#endif
        else {
            fprintf(stderr, "usage: %s --unit | --unit-menu | --mock [--drop N] | --relay HOST:PORT [--drop N] [--work DIR]\n", argv[0]);
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
    } else if (mock || relay) {
        check_sessions(relay, drop, argv[0], work);
    } else {
        fprintf(stderr, "usage: %s --unit | --unit-menu | --mock [--drop N] | --relay HOST:PORT [--drop N] [--work DIR]\n", argv[0]);
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
