// Co-op session layer (coop.h): the state machine of one player's connection to a relay, and the thread that runs it.
//
// States: OFF (co-op is off: HOS_COOP or the options file's coop= did not switch it on), LEFT (on, in no room),
// CONNECTING (reaching the relay to create or join a room), JOINED, RECONNECTING (the relay went silent; joining the
// same room again as the same member).
//
// A room is its secret (coop_proto.h): the relay knows it by the locator derived from it, and the players seal their
// messages with the message key derived from it, so the relay forwards what it cannot read, change or make.
//
// Reaching the relay is a cookie exchange (scripts/coop_relay.py): a CONNECT is answered by a CHALLENGE with a cookie, a
// CONNECT that carries the cookie is answered by JOINED or an ERROR. Every answer carries the nonce of the attempt and
// the locator's tag, so a datagram someone else sends from the relay's address does not end or redirect the attempt.
// A burst of four CONNECTs 250 ms apart carries the exchange over a lossy path; a burst without an answer is followed by
// a wait of 1, 2, 4, 8, then 15 s. A session that keeps its room (retry) asks for a room that is not there at most every
// 10 s, and waits a minute when the relay refuses it for its network's joins, instead of ending. In a room a PING every
// 2 s keeps the NAT mapping open and measures the round trip, and 8 s without any datagram from the relay is
// RECONNECTING. JOINED hands out a rejoin token, which a later CONNECT of the same member carries: the relay gives a
// member's id back only to it.
//
// Every join is a new session: a random number the CONNECT tells the relay, which lists it in the roster. A player's
// messages carry its session and a counter from 1 in the sealed envelope; the others take a message only of the session
// the roster names for its sender and once within a window of 64 counters, so a message recorded before a rejoin, or
// sent again within the window, is refused. Four older ones in a row start the window again (window_below), after which
// a recorded message of that session can come again: a layer that must take a message once checks a counter of its own.
//
// One thread owns the socket, the DNS lookup and the state. Other threads reach it through a mailbox (commands, settings), an
// outbox (messages to send) and a copy of the state they may read; all three sit behind one lock that the thread holds only
// to copy. The thread is a plain host thread, as host/discord_rpc.c's: it never touches guest memory. The process's
// session has it only while co-op is switched on: switching co-op off, or the game closing, ends it (asked, then waited
// for at most STOP_WAIT_MS), and switching co-op on starts it again.
#include "coop_net.h"   // (first: winsock2.h before windows.h)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "../gpu/vk/compat/hookshot_env.h"
#endif

void hos_log(const char *fmt, ...);

enum {
    PING_MS = 2000, SILENCE_MS = 8000, BURST_GAP_MS = 250, BACKOFF_FIRST_MS = 1000, BACKOFF_MAX_MS = 15000,
    BURST_TRIES = 4,                 // CONNECTs per burst
    NO_ROOM_WAIT_MS = 10000,         // a room that is not there is asked for this often at most (each asks the relay for a join)
    RATE_WAIT_MS = 60000,            // the wait after the relay refused a join for the network's budget
    ROSTER_AHEAD = 1 << 20,          // a roster's number is taken at most this far ahead of the last one's
    ROSTER_STALE_MS = 4000,          // after the silence time and this long without a roster taken, any roster is taken
    STATS_MS = 60000,                // the periodic counters line, while in a room
    LOG_EVERY_MS = 30000,            // a repeated cause logs this often
    RENAME_MS = 1000,                // a name change joins the room again after this much quiet
    OUT_SLOTS = 64,                  // messages waiting to be sent
    DEFAULT_PORT = 24680,
    VIEW_HOOKS = 8,
    TICK_HOOKS = 4,
};

typedef struct { uint8_t dst, kind; uint16_t len; uint8_t data[COOP_MAX_MESSAGE]; } OutMsg;
typedef struct { CoopMsgFn fn; void *user; } MsgHandler;
typedef struct { CoopViewFn fn; void *user; } ViewHook;
typedef struct { CoopTickFn fn; void *user; } TickHook;

// A cause that logs at most once per LOG_EVERY_MS.
enum { L_MALFORMED, L_SENDER, L_TAG, L_REPLAY, L_OUTBOX, L_NO_SERVER, L_RESOLVE, L_SOCKET, L_CAUSES };

// The counters of one sender's session seen last: a counter at most 64 below the highest is taken once, an older one
// is not, unless the sender's own messages keep arriving below the window (window_below). A new session (the roster
// names it) starts a new window. low and low_n: the last counter that opened below it, and how many did in a row.
typedef struct { uint64_t session; uint32_t high, low; uint64_t bits; int valid, low_n; } Window;
enum { WINDOW_AGAIN = 4 };

struct CoopSess {
    // ---- settings, set by coop_sess_new and coop_sess_configure
    char tag[16];
    uint32_t ping_ms, silence_ms, burst_gap_ms, backoff_first_ms, backoff_max_ms;
    // what the stepping thread works with now
    char server[160], name[COOP_NAME_MAX + 1], host[128];
    uint16_t port, local_port;
    uint64_t client_id;
    unsigned drop_pct;
    int retry;

    // ---- shared, behind lock
    CoopLock lock;
    CoopEvent off_done;
    CoopEvent wake;                             // a command or settings were queued: a session with no socket to wait on takes them now
    int cmd;                                    // a command waiting (COOP_CMD_*), 0 none; a later one replaces it
    char cmd_secret[COOP_SECRET_LEN + 1];
    int cmd_remove;                             // a member the maker removes, 0 none, and its session in the view then
    uint64_t cmd_remove_session;
    int cfg_pending;                            // pending holds settings the thread has not applied
    struct { char server[160], name[COOP_NAME_MAX + 1], why[48]; uint64_t client_id; uint16_t local_port; unsigned drop_pct; int retry, on; } pending;
    OutMsg *out;
    unsigned out_head, out_tail;
    CoopView view;
    uint64_t heard_at[COOP_MAX_MEMBERS + 1];    // last message of each member that opened, by id; 0 never
    MsgHandler msg[256];
    ViewHook hooks[VIEW_HOOKS];
    int n_hooks;
    TickHook ticks[TICK_HOOKS];
    int n_ticks;
    int pub_state;                              // state, readable without the lock
    int pub_joined;

    // ---- the stepping thread's
    CoopState state;
    CoopUdp *sock;
    CoopAddr relay;
    int relay_known;
    char relay_text[64];                        // the relay's resolved address, for the trace
    int intent, made_room;                      // CONNECT's intent; this session's own CREATE made the room
    uint64_t room_session;                      // the session the room lists for this player since its last JOINED
    char want[COOP_SECRET_LEN + 1];             // the secret of the room being joined or made
    uint8_t locator[COOP_KEY_LEN];              // of want: the relay's name of the room and the tags' key
    uint8_t msg_key[COOP_MSG_KEY_LEN];          // of want: seals the players' messages
    uint64_t session, nonce;                    // this join's session (in the roster); this attempt's nonce
    uint8_t cookie[COOP_COOKIE_LEN], token[COOP_COOKIE_LEN];
    int have_cookie, have_token, said_answer, heard_relay;
    uint32_t connect_seq, out_seq, out_counter;
    uint64_t next_tx, last_connect_tx, last_rx, next_ping, next_stats, rename_at;
    uint64_t roster_at, roster_rejoin_at;       // when a roster was last taken; the last rejoin a roster asked for
    int own_taken;                              // the roster taken last lists another session for this player
    uint32_t backoff;
    int burst_left;
    uint32_t room;
    int self;
    char secret[COOP_SECRET_LEN + 1];           // of the room we are in
    CoopRoster roster;
    uint32_t roster_seq;
    Window rw[COOP_MAX_MEMBERS + 1];
    char status[96];
    int dirty;
    int off_applied;                            // a stop was applied: off_done is set once the view saying so is published
    uint64_t rng;
    uint64_t log_at[L_CAUSES];
    uint32_t n_tx, n_rx, n_lost;                // (read by other threads for the view: written with bump)
    uint32_t n_malformed, n_bad_tag, n_sender, n_dup, n_test_drop, n_outbox_full;
    uint32_t stats_seen;
    uint8_t rx[2048];
};

// ---- logging

// HOS_COOP_TRACE=1: a line per datagram, and the relay's address (never logged otherwise)
static int trace_on(void) {
    static volatile int on = -1;   // (read once; two threads reading it at once both find the same answer)
    if (on < 0) on = env_on("HOS_COOP_TRACE");
    return on;
}
#define TRACE(s, ...) do { if (trace_on()) say(s, __VA_ARGS__); } while (0)
static void say(const CoopSess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void say(const CoopSess *s, const char *fmt, ...) {
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (s->tag[0]) hos_log("coop[%s]: %s", s->tag, msg);
    else hos_log("coop: %s", msg);
}

// 1 when cause may log now: the first time, then once per LOG_EVERY_MS (the periodic counters line has the totals).
static int log_ready(CoopSess *s, int cause, uint64_t now) {
    if (s->log_at[cause] && now - s->log_at[cause] < LOG_EVERY_MS) return 0;
    s->log_at[cause] = now ? now : 1;
    return 1;
}
#define SAY_LIMITED(s, cause, now, ...) do { if (log_ready(s, cause, now)) say(s, __VA_ARGS__); } while (0)

static const char *type_name(unsigned t) {
    switch (t) {
    case COOP_T_CONNECT: return "CONNECT";
    case COOP_T_CHALLENGE: return "CHALLENGE";
    case COOP_T_JOINED: return "JOINED";
    case COOP_T_ERROR: return "ERROR";
    case COOP_T_ROSTER: return "ROSTER";
    case COOP_T_PING: return "PING";
    case COOP_T_PONG: return "PONG";
    case COOP_T_LEAVE: return "LEAVE";
    case COOP_T_KICK: return "KICK";
    case COOP_T_DATA: return "DATA";
    default: return "?";
    }
}

// ---- small things

// One more on a counter the view shows: coop_sess_view reads those three from other threads without waiting for the
// stepping thread, their only writer.
static void bump(uint32_t *n) { __atomic_store_n(n, *n + 1, __ATOMIC_RELAXED); }

static uint32_t rnd(CoopSess *s) {   // xorshift64*: for the test drop and nothing else
    s->rng ^= s->rng >> 12, s->rng ^= s->rng << 25, s->rng ^= s->rng >> 27;
    return (uint32_t)((s->rng * 0x2545F4914F6CDD1Dull) >> 32);
}

// 64 random bits from the system's source (sessions, nonces), never 0; 0 when the source failed: there is no fallback,
// since a session or nonce someone could guess would let them answer for the relay or take this player's place.
static uint64_t random64(void) {
    uint64_t v = 0;
    if (!coop_random(&v, sizeof v)) return 0;
    return v ? v : 1;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
}

// "XXX…" of a secret, for the log: the whole secret never goes there.
static const char *label_of(const char *secret, char out[8]) {
    if (secret[0]) coop_secret_short(secret, out);
    else copy_str(out, 8, "");
    return out;
}

static void set_status(CoopSess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_status(CoopSess *s, const char *fmt, ...) {
    char text[sizeof s->status];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (strcmp(text, s->status)) memcpy(s->status, text, sizeof text), s->dirty = 1;
}

static void set_state(CoopSess *s, CoopState st) {
    if (s->state != st) s->dirty = 1;
    s->state = st;
    __atomic_store_n(&s->pub_state, (int)st, __ATOMIC_RELEASE);
    __atomic_store_n(&s->pub_joined, st == COOP_JOINED, __ATOMIC_RELEASE);
}

int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port) {
    while (*text == ' ') text++;
    size_t n = strlen(text);
    while (n && text[n - 1] == ' ') n--;
    if (!n) return 0;
    const char *h = text, *colon = NULL;
    size_t hn = n;
    if (*text == '[') {   // [v6] or [v6]:port
        const char *close = memchr(text, ']', n);
        if (!close) return 0;
        h = text + 1, hn = (size_t)(close - h);
        if (close + 1 < text + n) {
            if (close[1] != ':') return 0;
            colon = close + 1;
        }
    } else {
        size_t colons = 0;
        for (size_t i = 0; i < n; i++)
            if (text[i] == ':') colons++, colon = text + i;
        if (colons > 1) colon = NULL;   // a bare IPv6 address
        else if (colon) hn = (size_t)(colon - text);
    }
    uint32_t p = DEFAULT_PORT;
    if (colon) {
        const char *d = colon + 1;
        size_t dn = (size_t)(text + n - d);
        if (!dn || dn > 5) return 0;
        p = 0;
        for (size_t i = 0; i < dn; i++) {
            if (d[i] < '0' || d[i] > '9') return 0;
            p = p * 10 + (uint32_t)(d[i] - '0');
        }
        if (!p || p > 65535) return 0;
    }
    if (!hn || hn >= cap) return 0;
    memcpy(host, h, hn);
    host[hn] = 0;
    *port = (uint16_t)p;
    return 1;
}

// ---- sending

// One datagram to the relay, tagged with the locator.
static void send_dgram(CoopSess *s, uint8_t type, uint32_t room, uint8_t src, uint8_t dst, uint32_t seq,
                       const void *payload, size_t n) {
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .src = src, .dst = dst, .room = room, .seq = seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, s->locator);
    if (!k || !s->sock || !s->relay_known) return;
    coop_udp_send(s->sock, &s->relay, d, k);
    bump(&s->n_tx);
    TRACE(s, "tx %s to %u, %zu bytes", type_name(type), dst, k);
}

// A datagram of the room we are in.
static void send_room(CoopSess *s, uint8_t type, uint8_t dst, const void *payload, size_t n) {
    send_dgram(s, type, s->room, (uint8_t)s->self, dst, s->out_seq++, payload, n);
}

static void go_left(CoopSess *s, const char *why, int log_it);

// A player message, sealed with the room's message key under this session and the next counter.
static void send_sealed(CoopSess *s, uint8_t dst, uint8_t kind, const void *bytes, size_t n) {
    if (s->out_counter == UINT32_MAX) {   // the nonce includes this counter: a new join must supply a new session
        go_left(s, "encrypted message counter exhausted; join the room again", 1);
        return;
    }
    uint8_t payload[COOP_MAX_PAYLOAD];
    size_t k = coop_seal(payload, sizeof payload, s->msg_key, (uint8_t)s->self, dst, s->session, ++s->out_counter, kind, bytes, n);
    if (k) send_room(s, COOP_T_DATA, dst, payload, k);
}

static void send_connect(CoopSess *s, uint64_t now) {
    CoopConnect c = {.intent = (uint8_t)s->intent, .client_id = s->client_id, .session = s->session, .nonce = s->nonce};
    uint8_t payload[128];
    memcpy(c.locator, s->locator, COOP_KEY_LEN);
    copy_str(c.name, sizeof c.name, s->name);
    if (s->have_cookie) memcpy(c.cookie, s->cookie, COOP_COOKIE_LEN);
    if (s->have_token) memcpy(c.token, s->token, COOP_COOKIE_LEN);
    size_t n = coop_connect_pack(payload, sizeof payload, &c);
    send_dgram(s, COOP_T_CONNECT, 0, 0, 0, s->connect_seq, payload, n);
    if (!s->have_cookie) s->last_connect_tx = now;
}

// ---- leaving

static void close_socket(CoopSess *s) {
    if (s->sock) coop_udp_close(s->sock);
    s->sock = NULL;
}

// Tells the others and the relay, twice (a datagram may be lost): BYE to every member, then LEAVE.
static void say_goodbye(CoopSess *s) {
    if (!s->sock || !s->relay_known || !s->room) return;
    for (int i = 0; i < 2; i++) {
        if (s->state == COOP_JOINED) send_sealed(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
        send_room(s, COOP_T_LEAVE, 0, NULL, 0);
    }
}

// Out of the room (or never in): closes the socket and shows why.
static void go_left(CoopSess *s, const char *why, int log_it) {
    int was_in = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    if (log_it) {
        if (was_in) say(s, "left room (%s)", why);
        else say(s, "no room: %s", why);
    }
    close_socket(s);
    s->room = 0, s->self = 0, s->made_room = 0;
    s->roster.n = 0;
    s->have_token = 0;
    memset(s->rw, 0, sizeof s->rw);
    set_state(s, COOP_LEFT);
    set_status(s, "%s", why);
    s->dirty = 1;
}

// ---- reaching the relay

// A socket and the relay's address, when there are none yet. 0 and a status when they cannot be had.
static int ensure_socket(CoopSess *s, uint64_t now) {
    if (!s->sock) {
        s->sock = coop_udp_open(s->local_port);
        if (!s->sock) {
            set_status(s, "cannot open a UDP socket");
            SAY_LIMITED(s, L_SOCKET, now, "cannot open a UDP socket");
            return 0;
        }
    }
    if (!s->relay_known) {
        say(s, "connecting to the relay");
        TRACE(s, "the relay is %s", s->server);
        if (!coop_net_resolve(s->host, s->port, &s->relay)) {
            set_status(s, "cannot find the relay's address");
            SAY_LIMITED(s, L_RESOLVE, now, "cannot resolve the relay's name");
            return 0;
        }
        coop_addr_str(&s->relay, s->relay_text, sizeof s->relay_text);
        s->relay_known = 1;
    }
    return 1;
}

static void schedule_backoff(CoopSess *s, uint64_t now) {
    s->next_tx = now + s->backoff;
    if (!s->heard_relay && s->sock && s->relay_known) set_status(s, "no answer from the relay; trying again in %u s", (s->backoff + 500) / 1000);
    s->backoff = s->backoff * 2 > s->backoff_max_ms ? s->backoff_max_ms : s->backoff * 2;
}

// A new attempt to get into a room: intent, the room's secret, and whether it is a rejoin of the room we were in (which
// keeps the rejoin token). Every attempt is a new session and a new nonce; 0 (and no room) when the system's random
// source cannot give them.
static int begin_episode(CoopSess *s, int intent, const char *secret, int rejoin, uint64_t now) {
    uint64_t session = random64(), nonce = random64();
    if (!session || !nonce) {
        go_left(s, "the system's random source failed: co-op needs it", 1);
        return 0;
    }
    s->intent = intent;
    if (secret != s->want) copy_str(s->want, sizeof s->want, secret);   // a rejoin passes the secret it already holds
    coop_room_keys(s->want, s->locator, s->msg_key);
    s->session = session, s->nonce = nonce, s->out_counter = 0;
    s->connect_seq = s->out_seq++;
    s->have_cookie = s->said_answer = s->heard_relay = 0;
    if (!rejoin) s->have_token = 0;
    s->burst_left = 0;
    s->next_tx = now;
    s->backoff = s->backoff_first_ms;
    s->relay_known = 0;   // looked up again: the name may point elsewhere by now
    if (!rejoin) {
        s->room = 0, s->self = 0, s->made_room = 0;
        s->roster.n = 0;
    }
    set_state(s, rejoin ? COOP_RECONNECTING : COOP_CONNECTING);
    if (rejoin) set_status(s, "reconnecting to the relay");
    else set_status(s, "%s", intent == COOP_INTENT_CREATE ? "creating a room" : "joining the room");
    s->dirty = 1;
    return 1;
}

static void connect_timers(CoopSess *s, uint64_t now) {
    if (now < s->next_tx) return;
    if (s->burst_left == 0) {   // a new burst: the socket and the relay's address first
        if (!ensure_socket(s, now)) {
            s->relay_known = 0;
            schedule_backoff(s, now);
            return;
        }
        s->burst_left = BURST_TRIES;
    }
    send_connect(s, now);
    if (--s->burst_left) s->next_tx = now + s->burst_gap_ms;
    else schedule_backoff(s, now);
}

// ---- the roster

// 1 when every id of r is 1..8 and none repeats: what the rest of the code indexes by.
static int roster_valid(const CoopRoster *r) {
    unsigned seen = 0;
    for (int i = 0; i < r->n; i++) {
        unsigned id = r->m[i].id;
        if (id < 1 || id > COOP_MAX_MEMBERS || (seen & 1u << id)) return 0;
        seen |= 1u << id;
    }
    return 1;
}

static const CoopMemberInfo *member_of(const CoopSess *s, int id) {
    for (int i = 0; i < s->roster.n; i++)
        if (s->roster.m[i].id == id) return &s->roster.m[i];
    return NULL;
}

// The roster the relay sent replaces ours; the log tells who came, went, changed name or went away. 1 when this
// player's own line lists another session than its own: someone took its place at the relay (a CONNECT with its
// token, seen on the way), and the session joins again to take it back (roster_rejoin).
static int apply_roster(CoopSess *s, const CoopRoster *r) {
    int taken = 0;
    for (int i = 0; i < r->n; i++) {
        const CoopMemberInfo *m = &r->m[i], *old = member_of(s, m->id);
        if (m->id == s->self) {   // the relay's maker mark is not taken for this session's own creation (on_joined)
            taken = m->session != s->session;
            continue;
        }
        if (!old) say(s, "%s (member %u) joined the room", m->name, m->id);
        else if (strcmp(old->name, m->name)) say(s, "member %u is now %s", m->id, m->name);
        else if ((old->flags ^ m->flags) & COOP_FLAG_AWAY)
            say(s, "%s (member %u) %s", m->name, m->id, m->flags & COOP_FLAG_AWAY ? "is away" : "is back");
        // a new member, or a new session of it: nothing heard from it yet. Its replay window stays: a window belongs to
        // one session (window_check takes another session as new), so the same session, listed again after this player
        // joined again, keeps refusing what it already delivered
        if (!old || old->session != m->session) {
            coop_lock(&s->lock);
            s->heard_at[m->id] = 0;
            coop_unlock(&s->lock);
        }
    }
    for (int j = 0; j < s->roster.n; j++) {
        int id = s->roster.m[j].id, found = 0;
        for (int i = 0; i < r->n; i++) found |= r->m[i].id == id;
        if (found) continue;
        say(s, "%s (member %u) left the room", s->roster.m[j].name, id);
        coop_lock(&s->lock);
        s->heard_at[id] = 0;
        coop_unlock(&s->lock);
    }
    s->roster = *r;
    s->dirty = 1;
    return taken;
}

// ---- receiving

// 1 when counter of session is new in the window, 0 when it was taken before, -1 when it is too old to tell (64 or more
// below the highest); nothing changes (commit after the message opened).
static int window_check(const Window *w, uint64_t session, uint32_t counter) {
    if (!w->valid || w->session != session) return 1;
    int32_t d = (int32_t)(counter - w->high);
    if (d > 0) return 1;
    if (d <= -64) return -1;
    return !(w->bits >> (unsigned)-d & 1);
}

static void window_commit(Window *w, uint64_t session, uint32_t counter) {
    if (!w->valid || w->session != session) {
        *w = (Window){.session = session, .high = counter, .bits = 1, .valid = 1};
        return;
    }
    int32_t d = (int32_t)(counter - w->high);
    if (d > 0) {
        w->bits = d >= 64 ? 1 : w->bits << d | 1;
        w->high = counter;
        w->low_n = 0;
    } else {
        w->bits |= 1ull << (unsigned)-d;
    }
}

// A message that opened with the room's message key but is too old for the window. That key is the whole room's, so
// whoever holds it can seal one message in a member's name with a counter far ahead and leave every later message of
// that member below the window for the rest of its session. The sender's own next messages then keep arriving below
// it, each later than the one before: at the WINDOW_AGAIN-th in a row the window starts again from it, which takes this
// one. One that is not later than the last starts the row again (a copy of one counts once, and one sealed above the
// sender's own cannot hold the row there). The window forgets what it took: recorded messages of the session above the
// new start, and its last 64, can then be taken again (coop.h says what a handler does about it). 1 when the window
// started again.
static int window_below(Window *w, uint32_t counter) {
    if (w->low_n && (int32_t)(counter - w->low) <= 0) w->low_n = 0;
    w->low = counter;
    if (++w->low_n < WINDOW_AGAIN) return 0;
    *w = (Window){.session = w->session, .high = counter, .bits = 1, .valid = 1};
    return 1;
}

// The answers to a CONNECT: the attempt's nonce and the locator's tag.
static int answer_ok(CoopSess *s, const uint8_t *d, size_t n, uint64_t nonce) {
    if (nonce != s->nonce || !coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    return 1;
}

// The on_* handlers return 1 for a datagram that passed its checks and made sense: only those show the relay is there.
static int on_challenge(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint64_t nonce;
    uint8_t cookie[COOP_COOKIE_LEN];
    if ((s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) || !coop_challenge_parse(p, h->len, &nonce, cookie)) return 0;
    if (!answer_ok(s, d, n, nonce)) return 0;
    memcpy(s->cookie, cookie, COOP_COOKIE_LEN);
    s->have_cookie = 1;
    s->heard_relay = 1;
    if (!s->said_answer) {
        s->said_answer = 1;
        say(s, "the relay answered in %u ms", (unsigned)(now - s->last_connect_tx));
        TRACE(s, "the relay is at %s", s->relay_text);
    }
    send_connect(s, now);   // the CONNECT with the cookie, at once
    return 1;
}

static int on_joined(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if (s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) return 0;
    CoopJoined j;
    if (!coop_joined_parse(p, h->len, &j) || !h->room || j.member < 1 || j.member > COOP_MAX_MEMBERS || !roster_valid(&j.roster)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    if (!answer_ok(s, d, n, j.nonce)) return 0;   // the room asked for (our locator), this attempt (our nonce)
    int rejoined = s->state == COOP_RECONNECTING;
    s->room = h->room;
    s->self = j.member;
    memcpy(s->secret, s->want, sizeof s->secret);
    memcpy(s->token, j.token, COOP_COOKIE_LEN);
    s->have_token = 1;
    if (!rejoined) s->made_room = s->intent == COOP_INTENT_CREATE;   // this session's own creation, not the relay's mark
    s->room_session = s->session;
    apply_roster(s, &j.roster);   // after a rejoin: against the roster before it, so a member whose session is the same stays as it was
    s->roster_seq = h->seq;
    s->roster_at = now, s->own_taken = 0;
    s->last_rx = now;
    s->next_ping = now + s->ping_ms;
    s->next_stats = now + STATS_MS;
    char label[8];
    label_of(s->secret, label);
    if (rejoined) say(s, "rejoined room %s as member %u", label, s->self);
    else if (s->intent == COOP_INTENT_CREATE) say(s, "room %s created (member %u)", label, s->self);
    else say(s, "joined room %s as member %u", label, s->self);
    set_state(s, COOP_JOINED);
    set_status(s, "%s", "");
    s->dirty = 1;
    return 1;
}

static void refused(CoopSess *s, uint8_t reason, uint64_t now) {
    char label[8];
    label_of(s->want, label);
    switch (reason) {
    case COOP_E_NO_ROOM:
        if (s->state == COOP_RECONNECTING) {   // the relay lost the room: a restart
            say(s, "room %s is gone from the relay", label);
            if (!s->retry) go_left(s, "the room is closed", 0);
            else if (s->made_room) {
                say(s, "creating room %s again", label);
                if (begin_episode(s, COOP_INTENT_CREATE, s->want, 0, now)) set_status(s, "the room was lost; creating it again");
            } else if (begin_episode(s, COOP_INTENT_JOIN, s->want, 0, now)) {
                set_status(s, "the room was lost; waiting for it");
            }
        } else if (s->retry) {   // not made yet, perhaps: wait and ask again
            s->heard_relay = 1;
            set_status(s, "room %s not found; waiting for it", label);
            if (s->burst_left || now >= s->next_tx) {   // the first answer of a burst ends it; a CONNECT sent twice is answered twice
                s->burst_left = 0;
                schedule_backoff(s, now);
                // each question asks the relay for one of the network's joins: at most one every NO_ROOM_WAIT_MS, so
                // that players behind one address who wait for a room leave joins for the others (the same attempt
                // asked again costs nothing at the relay)
                if (s->next_tx < now + NO_ROOM_WAIT_MS) s->next_tx = now + NO_ROOM_WAIT_MS;
            }
        } else {
            go_left(s, "no such room", 1);
        }
        break;
    case COOP_E_ROOM_FULL: go_left(s, "the room is full", 1); break;
    case COOP_E_PROTO: go_left(s, "update required: the relay speaks another protocol version", 1); break;
    case COOP_E_RATE:
        if (s->retry) {   // a session that keeps its room waits until the network's joins are back, and stays
            if (s->next_tx < now + RATE_WAIT_MS) {
                say(s, "the relay refused: too many joins from this network; trying again in a minute");
                s->next_tx = now + RATE_WAIT_MS;
            }
            s->heard_relay = 1, s->burst_left = 0;
            set_status(s, "too many joins from this network; trying again in a minute");
        } else {
            go_left(s, "too many joins from this network; try again in a minute", 1);
        }
        break;
    case COOP_E_ROOM_EXISTS:
        if (s->retry && s->intent == COOP_INTENT_CREATE) {
            // a session that keeps its room (coop_auto): someone made the room again first after the relay lost it, or
            // it is open already. Joining it keeps the player in the room they meant; the rules stay the ones the
            // players accepted, since a game takes them only signed by the maker it pinned (coop_state.h)
            say(s, "room %s is open already; joining it", label);
            if (begin_episode(s, COOP_INTENT_JOIN, s->want, 0, now)) set_status(s, "the room is open already; joining it");
        } else {
            go_left(s, "a room with this secret is already open on the relay: join it", 1);
        }
        break;
    case COOP_E_REMOVED: go_left(s, "the room's maker removed this player", 1); break;
    case COOP_E_SESSION_USED:
        if (s->retry) {   // someone sent this attempt's CONNECT first (seen on the way): a new attempt, a new session
            say(s, "the relay saw this session in the room before (a replay on the way?); trying again with a new one");
            int rejoin = s->state == COOP_RECONNECTING;
            if (begin_episode(s, s->intent, s->want, rejoin, now)) s->next_tx = now + NO_ROOM_WAIT_MS;
        } else {
            go_left(s, "the relay saw this session in the room before (a replay on the way?)", 1);
        }
        break;
    default: go_left(s, "the relay refused", 1); break;
    }
}

static int on_error(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    int connecting = s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING;
    CoopError e;
    if (!coop_error_parse(p, h->len, &e)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    // an answer to this attempt; in a room, only a removal (nonce 0, the room's tag). A removal in the room never
    // arrives (it carries nonce 0, never this attempt's), and must not be taken by accepting nonce 0: anyone who saw a
    // CONNECT can tag a datagram with its locator. The maker's removal shows as the member leaving the roster instead.
    if (connecting ? !answer_ok(s, d, n, e.nonce) : s->state != COOP_JOINED || e.reason != COOP_E_REMOVED || h->room != s->room ||
                                                        !answer_ok(s, d, n, 0))
        return 0;
    refused(s, e.reason, now);
    return 1;
}

// This player's own line lists another session: join again to take the place back, at most once per backoff_max_ms,
// so that rosters from someone on the path cannot use up the network's joins; a later one waits for its turn (timers).
static void roster_rejoin(CoopSess *s, uint64_t now) {
    if (s->roster_rejoin_at && now - s->roster_rejoin_at < s->backoff_max_ms) return;
    s->roster_rejoin_at = now ? now : 1;
    say(s, "the relay lists another session for this player; joining the room again");
    begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
}

static int on_roster(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    CoopRoster r;
    if (s->state != COOP_JOINED || h->room != s->room) return 0;
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    if (!coop_roster_parse(p, h->len, &r, NULL) || !roster_valid(&r)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    // a later roster, up to ROSTER_AHEAD past the last one: anyone who saw a CONNECT can tag one, and a number far
    // ahead would make every roster of the relay after it look old. An older one arriving late is dropped, unless no
    // roster was taken for a while (the relay sends one every few seconds): then the last one's number was not the
    // relay's
    int32_t ahead = (int32_t)(h->seq - s->roster_seq);
    int stale = now - s->roster_at >= (uint64_t)s->silence_ms + ROSTER_STALE_MS;
    if ((ahead > 0 && ahead <= ROSTER_AHEAD) || (stale && ahead)) {
        s->roster_seq = h->seq, s->roster_at = now;
        s->own_taken = apply_roster(s, &r);
        if (s->own_taken) roster_rejoin(s, now);
    }
    return 1;
}

static int on_pong(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint32_t token;
    if (s->state != COOP_JOINED || h->room != s->room || !coop_u32_parse(p, h->len, &token)) return 0;
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    uint32_t rtt = (uint32_t)now - token;
    if (rtt <= 60000) {   // a token of ours
        coop_lock(&s->lock);
        s->view.ping_ms = rtt ? rtt : 1;
        coop_unlock(&s->lock);
    }
    return 1;
}

static int on_data(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    (void)d, (void)n;
    if (s->state != COOP_JOINED || h->room != s->room) return 0;
    const CoopMemberInfo *m = h->src != s->self ? member_of(s, h->src) : NULL;
    uint64_t session;
    uint32_t counter;
    if (!m || (h->dst != COOP_DST_ALL && h->dst != s->self)) {
        s->n_sender++, bump(&s->n_lost);
        SAY_LIMITED(s, L_SENDER, now, "a message from member %u to %u was thrown away: not a member the roster names", h->src, h->dst);
        return 0;
    }
    if (!coop_envelope_peek(p, h->len, &session, &counter)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    int fresh = session == m->session ? window_check(&s->rw[h->src], session, counter) : 0;
    if (!fresh) {   // an earlier session's, or seen
        s->n_dup++, bump(&s->n_lost);
        SAY_LIMITED(s, L_REPLAY, now, "a message of member %u that is not of its present session, or seen before, was thrown away", h->src);
        return 0;
    }
    uint8_t kind, bytes[COOP_MAX_PAYLOAD];
    size_t nb;
    if (!coop_open(p, h->len, s->msg_key, h->src, h->dst, &kind, bytes, sizeof bytes, &nb)) {
        s->n_bad_tag++, bump(&s->n_lost);
        SAY_LIMITED(s, L_TAG, now, "a message that does not open with the room's key was thrown away");
        return 0;
    }
    if (fresh < 0 && !window_below(&s->rw[h->src], counter)) {
        s->n_dup++, bump(&s->n_lost);
        SAY_LIMITED(s, L_REPLAY, now, "a message of member %u older than its last 64 was thrown away", h->src);
        return 0;
    }
    if (fresh > 0) window_commit(&s->rw[h->src], session, counter);
    coop_lock(&s->lock);
    s->heard_at[h->src] = now;
    coop_unlock(&s->lock);
    TRACE(s, "rx message kind %u from member %u, %zu bytes", kind, h->src, nb);
    CoopMsgFn fn = __atomic_load_n(&s->msg[kind].fn, __ATOMIC_ACQUIRE);
    if (fn) fn(h->src, bytes, nb, s->msg[kind].user);
    return 1;
}

static void handle_datagram(CoopSess *s, size_t n, const CoopAddr *from, uint64_t now) {
    if (s->drop_pct && rnd(s) % 100 < s->drop_pct) {
        s->n_test_drop++;
        return;
    }
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(s->rx, n, &h, &p)) {
        s->n_malformed++, bump(&s->n_lost);
        SAY_LIMITED(s, L_MALFORMED, now, "a datagram that is not a co-op datagram was thrown away");
        return;
    }
    if (!s->relay_known || !coop_addr_eq(from, &s->relay)) {   // only the relay talks to us
        s->n_sender++, bump(&s->n_lost);
        SAY_LIMITED(s, L_SENDER, now, "a datagram from an address that is not the relay was thrown away");
        return;
    }
    if (h.ver != COOP_PROTO) {
        // a relay of another version answers a CONNECT with an untagged ERROR it cannot word in our format: taken only
        // as the first answer of an attempt, so a stray one does not end a session
        if (h.type == COOP_T_ERROR && coop_dgram_tag_zero(s->rx) && !s->heard_relay &&
            (s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING)) {
            bump(&s->n_rx);
            refused(s, COOP_E_PROTO, now);
        }
        return;
    }
    TRACE(s, "rx %s, %zu bytes", type_name(h.type), n);
    int ok = 0;
    switch (h.type) {
    case COOP_T_CHALLENGE: ok = on_challenge(s, s->rx, n, &h, p, now); break;
    case COOP_T_JOINED: ok = on_joined(s, s->rx, n, &h, p, now); break;
    case COOP_T_ERROR: ok = on_error(s, s->rx, n, &h, p, now); break;
    case COOP_T_ROSTER: ok = on_roster(s, s->rx, n, &h, p, now); break;
    case COOP_T_PONG: ok = on_pong(s, s->rx, n, &h, p, now); break;
    case COOP_T_DATA: ok = on_data(s, s->rx, n, &h, p, now); break;
    default: break;
    }
    if (ok) {
        bump(&s->n_rx);
        s->last_rx = now;   // a datagram the relay made: it is there
    }
}

// ---- the outbox

static void flush_outbox(CoopSess *s) {
    for (int i = 0; i < OUT_SLOTS; i++) {
        OutMsg m;
        coop_lock(&s->lock);
        if (s->out_head == s->out_tail) {
            coop_unlock(&s->lock);
            return;
        }
        OutMsg *slot = &s->out[s->out_tail];
        m.dst = slot->dst, m.kind = slot->kind, m.len = slot->len;
        memcpy(m.data, slot->data, slot->len);
        s->out_tail = (s->out_tail + 1) % OUT_SLOTS;
        coop_unlock(&s->lock);
        if (s->state == COOP_JOINED) send_sealed(s, m.dst, m.kind, m.data, m.len);   // anywhere else it is dropped
    }
}

int coop_sess_send(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n) {
    if (!__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE) || n > COOP_MAX_MESSAGE || member < 0 ||
        (member > COOP_MAX_MEMBERS && member != COOP_DST_ALL))
        return 0;
    coop_lock(&s->lock);
    unsigned next = (s->out_head + 1) % OUT_SLOTS;
    if (next == s->out_tail) {
        coop_unlock(&s->lock);
        __atomic_add_fetch(&s->n_outbox_full, 1, __ATOMIC_RELAXED);
        return 0;
    }
    OutMsg *slot = &s->out[s->out_head];
    slot->dst = member ? (uint8_t)member : COOP_DST_ALL;
    slot->kind = kind;
    slot->len = (uint16_t)n;
    if (n) memcpy(slot->data, bytes, n);
    s->out_head = next;
    coop_unlock(&s->lock);
    return 1;
}

// ---- commands and settings

// The settings coop_sess_configure queued, on the stepping thread.
static void apply_settings(CoopSess *s, uint64_t now) {
    coop_lock(&s->lock);
    if (!s->cfg_pending) {
        coop_unlock(&s->lock);
        return;
    }
    s->cfg_pending = 0;
    char server[sizeof s->server], name[sizeof s->name], why[sizeof s->pending.why];
    copy_str(server, sizeof server, s->pending.server);
    copy_str(name, sizeof name, s->pending.name);
    copy_str(why, sizeof why, s->pending.why);
    s->client_id = s->pending.client_id;
    s->local_port = s->pending.local_port;
    s->drop_pct = s->pending.drop_pct;
    s->retry = s->pending.retry;
    int on = s->pending.on;
    coop_unlock(&s->lock);
    if (strcmp(server, s->server)) {
        copy_str(s->server, sizeof s->server, server);
        s->relay_known = 0;
        s->host[0] = 0;
        if (s->server[0] && !coop_server_parse(s->server, s->host, sizeof s->host, &s->port))
            say(s, "the coop_server setting is not host or host:port");
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            go_left(s, "the server changed", 1);
        } else if (s->state == COOP_CONNECTING) {
            go_left(s, "the server changed", 0);
        }
    }
    if (strcmp(name, s->name)) {
        copy_str(s->name, sizeof s->name, name);
        s->dirty = 1;   // (the view shows the name in use, in a room or not)
        if (s->state == COOP_JOINED) s->rename_at = now + RENAME_MS;   // joins again once the typing has stopped
    }
    if (on && s->state == COOP_OFF) {
        set_state(s, COOP_LEFT);
        set_status(s, "%s", "");
    } else if (!on) {
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            say(s, "left room (%s)", why[0] ? why : "co-op turned off");
        }
        close_socket(s);
        s->room = 0, s->self = 0, s->made_room = 0, s->roster.n = 0;
        s->rename_at = 0;
        set_state(s, COOP_OFF);
        set_status(s, "%s", "");
        s->dirty = 1;
        s->off_applied = 1;
    }
}

static void do_command(CoopSess *s, int cmd, const char *secret, uint64_t now) {
    if (s->state == COOP_OFF) return;
    switch (cmd) {
    case COOP_CMD_LEAVE:
        if (s->state == COOP_LEFT) break;
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            say(s, "left room (leave requested)");
        } else {
            say(s, "stopped trying to get into a room");
        }
        go_left(s, "you left the room", 0);
        break;
    case COOP_CMD_CREATE:
    case COOP_CMD_JOIN:
        if (!s->host[0]) {
            SAY_LIMITED(s, L_NO_SERVER, now, "no relay given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)");
            go_left(s, "no server configured", 0);
            break;
        }
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) say_goodbye(s);
        begin_episode(s, cmd == COOP_CMD_CREATE ? COOP_INTENT_CREATE : COOP_INTENT_JOIN, secret, 0, now);
        break;
    }
}

// The maker's KICK of a member, with the member's session so that a KICK sent again later cannot remove whoever has the
// id then: the session the published view listed when the maker removed it (progress sync removed that member's key at
// that moment), so that a member that rejoined since, or another player given the id, is not the one kicked.
static void do_remove(CoopSess *s, int member, uint64_t session) {
    const CoopMemberInfo *m = member_of(s, member);
    uint8_t payload[16];
    if (s->state != COOP_JOINED || !s->made_room || !m || member == s->self) {
        say(s, "only the room's maker can remove a member, and only one that is in the room");
        return;
    }
    size_t k = coop_kick_pack(payload, sizeof payload, (uint8_t)member, session ? session : m->session);
    for (int i = 0; i < 2; i++) send_room(s, COOP_T_KICK, 0, payload, k);
    say(s, "removing %s (member %d) from the room", m->name, member);
}

// ---- the step

static void lost_relay(CoopSess *s, uint64_t now) {
    say(s, "relay lost; rejoining");
    begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
}

static void timers(CoopSess *s, uint64_t now) {
    switch (s->state) {
    case COOP_CONNECTING:
    case COOP_RECONNECTING:
        connect_timers(s, now);
        break;
    case COOP_JOINED:
        if (now - s->last_rx >= s->silence_ms) {
            lost_relay(s, now);
            break;
        }
        if (s->rename_at && now >= s->rename_at) {   // the name changed: the relay learns it with a new CONNECT
            s->rename_at = 0;
            say(s, "joining the room again as %s", s->name);
            begin_episode(s, COOP_INTENT_JOIN, s->want, 1, now);
            break;
        }
        if (s->own_taken && now - s->roster_rejoin_at >= s->backoff_max_ms) {   // a roster's rejoin that waited for its turn
            roster_rejoin(s, now);
            break;
        }
        if (now >= s->next_ping) {
            uint8_t token[4];
            coop_u32_pack(token, sizeof token, (uint32_t)now);
            send_room(s, COOP_T_PING, 0, token, sizeof token);
            s->next_ping = now + s->ping_ms;
        }
        if (now >= s->next_stats) {
            s->next_stats = now + STATS_MS;
            uint32_t seen = s->n_rx + s->n_tx + s->n_lost + s->n_test_drop;
            if (seen != s->stats_seen) {
                s->stats_seen = seen;
                coop_lock(&s->lock);
                const uint32_t ping = s->view.ping_ms;
                coop_unlock(&s->lock);
                say(s, "stats: sent %u, accepted %u, thrown away %u (tag %u, sender %u, not co-op %u, replay %u), dropped by the test %u, outbox full %u, round trip %u ms",
                    s->n_tx, s->n_rx, s->n_lost, s->n_bad_tag, s->n_sender, s->n_malformed, s->n_dup, s->n_test_drop,
                    __atomic_load_n(&s->n_outbox_full, __ATOMIC_RELAXED), ping);
            }
        }
        break;
    default:
        break;
    }
}

static void view_rebuild(CoopSess *s) {
    coop_lock(&s->lock);
    CoopView *v = &s->view;
    int in_room = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    const char *secret = in_room ? s->secret : s->state == COOP_CONNECTING ? s->want : "";
    v->state = s->state;
    if (secret[0]) coop_secret_format(secret, v->code);
    else v->code[0] = 0;
    label_of(secret, v->label);
    v->self = in_room ? s->self : 0;
    v->made = in_room && s->made_room;
    v->session = in_room ? s->room_session : 0;
    copy_str(v->name, sizeof v->name, s->name);
    v->maker = 0;
    v->n = in_room ? s->roster.n : 0;
    for (int i = 0; i < v->n; i++) {
        v->member[i].id = s->roster.m[i].id;
        v->member[i].away = s->roster.m[i].flags & COOP_FLAG_AWAY ? 1 : 0;
        v->member[i].maker = s->roster.m[i].flags & COOP_FLAG_MAKER ? 1 : 0;
        if (v->member[i].maker) v->maker = v->member[i].id;
        v->member[i].session = s->roster.m[i].session;
        copy_str(v->member[i].name, sizeof v->member[i].name, s->roster.m[i].name);
    }
    copy_str(v->status, sizeof v->status, s->status);
    if (s->state != COOP_JOINED) v->ping_ms = 0;
    v->version++;
    coop_unlock(&s->lock);
}

static void publish(CoopSess *s) {
    if (!s->dirty) return;
    s->dirty = 0;
    view_rebuild(s);
    CoopView copy;
    ViewHook hooks[VIEW_HOOKS];
    coop_lock(&s->lock);
    copy = s->view;
    int n = s->n_hooks;
    memcpy(hooks, s->hooks, sizeof hooks);
    coop_unlock(&s->lock);
    for (int i = 0; i < n; i++) hooks[i].fn(&copy, hooks[i].user);
    // A stop is signalled only now: whoever it wakes (coop_shutdown's caller, a page that switched co-op off) reads the
    // view that says the session is off, and every view hook has been told of it.
    if (s->off_applied) {
        s->off_applied = 0;
        coop_event_set(&s->off_done);
    }
}

void coop_sess_step(CoopSess *s) {
    uint64_t now = coop_now_ms();
    coop_lock(&s->lock);
    int cmd = s->cmd, remove = s->cmd_remove;
    uint64_t remove_session = s->cmd_remove_session;
    char secret[sizeof s->cmd_secret];
    memcpy(secret, s->cmd_secret, sizeof secret);
    s->cmd = 0, s->cmd_remove = 0;
    coop_unlock(&s->lock);
    apply_settings(s, now);
    const int stopped = s->off_applied;   // (a stop carried out in this step: no wait after it, its thread may end now)
    if (cmd) do_command(s, cmd, secret, now);
    if (remove) do_remove(s, remove, remove_session);
    flush_outbox(s);
    publish(s);   // what a command or a setting changed shows before this thread waits for a datagram
    uint32_t wait = stopped ? 0 : s->state == COOP_JOINED ? 8 : s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING ? 50 : 250;
    if (s->sock) {
        CoopAddr from;
        int n = coop_udp_recv(s->sock, s->rx, sizeof s->rx, &from, wait);
        for (int i = 0; n > 0 && i < 64; i++) {
            handle_datagram(s, (size_t)n, &from, coop_now_ms());
            if (!s->sock) break;   // a handler ended it
            n = coop_udp_recv(s->sock, s->rx, sizeof s->rx, &from, 0);
        }
        if (n < 0 && s->sock) {
            SAY_LIMITED(s, L_SOCKET, now, "the socket failed");
            close_socket(s);
            if (s->state == COOP_JOINED) lost_relay(s, coop_now_ms());
        }
    } else if (wait) {
        coop_event_wait_ns(&s->wake, (uint64_t)wait * 1000000u);   // (a command, new settings or a stop end the wait)
    }
    timers(s, coop_now_ms());
    publish(s);
    TickHook ticks[TICK_HOOKS];
    coop_lock(&s->lock);
    int nt = s->n_ticks;
    memcpy(ticks, s->ticks, sizeof ticks);
    coop_unlock(&s->lock);
    for (int i = 0; i < nt; i++) ticks[i].fn(coop_now_ms(), ticks[i].user);
}

// ---- the session's interface

CoopSess *coop_sess_new(const CoopParams *p) {
    CoopSess *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->out = calloc(OUT_SLOTS, sizeof *s->out);
    if (!s->out) {
        free(s);
        return NULL;
    }
    copy_str(s->tag, sizeof s->tag, p->tag);
    s->ping_ms = p->ping_ms ? p->ping_ms : PING_MS;
    s->silence_ms = p->silence_ms ? p->silence_ms : SILENCE_MS;
    s->burst_gap_ms = p->burst_gap_ms ? p->burst_gap_ms : BURST_GAP_MS;
    s->backoff_first_ms = p->backoff_first_ms ? p->backoff_first_ms : BACKOFF_FIRST_MS;
    s->backoff_max_ms = p->backoff_max_ms ? p->backoff_max_ms : BACKOFF_MAX_MS;
    coop_lock_init(&s->lock);
    coop_event_init(&s->off_done, 1, 0);
    coop_event_init(&s->wake, 0, 0);
    uint64_t seed = 0;
    if (!coop_random(&seed, sizeof seed) || !seed) seed = coop_now_ns() | 1;
    s->rng = seed;
    s->state = COOP_OFF;
    s->view.state = COOP_OFF;
    coop_sess_configure(s, p);
    apply_settings(s, coop_now_ms());   // nothing else runs yet: the first step need not wait for it
    publish(s);
    return s;
}

void coop_sess_free(CoopSess *s) {
    if (!s) return;
    close_socket(s);
    free(s->out);
    free(s);
}

void coop_sess_configure(CoopSess *s, const CoopParams *p) {
    coop_lock(&s->lock);
    copy_str(s->pending.server, sizeof s->pending.server, p->server);
    char clean[COOP_NAME_MAX + 1];
    coop_name_clean(clean, sizeof clean, (const uint8_t *)(p->name ? p->name : ""), p->name ? strlen(p->name) : 0);
    copy_str(s->pending.name, sizeof s->pending.name, clean);
    s->pending.client_id = p->client_id;
    s->pending.local_port = p->local_port;
    s->pending.drop_pct = p->drop_pct > 100 ? 100 : p->drop_pct;
    s->pending.retry = p->retry;
    s->pending.on = p->on;
    s->pending.why[0] = 0;
    if (!p->on) coop_event_reset(&s->off_done);
    s->cfg_pending = 1;
    coop_unlock(&s->lock);
    coop_event_set(&s->wake);
}

// A stop for why (the room's "left room (why)" line), as coop_sess_stop.
static void sess_stop_because(CoopSess *s, const char *why) {
    coop_lock(&s->lock);
    s->pending.on = 0;
    copy_str(s->pending.why, sizeof s->pending.why, why);
    coop_event_reset(&s->off_done);
    s->cfg_pending = 1;
    coop_unlock(&s->lock);
    coop_event_set(&s->wake);
}

void coop_sess_stop(CoopSess *s) { sess_stop_because(s, "co-op turned off"); }

int coop_sess_command(CoopSess *s, CoopCmd cmd, const char *secret) {
    char canon[COOP_SECRET_LEN + 1] = "";
    if (cmd != COOP_CMD_CREATE && cmd != COOP_CMD_JOIN && cmd != COOP_CMD_LEAVE) return 0;
    if (cmd == COOP_CMD_JOIN || (cmd == COOP_CMD_CREATE && secret && *secret)) {
        if (!secret || !coop_secret_normalize(secret, canon)) return 0;
    } else if (cmd == COOP_CMD_CREATE) {   // a new room: a new secret, which only this player knows until it passes it on
        uint8_t r[32];
        if (!coop_random(r, sizeof r) || !coop_secret_from_random(r, canon)) return 0;
    }
    coop_lock(&s->lock);
    s->cmd = (int)cmd;
    copy_str(s->cmd_secret, sizeof s->cmd_secret, canon);
    coop_unlock(&s->lock);
    coop_event_set(&s->wake);
    return 1;
}

int coop_sess_remove(CoopSess *s, int member) {
    if (member < 1 || member > COOP_MAX_MEMBERS || !__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE)) return 0;
    coop_lock(&s->lock);
    s->cmd_remove = member, s->cmd_remove_session = 0;
    for (int i = 0; i < s->view.n; i++)
        if (s->view.member[i].id == member) s->cmd_remove_session = s->view.member[i].session;
    coop_unlock(&s->lock);
    coop_event_set(&s->wake);
    return 1;
}

int coop_sess_wait_off(CoopSess *s, uint32_t ms) { return coop_event_wait_ns(&s->off_done, (uint64_t)ms * 1000000u); }

int coop_sess_on_message(CoopSess *s, uint8_t kind, CoopMsgFn fn, void *user) {
    s->msg[kind].user = user;
    __atomic_store_n(&s->msg[kind].fn, fn, __ATOMIC_RELEASE);
    return 1;
}

int coop_sess_on_view(CoopSess *s, CoopViewFn fn, void *user) {
    coop_lock(&s->lock);
    int ok = s->n_hooks < VIEW_HOOKS;
    if (ok) s->hooks[s->n_hooks++] = (ViewHook){fn, user};
    coop_unlock(&s->lock);
    return ok;
}

int coop_sess_on_tick(CoopSess *s, CoopTickFn fn, void *user) {
    coop_lock(&s->lock);
    int ok = s->n_ticks < TICK_HOOKS;
    if (ok) s->ticks[s->n_ticks++] = (TickHook){fn, user};
    coop_unlock(&s->lock);
    return ok;
}

void coop_sess_view(CoopSess *s, CoopView *out) {
    uint64_t now = coop_now_ms();
    coop_lock(&s->lock);
    *out = s->view;
    for (int i = 0; i < out->n; i++) {
        uint64_t at = s->heard_at[out->member[i].id];
        out->member[i].heard_ms = out->member[i].id == out->self ? 0 : at ? (uint32_t)(now - at) : UINT32_MAX;
    }
    out->tx = __atomic_load_n(&s->n_tx, __ATOMIC_RELAXED);   // counted by the stepping thread without the lock
    out->rx = __atomic_load_n(&s->n_rx, __ATOMIC_RELAXED);
    out->lost = __atomic_load_n(&s->n_lost, __ATOMIC_RELAXED);
    coop_unlock(&s->lock);
}

// ---- the process's session

static CoopSess *g_sess;
static CoopOnce g_once = COOP_ONCE_INIT;
static int g_on;        // co-op is switched on (coop_init, coop_configure): commands are taken

static void make_session(void *arg) {
    (void)arg;
    CoopParams p = {0};
    g_sess = coop_sess_new(&p);
    if (!g_sess) hos_log("coop: out of memory");
}

static CoopSess *process_session(void) {
    coop_once(&g_once, make_session, NULL);
    return g_sess;
}

// The player's id towards the relay: random for each start of the game, so that nothing ties one session to another,
// and never sent to the other players. The default name's digits are random too, not taken from it. 0 when the system's
// random source failed: an id others could guess would let them take this player's place, so co-op stays off.
static uint64_t g_client_id;
static unsigned g_name_digits;
static CoopOnce g_client_once = COOP_ONCE_INIT;
static void make_client_id(void *arg) {
    (void)arg;
    uint64_t r[2] = {0, 0};
    if (!coop_random(r, sizeof r)) return;
    g_client_id = r[0] ? r[0] : 1;
    g_name_digits = (unsigned)(r[1] & 0xFFFF);
}

uint64_t coop_client_id(void) {
    coop_once(&g_client_once, make_client_id, NULL);
    return g_client_id;
}

// The settings of the process's session, read once at coop_init. hookshot_options.ini beside the game: coop=1 switches
// co-op on, coop_name= is the player's name (empty: "Player-" and four random hex digits), coop_server= the relay
// ("host", "host:port" or "[v6]:port"; there is no default: the players run one), coop_port= the local UDP port (0:
// any). The environment wins over each: HOS_COOP, HOS_COOP_NAME, HOS_COOP_SERVER, HOS_COOP_PORT. Off, no thread is
// started and nothing else runs. For tests: HOS_COOP_AUTO=create|join with HOS_COOP_ROOM, a room at start (and LAN
// parties; a room the relay lost is made or waited for again), and HOS_COOP_DROP, a test's share of the received
// datagrams thrown away (percent). The relay's address reaches the log only with HOS_COOP_TRACE. No HOS_COOP name is
// taken from hookshot_env.txt, a file players pass around (host/hos_main.c env_line_allowed): the environment itself only.
int hos_ini_option(const char *key, int otherwise);          // host/nn_applet.c: hookshot_options.ini
int hos_ini_text(const char *key, char *out, size_t size);

static struct {
    char name[COOP_NAME_MAX + 1], server[160], room[COOP_SECRET_LEN + 1];
    int on, automatic, join;
    uint16_t port;
    unsigned drop;
} g_set;                        // what the process's session is to be: coop_init's settings, coop_configure's changes
static CoopLock g_set_lock;     // g_set, g_stop and g_ended; taken before the session's own lock, never after it

// The coop thread runs while co-op is switched on. Switching it off (coop_configure) or the game closing (coop_shutdown)
// queues the session's stop, asks the thread to end (g_stop) and waits for it, at most STOP_WAIT_MS. The thread ends
// once it has carried the stop out: goodbye said, the socket closed, the view that says "off" published. Held up for
// longer (a name lookup), it ends by itself when it gets there and is joined at the next switch, or goes on if co-op is
// switched on again first, which takes the request back. g_life makes coop_init, coop_configure and coop_shutdown take
// turns; the coop thread never takes it.
enum { STOP_WAIT_MS = 400 };
static CoopLock g_life;         // taken before g_set_lock
static CoopThread g_thread;
static int g_ready;             // g_set was read: coop_init, or the first coop_configure that switched co-op on (g_life)
static int g_live;              // g_thread was started and is not joined yet (g_life; read without it atomically)
static int g_stop;              // the thread is asked to end (g_set_lock; read without it atomically)
static int g_ended;             // the thread saw g_stop and is returning (g_set_lock)
static int g_auto_done;         // HOS_COOP_AUTO's room was asked for: at the first start only (g_life)
static CoopOnce g_set_once = COOP_ONCE_INIT;
static void make_locks(void *arg) {
    (void)arg;
    coop_lock_init(&g_set_lock);
    coop_lock_init(&g_life);
}

// The environment's value, else the options file's key's, else "", into out; where it came from (NULL: nowhere).
static const char *setting(const char *env, const char *key, char *out, size_t cap) {
    const char *v = getenv(env);
    if (v) {
        copy_str(out, cap, v);
        return env;
    }
    if (hos_ini_text(key, out, cap)) return "hookshot_options.ini";
    out[0] = 0;
    return NULL;
}

// The settings into g_set, and the lines that say what they are (never the relay's address, nor the room's secret).
// menu: the name and relay the menu gives (coop_configure), over the environment's and the options file's; NULL: those.
static void read_settings(const char *const menu[2]) {
    char text[160];
    if (menu) copy_str(text, sizeof text, menu[0]);
    else setting("HOS_COOP_NAME", "coop_name", text, sizeof text);
    coop_name_clean(g_set.name, sizeof g_set.name, (const uint8_t *)text, strlen(text));
    if (!g_set.name[0]) snprintf(g_set.name, sizeof g_set.name, "Player-%04x", g_name_digits);
    const char *from = "the menu";
    if (menu) copy_str(g_set.server, sizeof g_set.server, menu[1]);
    else from = setting("HOS_COOP_SERVER", "coop_server", g_set.server, sizeof g_set.server);
    if (!g_set.server[0]) from = NULL;
    const char *port = getenv("HOS_COOP_PORT");
    const long p = port ? strtol(port, NULL, 10) : hos_ini_option("coop_port", 0);
    g_set.port = p > 0 && p <= 65535 ? (uint16_t)p : 0;
    if (p < 0 || p > 65535) hos_log("coop: the local port (HOS_COOP_PORT or coop_port=) is not 0 to 65535: any port is used");
    const char *drop = getenv("HOS_COOP_DROP");
    g_set.drop = drop ? (unsigned)strtoul(drop, NULL, 10) : 0;
    hos_log("coop: on, as %s; %s%s", g_set.name, from ? "the relay from " : "no relay given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)",
            from ? from : "");
    if (g_set.drop) hos_log("coop: HOS_COOP_DROP: %u%% of the received datagrams are thrown away (a test)", g_set.drop > 100 ? 100 : g_set.drop);
    const char *mode = getenv("HOS_COOP_AUTO"), *room = getenv("HOS_COOP_ROOM");
    if (!mode || !*mode || !strcmp(mode, "off")) return;
    if (strcmp(mode, "create") && strcmp(mode, "join")) {
        hos_log("coop: HOS_COOP_AUTO must be off, create or join");
        return;
    }
    g_set.join = !strcmp(mode, "join");
    if (room && *room && !coop_secret_normalize(room, g_set.room)) {
        hos_log("coop: HOS_COOP_ROOM is not a room secret (18 characters, XXXXXX-XXXXXX-XXXXXX)");
        return;
    }
    if (g_set.join && !g_set.room[0]) {
        hos_log("coop: HOS_COOP_AUTO=join needs HOS_COOP_ROOM");
        return;
    }
    g_set.automatic = 1;
    char label[8] = "";
    if (g_set.room[0]) label_of(g_set.room, label);
    hos_log("coop: HOS_COOP_AUTO=%s: %s%s at start", mode, g_set.room[0] ? "room " : "a new room", label);
}

// g_set into the session's settings (g_set_lock held).
static void configure_locked(CoopSess *s) {
    CoopParams p = {.server = g_set.server, .name = g_set.name, .client_id = coop_client_id(), .on = g_set.on,
                    .retry = g_set.automatic, .local_port = g_set.port, .drop_pct = g_set.drop};
    coop_sess_configure(s, &p);
}

static void coop_main(void *arg) {
    CoopSess *s = arg;
    for (;;) {
        coop_sess_step(s);
        if (!__atomic_load_n(&g_stop, __ATOMIC_ACQUIRE) || s->state != COOP_OFF) continue;
        coop_lock(&g_set_lock);
        coop_lock(&s->lock);
        const int end = g_stop && !s->cfg_pending;   // (not when co-op was switched on again meanwhile, or a later stop waits)
        coop_unlock(&s->lock);
        if (end) g_ended = 1;
        coop_unlock(&g_set_lock);
        if (end) return;   // (the stop was carried out: the socket is closed and the view says off)
    }
}

// The coop thread for g_set, which says on (g_life and g_set_lock held). 1 when it runs.
static int run_locked(CoopSess *s) {
    if (g_live && g_ended) {   // it carried an earlier stop out and has left its loop: gone in a moment
        if (!coop_thread_join(&g_thread, 2000)) return 0;
        __atomic_store_n(&g_live, 0, __ATOMIC_RELEASE);
    }
    g_ended = 0;
    __atomic_store_n(&g_stop, 0, __ATOMIC_RELEASE);
    configure_locked(s);
    if (g_live) return 1;   // (an earlier stop is still on its way, held up: taken back, the same thread goes on)
    if (g_set.automatic && !g_auto_done) {   // HOS_COOP_AUTO's room, queued behind the settings
        g_auto_done = 1;
        coop_sess_command(s, g_set.join ? COOP_CMD_JOIN : COOP_CMD_CREATE, g_set.room);
    }
    if (!coop_thread_start(&g_thread, coop_main, s)) {
        hos_log("coop: cannot start the co-op thread");
        return 0;
    }
    __atomic_store_n(&g_live, 1, __ATOMIC_RELEASE);
    return 1;
}

// Asks the coop thread to end (g_life and g_set_lock held). 1 when there is one: the caller then queues the session's
// stop, which wakes the thread, and waits with stop_wait once g_set_lock is released. In this order: a thread that had
// carried the stop out before it was asked to end would sit through one more idle wait first.
static int stop_locked(void) {
    g_set.on = 0;
    __atomic_store_n(&g_on, 0, __ATOMIC_RELEASE);
    if (g_live) __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE);
    return g_live;
}

static void stop_wait(void) {   // (g_life held, g_set_lock not: the thread takes it on its way out)
    if (coop_thread_join(&g_thread, STOP_WAIT_MS)) __atomic_store_n(&g_live, 0, __ATOMIC_RELEASE);
}

void coop_init(void) {
    const char *on = getenv("HOS_COOP");   // (set, it wins over the file's line both ways: env_value_off, the switches' rule)
    if (on ? env_value_off(on) : hos_ini_option("coop", 0) == 0) return;   // (off: no socket, no thread)
    if (!coop_client_id()) {   // an id others could guess would let them take this player's place
        hos_log("coop: the system's random source failed; co-op stays off");
        return;
    }
    CoopSess *s = process_session();
    if (!s) return;
    coop_once(&g_set_once, make_locks, NULL);
    coop_lock(&g_life);
    coop_lock(&g_set_lock);
    if (!g_ready) {   // (once: a second call, or one after the menu switched co-op on, changes nothing)
        read_settings(NULL);
        g_ready = 1;
        g_set.on = 1;
        __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
        if (!run_locked(s)) stop_locked();
    }
    coop_unlock(&g_set_lock);
    coop_unlock(&g_life);
}

int coop_configure(int on, const char *name, const char *server) {
    if (on && !coop_client_id()) {
        hos_log("coop: the system's random source failed; co-op stays off");
        return 0;
    }
    CoopSess *s = process_session();
    if (!s) return 0;
    coop_once(&g_set_once, make_locks, NULL);
    coop_lock(&g_life);
    coop_lock(&g_set_lock);
    int ok = 1, wait = 0;
    if (g_ready) {
        coop_name_clean(g_set.name, sizeof g_set.name, (const uint8_t *)(name ? name : ""), name ? strlen(name) : 0);
        if (!g_set.name[0]) snprintf(g_set.name, sizeof g_set.name, "Player-%04x", g_name_digits);
        copy_str(g_set.server, sizeof g_set.server, server);
    } else if (on) {   // (off at start: the settings are read now, with the menu's name and relay)
        const char *const menu[2] = {name ? name : "", server ? server : ""};
        read_settings(menu);
        g_ready = 1;
    }
    if (on) {
        g_set.on = 1;
        __atomic_store_n(&g_on, 1, __ATOMIC_RELEASE);
        ok = run_locked(s);
        if (!ok) stop_locked();
    } else {
        wait = stop_locked();
        if (wait) configure_locked(s);   // (the session's stop: BYE and LEAVE, the socket closed)
    }
    coop_unlock(&g_set_lock);
    if (wait) stop_wait();
    coop_unlock(&g_life);
    return ok;
}

void coop_shutdown(void) {
    if (!__atomic_load_n(&g_live, __ATOMIC_ACQUIRE)) return;   // (no thread: co-op is off, or never was on)
    CoopSess *s = process_session();
    coop_lock(&g_life);
    coop_lock(&g_set_lock);
    const int wait = stop_locked();
    if (wait) sess_stop_because(s, "the game is closing");
    coop_unlock(&g_set_lock);
    if (wait) stop_wait();
    coop_unlock(&g_life);
}

int coop_command(CoopCmd cmd, const char *secret) {
    CoopSess *s = process_session();
    if (!s || !__atomic_load_n(&g_on, __ATOMIC_ACQUIRE)) return 0;   // (a command right after coop_configure(1) is queued)
    return coop_sess_command(s, cmd, secret);
}

static CoopRemoveFn g_remove_fn;
static void *g_remove_user;

int coop_on_remove(CoopRemoveFn fn, void *user) {
    g_remove_user = user;
    __atomic_store_n(&g_remove_fn, fn, __ATOMIC_RELEASE);
    return 1;
}

int coop_remove_member(int member) {
    CoopSess *s = process_session();
    if (!s) return 0;
    CoopRemoveFn fn = __atomic_load_n(&g_remove_fn, __ATOMIC_ACQUIRE);
    int signed_out = fn && fn(member, g_remove_user);
    CoopView v;
    coop_sess_view(s, &v);
    int kicked = v.made && coop_sess_remove(s, member);   // the relay takes a removal only from the member it marks
    return signed_out || kicked;
}

void coop_view_get(CoopView *out) {
    CoopSess *s = process_session();
    if (s) coop_sess_view(s, out);
    else memset(out, 0, sizeof *out);
}

int coop_send(uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s && coop_sess_send(s, 0, kind, bytes, n);
}

int coop_send_to(int member, uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s && member > 0 && coop_sess_send(s, member, kind, bytes, n);
}

int coop_on_message(uint8_t kind, CoopMsgFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_message(s, kind, fn, user);
}

int coop_on_view(CoopViewFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_view(s, fn, user);
}

int coop_on_tick(CoopTickFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_tick(s, fn, user);
}
