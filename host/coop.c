// Co-op session layer (coop.h): the state machine of one player's link, and the thread that runs it.
//
// There is one way to reach the other players, and it is the other players' games. One player hosts: its game listens
// on a TCP port (43400 unless coop_port says otherwise) and is the room's maker. The others connect to that game's
// address (coop_server=, or the Co-op tab's Host address row); no one in between forwards anything, and the host
// passes every player message on to the rest. Up to eight games, the host one of them.
//
// States: OFF (co-op is off: HOS_COOP or the options file's coop= did not switch it on), LEFT (on, in no room),
// CONNECTING (reaching the host, or waiting for the first player on the port), JOINED, RECONNECTING (the host went
// silent; the same player joining the same room again as the same member).
//
// A room is its secret (coop_proto.h). The host makes one when it starts hosting; a player that connects is told it in
// ROOM and takes the keys made from it, so every frame of that room carries the locator's tag and every player message
// is sealed with the message key: what a game takes was made by a player of this room and was not changed on the way.
// Reaching the port gets a game the secret — the link trusts the players it reaches, as docs/coop.md says — and what
// the keys hold is that two games mean one room, and that nothing of another room, or of this room's earlier sessions,
// is taken.
//
// Joining is a short exchange: CONNECT, answered by CHALLENGE (a cookie) and ROOM (the secret); the CONNECT that
// carries the cookie back and the room's locator is answered by JOINED with a member id and the roster, or by an ERROR
// that says why not. The answers carry the attempt's nonce, and the ones after the keys are known carry its tag. A
// burst of four CONNECTs 250 ms apart carries the exchange over a slow link; a burst without an answer is followed by a
// wait of 1, 2, 4, 8, then 15 s. In a room a PING every 2 s measures the round trip, and 8 s without a frame from the
// host is RECONNECTING. JOINED hands out a rejoin token, which the next CONNECT of the same player carries: the host
// gives a member's id back only to it, for a minute after it left.
//
// Every join is a new session: a random number the CONNECT tells the host, which lists it in the roster the host
// broadcasts at every change and every 5 s. A player's messages carry its session and a counter from 1 in the sealed
// envelope; the others take a message only of the session the roster names for its sender and once within a window of
// 64 counters, so a message recorded before a rejoin, or sent again within the window, is refused. Four older ones in a
// row start the window again (window_below), after which a recorded message of that session can come again: a layer
// that must take a message once checks a counter of its own.
//
// The host is a player too: it holds the roster, marks who is away (one it has heard nothing from for 15 s), removes a
// member when its own game asks (the maker's remedy: KICK, then the link ends), and its own messages go out like
// everyone else's. It keeps no room state on disk: when it leaves or closes, the room ends.
//
// One thread owns the sockets, the DNS lookup and the state. Other threads reach it through a mailbox (commands, settings), an
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
    ROSTER_EVERY_MS = 5000,          // (hosting) who is in the room, said again this often whatever else changed
    ROSTER_AGAIN_MS = 750,           // and again after a change, so that a lost one is made up
    AWAY_MS = 15000,                 // (hosting) a member it has heard nothing from for this long is marked away
    HANDSHAKE_MS = 10000,            // a link that has not become a member by then is closed
    CONNECT_WAIT_MS = 3000,          // how long a link to the host is waited for
    TOKEN_KEEP_MS = 60000,           // a member that left may come back as the same member for this long
    ROSTER_AHEAD = 1 << 20,          // a roster's number is taken at most this far ahead of the last one's
    ROSTER_STALE_MS = 4000,          // after the silence time and this long without a roster taken, any roster is taken
    STATS_MS = 60000,                // the periodic counters line, while in a room
    LOG_EVERY_MS = 30000,            // a repeated cause logs this often
    RENAME_MS = 1000,                // a name change is told to the host after this much quiet
    OUT_SLOTS = 64,                  // messages waiting to be sent
    LINK_SLOTS = COOP_MAX_MEMBERS + 4,   // the links one session serves: its members, and a few still handshaking
    DEFAULT_PORT = 43400,            // the port a game hosts on, and the one it goes to when the address names none
    VIEW_HOOKS = 8,
    TICK_HOOKS = 4,
};

typedef struct { uint8_t dst, kind; uint16_t len; uint8_t data[COOP_MAX_MESSAGE]; } OutMsg;
typedef struct { uint8_t token[COOP_COOKIE_LEN]; uint64_t session; uint64_t until; } GoneMember;   // until 0: never left
typedef struct { int slot; uint64_t nonce, since; uint8_t cookie[COOP_COOKIE_LEN]; } PendingLink;   // (hosting) accepted, not yet a member
typedef struct { CoopMsgFn fn; void *user; } MsgHandler;
typedef struct { CoopViewFn fn; void *user; } ViewHook;
typedef struct { CoopTickFn fn; void *user; } TickHook;

enum { EVENT_RETRY_MS = 250, EVENT_ACK_MS = 50, EVENT_SEND_MS = 32 };
typedef struct { CoopEventFn fn; void *user; } EventHandler;
typedef struct {
    uint64_t session, next, ack_epoch;
    uint16_t acked, sent;
} EventTarget;
typedef struct {
    uint64_t id;
    uint16_t len;
    uint8_t kind;
    EventTarget target[COOP_MAX_MEMBERS + 1];
    uint8_t bytes[COOP_EVENT_MAX];
} EventOut;
typedef struct {
    uint64_t session, id, high, ack_at, epoch, consent_epoch;
    uint16_t len, parts;
    uint8_t kind, dirty;
    uint8_t bytes[COOP_EVENT_MAX];
} EventIn;

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
    char server[160], name[COOP_NAME_MAX + 1], host[128];   // server: the host to join, "host", "host:port" or "[v6]:port"
    uint16_t port, host_port;        // port: the port this game hosts on; host_port: the one that address names
    uint64_t client_id;
    unsigned drop_pct;
    int retry;

    // ---- shared, behind lock
    CoopLock lock;
    CoopEvent off_done;
    CoopEvent wake;                             // a command or settings were queued: a session with no link to wait on takes them now
    int cmd;                                    // a command waiting (COOP_CMD_*), 0 none; a later one replaces it
    char cmd_secret[COOP_SECRET_LEN + 1];
    int cmd_remove;                             // a member the maker removes, 0 none, and its session in the view then
    uint64_t cmd_remove_session;
    int cfg_pending;                            // pending holds settings the thread has not applied
    struct { char server[160], name[COOP_NAME_MAX + 1], why[48]; uint64_t client_id; uint16_t port; unsigned drop_pct; int retry, on; } pending;
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
    uint64_t accepted_self, accepted[COOP_MAX_MEMBERS + 1];
    uint64_t accepted_epoch[COOP_MAX_MEMBERS + 1]; // revocations remembered even across revoke/reaccept between steps
    EventHandler event_handler[256];
    EventOut event_out[COOP_EVENT_SLOTS];
    uint64_t event_id;                         // allocated under lock, never reused in a session
    // Incoming reassembly and pacing are used only by the stepping thread.
    EventIn event_in[COOP_MAX_MEMBERS + 1];
    uint64_t event_next_send;
    unsigned event_turn;

    // ---- the stepping thread's
    CoopState state;
    CoopTcpSet *link;                           // the links this session serves: its own (joining) or its members' (hosting)
    CoopTcpListen *listener;                    // (hosting) the port it waits on; it lives inside link
    int hosting;                                // this game listens: it is the room's maker
    int process_session;
    CoopAddr host_at;                           // (joining) the host's address
    int host_known;
    char host_text[64];                         // the host's resolved address, for the trace
    int own_slot;                               // (joining) the slot the link to the host took
    int slot_of[COOP_MAX_MEMBERS + 1];          // (hosting) member id -> link slot, 0 none
    int who[LINK_SLOTS + 1];                    // link slot -> member id, 0 for a link still handshaking
    PendingLink pend[4];                        // links that have not become members yet: each holds the cookie it must carry back
    GoneMember gone[COOP_MAX_MEMBERS];          // (hosting) members that left: their token, kept for a minute
    int intent, made_room;                      // CONNECT's intent; this session's own hosting made the room
    uint64_t room_session;                      // the session the room lists for this player since its last JOINED
    char want[COOP_SECRET_LEN + 1];             // the secret of the room being joined, or this host's own
    uint8_t locator[COOP_KEY_LEN];              // of want: the tags' key
    uint8_t msg_key[COOP_MSG_KEY_LEN];          // of want: seals the players' messages
    uint64_t session, nonce;                    // this join's session (in the roster); this attempt's nonce
    uint8_t cookie[COOP_COOKIE_LEN], token[COOP_COOKIE_LEN];
    int have_cookie, have_token, have_secret, said_answer, heard_host;
    uint32_t connect_seq, out_seq, out_counter;
    uint64_t next_tx, last_connect_tx, last_rx, next_ping, next_stats, rename_at, next_roster, roster_again;
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
    uint8_t rx[COOP_MAX_DGRAM];
};

// ---- logging

// HOS_COOP_TRACE=1: a line per frame, and the host's address (never logged otherwise)
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
    case COOP_T_ROOM: return "ROOM";
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
// since a session or nonce someone could guess would let them answer for the host or take this player's place.
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

// One frame to the link at slot. key NULL: a zero tag, which is how a host answers a link before it has handed out the
// room's key. 1 when the link took it (a link whose backlog is full does not).
static int send_slot(CoopSess *s, int slot, uint8_t type, uint32_t room, uint8_t src, uint8_t dst,
                     const void *payload, size_t n, const uint8_t *key) {
    CoopTcp *t = coop_tcp_at(s->link, slot);
    if (!t) return 0;
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .src = src, .dst = dst, .room = room, .seq = s->out_seq++};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, key);
    if (!k) return 0;
    if (!coop_tcp_send(t, d, k)) {
        __atomic_add_fetch(&s->n_outbox_full, 1, __ATOMIC_RELAXED);
        return 0;
    }
    bump(&s->n_tx);
    TRACE(s, "tx %s to link %d, %u bytes", type_name(type), slot, (unsigned)k);
    return 1;
}

// A frame of the room we are in: over the link to the host, or (hosting) to every member dst names — a member id, or
// COOP_DST_ALL for every other member. A host does not send a room frame to itself.
static void send_room(CoopSess *s, uint8_t type, uint8_t dst, const void *payload, size_t n) {
    if (!s->link) return;
    if (!s->hosting) {
        send_slot(s, s->own_slot, type, s->room, (uint8_t)s->self, dst, payload, n, s->locator);
        return;
    }
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        if (id == s->self || !s->slot_of[id]) continue;
        if (dst != COOP_DST_ALL && dst != id) continue;
        send_slot(s, s->slot_of[id], type, s->room, (uint8_t)s->self, dst, payload, n, s->locator);
    }
}

// The host's answer to a link that is still handshaking: no room number, no member, no tag — it has no keys yet.
static void answer_link(CoopSess *s, int slot, uint8_t type, const void *payload, size_t n) {
    send_slot(s, slot, type, 0, 0, 0, payload, n, NULL);
}

static void go_left(CoopSess *s, const char *why, int log_it);
static void events_reset(CoopSess *s);
static void end_link(CoopSess *s, int slot, const char *why);
static void publish_roster(CoopSess *s);

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

// Who I am, to the host: the name, this join's session and nonce, the cookie it gave back, the room's keys once the
// host has handed them out, and the token of the member I was. A host is asked; it does not ask.
static void send_connect(CoopSess *s, uint64_t now) {
    if (s->hosting) return;
    CoopConnect c = {.intent = (uint8_t)s->intent, .client_id = s->client_id, .session = s->session, .nonce = s->nonce};
    uint8_t payload[128];
    memcpy(c.locator, s->locator, COOP_KEY_LEN);
    copy_str(c.name, sizeof c.name, s->name);
    if (s->have_cookie) memcpy(c.cookie, s->cookie, COOP_COOKIE_LEN);
    if (s->have_token) memcpy(c.token, s->token, COOP_COOKIE_LEN);
    size_t n = coop_connect_pack(payload, sizeof payload, &c);
    send_slot(s, s->own_slot, COOP_T_CONNECT, 0, 0, 0, payload, n, s->have_secret ? s->locator : NULL);
    if (!s->have_cookie) s->last_connect_tx = now;
}

// ---- leaving

// Every link, and the port when this game hosts one. The set owns the listener, so closing it closes the port.
static void close_link(CoopSess *s) {
    if (s->link) coop_tcp_set_free(s->link);
    s->link = NULL;
    s->listener = NULL;
    s->own_slot = 0;
    memset(s->slot_of, 0, sizeof s->slot_of);
    memset(s->who, 0, sizeof s->who);
    memset(s->pend, 0, sizeof s->pend);
}

// Tells the others, twice: BYE to every member, then LEAVE. A host's links end with the room, but it says so first.
static void say_goodbye(CoopSess *s) {
    if (!s->link || !s->room) return;
    for (int i = 0; i < 2; i++) {
        if (s->state == COOP_JOINED) send_sealed(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
        send_room(s, COOP_T_LEAVE, COOP_DST_ALL, NULL, 0);
    }
}

// Out of the room (or never in): the links close and the reason shows.
static void go_left(CoopSess *s, const char *why, int log_it) {
    int was_in = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    if (log_it) {
        if (was_in) say(s, "left room (%s)", why);
        else say(s, "no room: %s", why);
    }
    close_link(s);
    s->hosting = 0;
    s->room = 0, s->self = 0, s->made_room = 0;
    s->roster.n = 0;
    s->have_token = 0;
    memset(s->rw, 0, sizeof s->rw);
    events_reset(s);
    set_state(s, COOP_LEFT);
    set_status(s, "%s", why);
    s->dirty = 1;
}

// ---- the link: opening it, and getting into the room

// (hosting) The port this game waits on, and what the others can type to reach it.
// (joining) The host's address, and a link to it. 0 and a status when they cannot be had.
static int ensure_link(CoopSess *s, uint64_t now) {
    if (s->hosting) {
        if (s->link) return 1;
        CoopTcpListen *l = coop_tcp_listen(s->port);
        if (!l) {
            set_status(s, "cannot listen on port %u", (unsigned)s->port);
            SAY_LIMITED(s, L_SOCKET, now, "cannot listen on port %u: is another game hosting on it?", (unsigned)s->port);
            return 0;
        }
        s->link = coop_tcp_set_new(l, LINK_SLOTS);
        if (!s->link) {
            coop_tcp_listen_close(l);
            set_status(s, "cannot start the room");
            return 0;
        }
        s->listener = l;   /* (the set owns it: close_link closes it) */
        CoopAddr at[8];
        const unsigned n = coop_net_local_addresses(at, 8, s->port);
        for (unsigned i = 0; i < n && i < 4; i++) {
            char text[64];
            coop_addr_str(&at[i], text, sizeof text);
            say(s, "players on this network can join %s", text);
        }
        return 1;
    }
    if (!s->link) {
        s->link = coop_tcp_set_new(NULL, LINK_SLOTS);
        if (!s->link) return 0;
    }
    if (!s->host_known) {
        say(s, "connecting to the host");
        TRACE(s, "the host is %s", s->server);
        if (!coop_net_resolve(s->host, s->host_port, &s->host_at)) {
            set_status(s, "cannot find the host's address");
            SAY_LIMITED(s, L_RESOLVE, now, "cannot resolve the host's name");
            return 0;
        }
        coop_addr_str(&s->host_at, s->host_text, sizeof s->host_text);
        s->host_known = 1;
    }
    if (!s->own_slot) {
        CoopTcp *t = coop_tcp_connect(&s->host_at, CONNECT_WAIT_MS);
        if (!t) {
            set_status(s, "cannot reach the host at %s", s->host_text);
            SAY_LIMITED(s, L_SOCKET, now, "no link to the host at %s", s->host_text);
            s->host_known = 0;   // the whole attempt starts again, the name looked up fresh
            return 0;
        }
        s->own_slot = coop_tcp_add(s->link, t);
        if (!s->own_slot) {
            coop_tcp_close(t);
            return 0;
        }
        TRACE(s, "the host is at %s", s->host_text);
    }
    return 1;
}

static void schedule_backoff(CoopSess *s, uint64_t now) {
    s->next_tx = now + s->backoff;
    if (!s->heard_host && s->own_slot) set_status(s, "no answer from the host; trying again in %u s", (s->backoff + 500) / 1000);
    s->backoff = s->backoff * 2 > s->backoff_max_ms ? s->backoff_max_ms : s->backoff * 2;
}

// A new attempt at the host's room: its secret, and whether it is a rejoin of the room this player was in (which keeps
// the rejoin token, so the host gives back the same member). The link itself starts again: a TCP link that ended does
// not come back. Every attempt is a new session and a new nonce; 0 (and no room) when the system's random source
// cannot give them.
static int begin_episode(CoopSess *s, const char *secret, int rejoin, uint64_t now) {
    uint64_t session = random64(), nonce = random64();
    if (!session || !nonce) {
        go_left(s, "the system's random source failed: co-op needs it", 1);
        return 0;
    }
    s->intent = COOP_INTENT_JOIN;
    if (secret != s->want) copy_str(s->want, sizeof s->want, secret);   // a rejoin passes the secret it already holds
    coop_room_keys(s->want, s->locator, s->msg_key);
    s->session = session, s->nonce = nonce, s->out_counter = 0;
    events_reset(s);
    s->connect_seq = s->out_seq++;
    s->have_cookie = s->said_answer = s->heard_host = 0;
    s->have_secret = s->want[0] != 0;   // a player that already holds the room's secret says so in its first CONNECT
    if (!rejoin) s->have_token = 0;
    s->burst_left = 0;
    s->next_tx = now;
    s->backoff = s->backoff_first_ms;
    s->host_known = 0;   // looked up again: the name may point elsewhere by now
    if (s->link) end_link(s, s->own_slot, "reconnecting");   // the old link is not the host's link any more
    if (!rejoin) {
        s->room = 0, s->self = 0;
        s->roster.n = 0;
    }
    set_state(s, rejoin ? COOP_RECONNECTING : COOP_CONNECTING);
    if (rejoin) set_status(s, "rejoining the host");
    else set_status(s, "joining the host");
    s->dirty = 1;
    return 1;
}

// (hosting) This game is the room: the secret it was given, the port it waits on, and itself as the room's only member
// until someone connects. Its rules are the ones its key signs (coop_state.h), as the maker's always are.
static int start_hosting(CoopSess *s, const char *secret, uint64_t now) {
    uint64_t session = random64(), room = random64();
    if (!session || !room) {
        go_left(s, "the system's random source failed: co-op needs it", 1);
        return 0;
    }
    copy_str(s->want, sizeof s->want, secret);
    coop_room_keys(s->want, s->locator, s->msg_key);
    memcpy(s->secret, s->want, sizeof s->secret);
    s->intent = COOP_INTENT_HOST;
    s->hosting = 1;
    s->have_secret = 1;
    s->made_room = 1;
    s->self = 1;                       // (the host is member 1: the room's maker, and a player like the others)
    s->room = (uint32_t)room | 1u;
    s->session = session, s->room_session = session, s->out_counter = 0;
    s->roster.n = 0;
    events_reset(s);
    if (!ensure_link(s, now)) {
        s->hosting = 0;
        go_left(s, s->status[0] ? s->status : "cannot listen", 0);
        return 0;
    }
    set_state(s, COOP_JOINED);
    set_status(s, "%s", "");
    s->last_rx = now;
    s->next_ping = now + s->ping_ms;
    s->next_stats = now + STATS_MS;
    s->next_roster = now + ROSTER_EVERY_MS;
    char label[8];
    label_of(s->secret, label);
    say(s, "hosting room %s on port %u", label, (unsigned)s->port);
    s->dirty = 1;
    publish_roster(s);
    return 1;
}

static void connect_timers(CoopSess *s, uint64_t now) {
    if (now < s->next_tx) return;
    if (s->burst_left == 0) {   // a new burst: the address and the link first
        if (!ensure_link(s, now)) {
            s->host_known = 0;
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

static void publish_roster(CoopSess *s);
static int apply_roster(CoopSess *s, const CoopRoster *r);

// ---- the room, when this game is the one that hosts it
//
// It holds who is in the room (its own line first, marked as the maker's), which link each of them came in on, and the
// rejoin token of each one that left — the only way a player that drops for a moment gets its member id back. Every
// change goes out as a ROSTER, and one goes out every ROSTER_EVERY_MS whatever else changed, so that a player that
// missed one catches up.

// The roster of a ROSTER frame is the room's, with this game's own line marked as the maker's.
static void host_roster(const CoopSess *s, CoopRoster *r) {
    memset(r, 0, sizeof *r);
    for (int i = 0; i < s->roster.n && r->n < COOP_MAX_MEMBERS; i++) r->m[r->n++] = s->roster.m[i];
    CoopMemberInfo *own = NULL;
    for (int i = 0; i < r->n; i++)
        if (r->m[i].id == s->self) own = &r->m[i];
    if (!own && r->n < COOP_MAX_MEMBERS) own = &r->m[r->n++];
    if (own) {
        own->id = (uint8_t)s->self;
        own->flags = COOP_FLAG_MAKER;
        own->session = s->session;
        copy_str(own->name, sizeof own->name, s->name);
    }
}

// (hosting) The room as r says it is, said to every member. The number goes up with each one, which is how a player
// knows a later one arrived (on_roster); apply_roster logs who came, went, changed name or went away.
static void publish_rooms(CoopSess *s, const CoopRoster *r) {
    apply_roster(s, r);
    s->roster_seq++;
    uint8_t payload[COOP_MAX_PAYLOAD];
    size_t n = coop_roster_pack(payload, sizeof payload, r);
    if (n) send_room(s, COOP_T_ROSTER, COOP_DST_ALL, payload, n);
    const uint64_t now = coop_now_ms();
    s->next_roster = now + ROSTER_EVERY_MS;
    s->roster_again = now + ROSTER_AGAIN_MS;
    s->dirty = 1;
}

static void publish_roster(CoopSess *s) {
    CoopRoster r;
    host_roster(s, &r);
    publish_rooms(s, &r);
}

// A link that is over: whoever it carried leaves the room. Its rejoin token is kept for a minute, which is how the same
// player coming back keeps its member id instead of becoming someone new.
static void end_link(CoopSess *s, int slot, const char *why) {
    if (!coop_tcp_at(s->link, slot)) return;
    const int member = s->who[slot];
    coop_tcp_drop(s->link, slot);
    for (int i = 0; i < (int)(sizeof s->pend / sizeof s->pend[0]); i++)
        if (s->pend[i].slot == slot) s->pend[i] = (PendingLink){0};
    if (slot == s->own_slot) s->own_slot = 0;   // (the link to the host starts again as a new one)
    s->who[slot] = 0;
    if (!member) return;
    s->slot_of[member] = 0;
    const CoopMemberInfo *m = member_of(s, member);
    if (m) {
        for (int i = 0; i < COOP_MAX_MEMBERS; i++) {
            if (s->gone[i].until && s->gone[i].session != m->session) continue;
            memset(s->gone[i].token, 0, COOP_COOKIE_LEN);
            s->gone[i].session = m->session;
            s->gone[i].until = coop_now_ms() + TOKEN_KEEP_MS;
            break;
        }
    }
    if (!s->hosting) return;
    CoopRoster left;
    memset(&left, 0, sizeof left);
    for (int i = 0; i < s->roster.n && left.n < COOP_MAX_MEMBERS; i++)
        if (s->roster.m[i].id != member) left.m[left.n++] = s->roster.m[i];
    (void)why;   /* (apply_roster, through publish_rooms, says who left and why the log has it) */
    publish_rooms(s, &left);   // everyone hears who left
}

// Sixteen random bytes for a link to come back with. Zero when the system's source failed, which is a link the host
// cannot let into a room: a token anyone could guess would hand them someone else's member id.
static int random_token(CoopSess *s, uint8_t out[COOP_COOKIE_LEN]) {
    (void)s;
    return coop_random(out, COOP_COOKIE_LEN) && out[0];
}

// How many links are handshaking: each holds a cookie of its own, so a link that has proved nothing yet has its own
// answer and cannot borrow another's.
static int pending_count(const CoopSess *s) {
    int n = 0;
    for (int i = 0; i < (int)(sizeof s->pend / sizeof s->pend[0]); i++) n += s->pend[i].slot != 0;
    return n;
}

// The handshaking record of a link that was just accepted, or -1 when there is no room for one or the system's random
// source failed: a cookie anyone could guess would let its bearer take another link's place.
static int pending_add(CoopSess *s, int slot, uint64_t nonce, uint64_t now) {
    for (int i = 0; i < (int)(sizeof s->pend / sizeof s->pend[0]); i++) {
        if (s->pend[i].slot) continue;
        s->pend[i] = (PendingLink){.slot = slot, .nonce = nonce, .since = now};
        if (!coop_random(s->pend[i].cookie, COOP_COOKIE_LEN) || !s->pend[i].cookie[0]) {
            memset(s->pend[i].cookie, 0, COOP_COOKIE_LEN);
            s->pend[i].slot = 0;
            return -1;
        }
        return i;
    }
    return -1;
}

// The host's answer to a CONNECT it has not yet made a member: a cookie to carry back, and the room's secret, both for
// this attempt's nonce. A burst sends the same CONNECT several times, so this goes out several times.
static void answer_connect(CoopSess *s, int slot, uint64_t nonce, const uint8_t cookie[COOP_COOKIE_LEN]) {
    uint8_t payload[128];
    size_t k = coop_challenge_pack(payload, sizeof payload, nonce, cookie);
    if (k) answer_link(s, slot, COOP_T_CHALLENGE, payload, k);
    k = coop_room_pack(payload, sizeof payload, nonce, s->secret);
    if (k) answer_link(s, slot, COOP_T_ROOM, payload, k);
}

// Close a link, after what was just said on it has had a chance to get out: a refusal or a removal the other end never
// reads is a player that keeps asking for a room it was refused. Bounded, so a link that cannot drain is closed anyway.
static void close_said_link(CoopSess *s, int slot, const char *why) {
    for (int i = 0; i < 20 && coop_tcp_at(s->link, slot); i++)
        coop_tcp_next(s->link, s->rx, sizeof s->rx, NULL, 2);   // (it writes what the links still hold)
    end_link(s, slot, why);
}

// The host's "no" to a link that is not yet a member, said before the link closes: what a player of another room, or
// of a room that is full, gets is the reason and a closed link.
static void refuse_link(CoopSess *s, int slot, uint8_t reason, const char *why, uint64_t nonce) {
    CoopError e = {.reason = reason, .nonce = nonce};
    copy_str(e.text, sizeof e.text, why);
    uint8_t payload[128];
    size_t k = coop_error_pack(payload, sizeof payload, &e);
    if (k) answer_link(s, slot, COOP_T_ERROR, payload, k);
    close_said_link(s, slot, why);
}

// The member id a rejoin token belongs to (0 when it is no one's): a token is kept for a minute after its holder left,
// which is how a player that drops for a moment gets its id back instead of becoming someone new.
static int gone_of(const CoopSess *s, const uint8_t token[COOP_COOKIE_LEN], uint64_t now) {
    if (!token[0]) return 0;
    for (int i = 0; i < COOP_MAX_MEMBERS; i++)
        if (s->gone[i].until > now && s->gone[i].session && !memcmp(s->gone[i].token, token, COOP_COOKIE_LEN)) return i + 1;
    return 0;
}

// (hosting) A link that has proved it holds the room's keys becomes a member: the id it had before, if it carries the
// token of a player that left within the last minute, or else the lowest free one. The answer is its number, the room
// key it already has, a token for next time, and who is in the room.
static void take_member(CoopSess *s, int slot, const CoopConnect *c, uint64_t now) {
    const int id_old = gone_of(s, c->token, now);
    int id = id_old;
    if (id_old) s->gone[id_old - 1] = (GoneMember){0};   // (the token is spent: it is used once)
    for (int i = 2; !id && i <= COOP_MAX_MEMBERS; i++)   // the host is member 1, and the room holds eight
        if (!s->slot_of[i] && !member_of(s, i)) id = i;
    if (!id) {
        say(s, "refused a player: the room is full");
        refuse_link(s, slot, COOP_E_ROOM_FULL, "the room is full", c->nonce);
        return;
    }
    s->who[slot] = id;
    s->slot_of[id] = slot;
    for (int i = 0; i < (int)(sizeof s->pend / sizeof s->pend[0]); i++)
        if (s->pend[i].slot == slot) s->pend[i] = (PendingLink){0};
    CoopMemberInfo *line = NULL;   // (the host's roster is its members' lines plus its own)
    for (int i = 0; i < s->roster.n; i++)
        if (s->roster.m[i].id == id) line = &s->roster.m[i];
    if (!line && s->roster.n < COOP_MAX_MEMBERS) line = &s->roster.m[s->roster.n++];
    if (line) {
        line->id = (uint8_t)id;
        line->flags = 0;
        line->session = c->session;
        copy_str(line->name, sizeof line->name, c->name);
    }
    uint8_t payload[COOP_MAX_PAYLOAD];
    CoopJoined j = {.member = (uint8_t)id, .nonce = c->nonce};
    if (!random_token(s, j.token)) {   // a link the host cannot give a member id to: it would not come back as itself
        s->who[slot] = 0;
        s->slot_of[id] = 0;
        say(s, "refused a player: this game cannot make random numbers");
        refuse_link(s, slot, COOP_E_SERVER_FULL, "this game cannot make random numbers: co-op needs them", c->nonce);
        return;
    }
    memcpy(s->gone[id - 1].token, j.token, COOP_COOKIE_LEN);   /* (what it answers with if it is the one that left) */
    s->gone[id - 1].session = c->session;
    s->gone[id - 1].until = 0;
    host_roster(s, &j.roster);
    CoopMemberInfo *mine = NULL;
    for (int i = 0; i < j.roster.n; i++)
        if (j.roster.m[i].id == id) mine = &j.roster.m[i];
    if (mine) {
        mine->session = c->session;
        copy_str(mine->name, sizeof mine->name, c->name);
        mine->flags = 0;
    }
    size_t k = coop_joined_pack(payload, sizeof payload, &j);
    if (k) send_slot(s, slot, COOP_T_JOINED, s->room, (uint8_t)s->self, (uint8_t)id, payload, k, s->locator);
    coop_lock(&s->lock);
    s->heard_at[id] = 0;
    coop_unlock(&s->lock);
    publish_roster(s);   // everyone hears who joined
}

// ---- reliable events and the consent owner's session-bound bindings

static int accepted_locked(const CoopSess *s, int member) {
    if (member < 1 || member > COOP_MAX_MEMBERS || member == s->view.self ||
        !__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE) || s->accepted_self != s->view.session) return 0;
    for (int i = 0; i < s->view.n; i++)
        if (s->view.member[i].id == member)
            return s->accepted[member] && s->accepted[member] == s->view.member[i].session;
    return 0;
}

int coop_sess_member_accepted(CoopSess *s, int member) {
    coop_lock(&s->lock);
    int ok = accepted_locked(s, member);
    coop_unlock(&s->lock);
    return ok;
}

void coop_sess_accept_members(CoopSess *s, const CoopView *view, uint32_t members) {
    coop_lock(&s->lock);
    if (view && (view->version != s->view.version || view->session != s->view.session)) {
        coop_unlock(&s->lock);
        return;
    }
    uint64_t self = view && view->state == COOP_JOINED ? view->session : 0;
    uint64_t accepted[COOP_MAX_MEMBERS + 1] = {0};
    for (int i = 0; view && self && i < view->n; i++) {
        const CoopPeer *p = &view->member[i];
        if (p->id >= 1 && p->id <= COOP_MAX_MEMBERS && p->id != view->self && (members & (1u << p->id)))
            accepted[p->id] = p->session;
    }
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        if (s->accepted[id] && (s->accepted_self != self || s->accepted[id] != accepted[id]) &&
            s->accepted_epoch[id] != UINT64_MAX) s->accepted_epoch[id]++;
        // Never reuse a receipt epoch. Exhaustion stays closed until this local session ends.
        s->accepted[id] = s->accepted_epoch[id] == UINT64_MAX ? 0 : accepted[id];
    }
    s->accepted_self = self;
    for (int i = 0; i < COOP_EVENT_SLOTS; i++) {
        EventOut *e = &s->event_out[i];
        int any = 0;
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
            if (!s->accepted_self || e->target[id].session != s->accepted[id]) e->target[id].session = 0;
            any |= e->target[id].session != 0;
        }
        if (!any) e->id = 0;
    }
    coop_unlock(&s->lock);
}

static void events_reset(CoopSess *s) {
    coop_lock(&s->lock);
    s->accepted_self = s->event_id = 0;
    memset(s->accepted, 0, sizeof s->accepted);
    memset(s->accepted_epoch, 0, sizeof s->accepted_epoch);
    for (int i = 0; i < COOP_EVENT_SLOTS; i++) {
        s->event_out[i].id = 0;
        memset(s->event_out[i].target, 0, sizeof s->event_out[i].target);
    }
    coop_unlock(&s->lock);
    // No payload clearing: lengths/ids make the old storage unreachable.
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        EventIn *e = &s->event_in[id];
        e->session = e->id = e->high = e->ack_at = e->epoch = e->consent_epoch = 0;
        e->parts = e->dirty = 0;
    }
    s->event_next_send = 0, s->event_turn = 0;
}

int coop_sess_on_event(CoopSess *s, uint8_t kind, CoopEventFn fn, void *user) {
    if (!coop_event_kind(kind)) return 0;
    coop_lock(&s->lock);
    s->event_handler[kind] = (EventHandler){fn, user};
    coop_unlock(&s->lock);
    return 1;
}

int coop_sess_send_event(CoopSess *s, int member, uint8_t kind, const void *bytes, size_t n) {
    if (!coop_event_kind(kind) || n > COOP_EVENT_MAX || (n && !bytes) || member < 0 ||
        (member > COOP_MAX_MEMBERS && member != COOP_DST_ALL)) return COOP_EVENT_INVALID;
    coop_lock(&s->lock);
    uint64_t recipients[COOP_MAX_MEMBERS + 1] = {0};
    int any = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if ((!member || member == COOP_DST_ALL || member == id) && accepted_locked(s, id))
            recipients[id] = s->accepted[id], any = 1;
    int result = COOP_EVENT_UNAVAILABLE;
    if (any) {
        result = s->event_id == UINT64_MAX ? COOP_EVENT_INVALID : COOP_EVENT_FULL;
        for (int i = 0; s->event_id != UINT64_MAX && i < COOP_EVENT_SLOTS; i++) {
            EventOut *e = &s->event_out[i];
            if (e->id) continue;
            memset(e->target, 0, sizeof e->target);
            for (int id = 1; id <= COOP_MAX_MEMBERS; id++) e->target[id].session = recipients[id];
            e->id = ++s->event_id, e->kind = kind, e->len = (uint16_t)n;
            if (n) memcpy(e->bytes, bytes, n);
            result = COOP_EVENT_QUEUED;
            break;
        }
    }
    coop_unlock(&s->lock);
    return result;
}

// The stepping thread also checks the live roster: a new roster may not have been published to callers yet.
static int event_peer(CoopSess *s, int id, uint64_t session, uint64_t *epoch) {
    const CoopMemberInfo *m = member_of(s, id);
    coop_lock(&s->lock);
    int ok = m && m->session == session && s->accepted_self == s->session && s->accepted[id] == session;
    if (ok) *epoch = s->accepted_epoch[id];
    coop_unlock(&s->lock);
    return ok;
}

// Every actual partial discard advances the receipt epoch, regardless of what invalidated the binding. Keep the
// id as a monotonic floor and keep completed receipts: neither abandonment nor revocation can revive an old id.
static void event_discard(EventIn *e) {
    e->dirty = 0;
    if (e->id > e->high && e->parts) {
        e->parts = 0;
        if (e->epoch != UINT64_MAX) e->epoch++;
    }
}

static void event_binding(EventIn *e, uint64_t session, uint64_t consent_epoch) {
    if (e->session != session) {
        e->session = session, e->id = e->high = e->ack_at = e->epoch = 0;
        e->parts = e->dirty = 0;
    }
    if (e->consent_epoch != consent_epoch) {
        e->consent_epoch = consent_epoch;
        event_discard(e);
    }
}

static int event_receive(CoopSess *s, int from, uint64_t session, uint8_t kind, const uint8_t *bytes, size_t n, uint64_t now) {
    (void)now;
    uint64_t epoch;
    if (!event_peer(s, from, session, &epoch)) return 0;
    if (kind == COOP_K_EVENT_ACK) {
        CoopEventAck a;
        if (!coop_event_ack_parse(bytes, n, &a) || a.sender_session != s->session) return 0;
        coop_lock(&s->lock);
        for (int i = 0; i < COOP_EVENT_SLOTS; i++) {
            EventOut *e = &s->event_out[i];
            if (e->id != a.id || e->target[from].session != session) continue;
            uint16_t full = coop_event_parts(e->len);
            EventTarget *t = &e->target[from];
            if ((a.parts & ~full) || a.epoch < t->ack_epoch) break;
            if (a.epoch > t->ack_epoch) {
                t->ack_epoch = a.epoch, t->acked = 0;
                t->sent = a.parts, t->next = 0;   // the receiver discarded the earlier assembly
            }
            t->acked |= a.parts;
            if (t->acked == full) t->session = 0;
            int any = 0;
            for (int id = 1; id <= COOP_MAX_MEMBERS; id++) any |= e->target[id].session != 0;
            if (!any) e->id = 0;
            break;
        }
        coop_unlock(&s->lock);
        return 1;
    }
    CoopEventPart p;
    if (!coop_event_part_parse(bytes, n, &p) || p.recipient_session != s->session) return 0;
    coop_lock(&s->lock);
    EventHandler h = s->event_handler[p.kind];
    coop_unlock(&s->lock);
    if (!h.fn) return 0;   // No application handler: never claim delivery.
    EventIn *e = &s->event_in[from];
    event_binding(e, session, epoch);
    if (e->epoch == UINT64_MAX) return 0;   // no receipt epoch may wrap within this session
    if (p.id < e->id || p.id < e->high) return 0;
    if (e->id != p.id) {
        if (p.id <= e->high) return 0;
        // Every fragment is the sender's oldest live event for this recipient. A higher id therefore retires any
        // lower unfinished id: it was abandoned, not an out-of-order event still waiting to be delivered.
        event_discard(e);
        if (e->epoch == UINT64_MAX) return 0;
        e->id = p.id, e->len = p.total, e->kind = p.kind, e->parts = 0;
    }
    if (e->len != p.total || e->kind != p.kind) return 0;
    uint16_t bit = (uint16_t)(1u << p.part);
    if (!(e->parts & bit)) {
        size_t count = n - COOP_EVENT_HEAD;
        if (count) memcpy(e->bytes + (size_t)p.part * COOP_EVENT_PART, p.bytes, count);
        e->parts |= bit;
    }
    if (e->parts == coop_event_parts(e->len) && e->id > e->high) {
        e->high = e->id;   // commit before the callback; no repeated dispatch, even after envelope resync
        h.fn(from, session, e->id, e->bytes, e->len, h.user);
    }
    e->dirty = 1;   // ACKs coalesce, including a duplicate whose previous ACK was lost.
    return 1;
}

static void events_tick(CoopSess *s, uint64_t now) {
    if (s->state != COOP_JOINED) return;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        EventIn *e = &s->event_in[id];
        if (!e->session) continue;
        uint64_t epoch;
        if (!event_peer(s, id, e->session, &epoch)) {
            event_discard(e);
            continue;
        }
        event_binding(e, e->session, epoch);
        if (e->dirty && now >= e->ack_at) {
            uint8_t bytes[COOP_EVENT_ACK_SIZE];
            CoopEventAck a = {.sender_session = e->session, .id = e->id, .parts = e->parts, .epoch = e->epoch};
            size_t n = coop_event_ack_pack(bytes, sizeof bytes, &a);
            e->dirty = 0, e->ack_at = now + EVENT_ACK_MS;
            if (n) send_sealed(s, (uint8_t)id, COOP_K_EVENT_ACK, bytes, n);
            if (s->state != COOP_JOINED) return;
        }
    }
    // Drop departed recipients even when there is no send due. No new peer inherits queued events.
    coop_lock(&s->lock);
    for (int i = 0; i < COOP_EVENT_SLOTS; i++) {
        EventOut *e = &s->event_out[i];
        if (!e->id) continue;
        int any = 0;
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
            EventTarget *t = &e->target[id];
            const CoopMemberInfo *m = member_of(s, id);
            if (!m || m->session != t->session || s->accepted[id] != t->session || s->accepted_self != s->session) t->session = 0;
            any |= t->session != 0;
        }
        if (!any) e->id = 0;
    }
    uint8_t bytes[COOP_EVENT_HEAD + COOP_EVENT_PART];
    size_t n = 0;
    int dst = 0;
    for (unsigned turn = 0; now >= s->event_next_send && turn < COOP_MAX_MEMBERS; turn++) {
        int id = (int)((s->event_turn + turn) % COOP_MAX_MEMBERS) + 1;
        EventOut *first = NULL;
        for (int i = 0; i < COOP_EVENT_SLOTS; i++) {
            EventOut *e = &s->event_out[i];
            if (e->id && e->target[id].session && (!first || e->id < first->id)) first = e;
        }
        if (!first) continue;
        EventTarget *t = &first->target[id];
        uint16_t full = coop_event_parts(first->len), missing = (uint16_t)(full & ~(t->sent | t->acked));
        if (!missing && now >= t->next) t->sent = t->acked, missing = (uint16_t)(full & ~t->acked);
        if (!missing) continue;
        uint8_t part = 0;
        while (!(missing & (1u << part))) part++;
        CoopEventPart p = {.recipient_session = t->session, .id = first->id, .total = first->len,
                           .kind = first->kind, .part = part, .bytes = first->bytes + (size_t)part * COOP_EVENT_PART};
        n = coop_event_part_pack(bytes, sizeof bytes, &p);
        t->sent |= (uint16_t)(1u << part);
        if ((t->sent | t->acked) == full) t->next = now + EVENT_RETRY_MS;
        dst = id, s->event_turn = (unsigned)id, s->event_next_send = now + EVENT_SEND_MS;
        break;
    }
    coop_unlock(&s->lock);
    if (n) send_sealed(s, (uint8_t)dst, COOP_K_EVENT, bytes, n);
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

// The answers to a CONNECT carry this attempt's nonce. Before this game holds the room's secret they carry no tag at
// all — a host answers a link it has just accepted before that link can prove anything — and after it they carry the
// room's tag, so an answer is one this host could have made.
static int answer_ok(CoopSess *s, const uint8_t *d, size_t n, uint64_t nonce) {
    if (nonce && nonce != s->nonce) {   // (nonce 0: an answer that is not to one attempt, like a removal in a room)
        s->n_dup++, bump(&s->n_lost);
        return 0;
    }
    if (!s->have_secret) {
        if (!coop_dgram_tag_zero(d)) {
            s->n_bad_tag++, bump(&s->n_lost);
            return 0;
        }
        return 1;
    }
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    return 1;
}

// The on_* handlers return 1 for a frame that passed its checks and made sense: only those show the host is there.

// CHALLENGE and ROOM come together as the answer to the first CONNECT of a link: a cookie to carry back, and the
// host's room key.
static int on_challenge(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint64_t nonce;
    uint8_t cookie[COOP_COOKIE_LEN];
    if ((s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) || !coop_challenge_parse(p, h->len, &nonce, cookie)) return 0;
    if (!answer_ok(s, d, n, nonce)) return 0;
    memcpy(s->cookie, cookie, COOP_COOKIE_LEN);
    s->have_cookie = 1;
    s->heard_host = 1;
    if (!s->said_answer) {
        s->said_answer = 1;
        say(s, "the host answered in %u ms", (unsigned)(now - s->last_connect_tx));
        TRACE(s, "the host is at %s", s->host_text);
    }
    if (s->have_secret) send_connect(s, now);   // (already keyed: the cookie alone is what it needs to carry back)
    return 1;
}

// ROOM: the host's secret, which is how a joiner gets into its room. Its next CONNECT carries the keys made from it,
// and only then does the host give out a member id.
static int on_room(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint64_t nonce;
    char secret[COOP_SECRET_LEN + 1];
    if ((s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) || !coop_room_parse(p, h->len, &nonce, secret)) return 0;
    if (nonce != s->nonce || !coop_dgram_tag_zero(d)) {   // (an answer to this attempt, and never one to be forged)
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    memcpy(s->want, secret, sizeof s->want);
    coop_room_keys(s->want, s->locator, s->msg_key);
    s->have_secret = 1;
    s->heard_host = 1;
    s->burst_left = 0;
    s->next_tx = now;   // the CONNECT with the room's keys goes now
    char label[8];
    say(s, "the host is in room %s", label_of(secret, label));
    return 1;
}

static int on_joined(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    if (s->state != COOP_CONNECTING && s->state != COOP_RECONNECTING) return 0;
    CoopJoined j;
    if (!coop_joined_parse(p, h->len, &j) || !h->room || j.member < 1 || j.member > COOP_MAX_MEMBERS || !roster_valid(&j.roster)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    if (!answer_ok(s, d, n, j.nonce)) return 0;   // this room (its locator), this attempt (our nonce)
    const int rejoined = s->state == COOP_RECONNECTING;
    s->room = h->room;
    s->self = j.member;
    memcpy(s->secret, s->want, sizeof s->secret);
    memcpy(s->token, j.token, COOP_COOKIE_LEN);
    s->have_token = 1;
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
    else say(s, "joined room %s as member %u of %d", label, s->self, j.roster.n);
    set_state(s, COOP_JOINED);
    set_status(s, "%s", "");
    s->dirty = 1;
    return 1;
}

// Why the host would not take this player. A host says so in the few words an ERROR carries; a room that ended while
// this player was in it (the host left, or its game closed) is not something a player can wait out, so a session that
// was told to keep its room (HOS_COOP_AUTO) keeps asking for it and the rest leave.
static void refused(CoopSess *s, uint8_t reason, uint64_t now) {
    switch (reason) {
    case COOP_E_NO_ROOM:
        if (s->retry) {
            s->heard_host = 1;
            s->burst_left = 0;
            set_status(s, "that game is not hosting a room; waiting for it");
            if (s->next_tx < now + s->backoff_max_ms) s->next_tx = now + s->backoff_max_ms;
        } else {
            go_left(s, "that game is not hosting a room", 1);
        }
        break;
    case COOP_E_ROOM_FULL: go_left(s, "the room is full", 1); break;
    case COOP_E_PROTO: go_left(s, "update required: that game speaks another co-op version", 1); break;
    case COOP_E_BAD_TAG: go_left(s, "the host does not know this room key", 1); break;
    case COOP_E_REMOVED: go_left(s, "the room's maker removed this player", 1); break;
    case COOP_E_SERVER_FULL: go_left(s, "that game cannot take another player", 1); break;
    case COOP_E_SESSION_USED:   // its own session listed in the room before: a new attempt with a new one
        say(s, "the host saw this session in the room before; trying again with a new one");
        if (begin_episode(s, s->want, s->state == COOP_RECONNECTING, now)) s->next_tx = now + s->backoff_max_ms;
        break;
    default: go_left(s, "the host refused", 1); break;
    }
}

static int on_error(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    const int connecting = s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING;
    CoopError e;
    if (!coop_error_parse(p, h->len, &e)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    // An answer to this attempt carries its nonce; in a room the only ERROR a host sends is a removal, which carries
    // none and is tagged with the room's locator. A removal is what the maker's KICK becomes: the member's link ends
    // and the roster says who is left, so this is the reason the link says rather than a message that has to arrive.
    if (connecting ? !answer_ok(s, d, n, e.nonce)
                   : s->state != COOP_JOINED || e.reason != COOP_E_REMOVED || h->room != s->room || !answer_ok(s, d, n, 0))
        return 0;
    refused(s, e.reason, now);
    return 1;
}

// This player's own line lists another session: someone else's CONNECT reached this host with this player's token
// (its address read on the way, say). Join again to take the place back, at most once per backoff_max_ms; a later one
// waits for its turn (timers).
static void roster_rejoin(CoopSess *s, uint64_t now) {
    if (s->roster_rejoin_at && now - s->roster_rejoin_at < s->backoff_max_ms) return;
    s->roster_rejoin_at = now ? now : 1;
    say(s, "the host lists another session for this player; joining again");
    begin_episode(s, s->want, 1, now);
}

// The host's roster replaces ours; the log tells who came, went, changed name or went away. 1 when this player's own
// line lists another session than its own: someone reached the host with this player's token, and the session joins
// again to take the place back (roster_rejoin).
static int apply_roster(CoopSess *s, const CoopRoster *r) {
    int taken = 0;
    for (int i = 0; i < r->n; i++) {
        const CoopMemberInfo *m = &r->m[i], *old = member_of(s, m->id);
        if (m->id == s->self) {   // this session's own line: the host's maker mark is not taken for it (start_hosting)
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

static int on_roster(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    CoopRoster r;
    if (s->hosting || s->state != COOP_JOINED || h->room != s->room) return 0;
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    if (!coop_roster_parse(p, h->len, &r, NULL) || !roster_valid(&r)) {
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
    // A later roster, up to ROSTER_AHEAD past the last one: a player that recorded one and sends it again, with a
    // number far ahead, would make every roster the host sends after it look old. An older one arriving late is
    // dropped, unless no roster was taken for a while (the host sends one every few seconds): then the last one's
    // number was not the host's.
    const int32_t ahead = (int32_t)(h->seq - s->roster_seq);
    const int stale = now - s->roster_at >= (uint64_t)s->silence_ms + ROSTER_STALE_MS;
    if ((ahead > 0 && ahead <= ROSTER_AHEAD) || (stale && ahead)) {
        s->roster_seq = h->seq, s->roster_at = now;
        s->own_taken = apply_roster(s, &r);
        if (s->own_taken) roster_rejoin(s, now);
    }
    return 1;
}

static int on_pong(CoopSess *s, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    uint32_t token;
    if (s->hosting || s->state != COOP_JOINED || h->room != s->room || !coop_u32_parse(p, h->len, &token)) return 0;
    if (!coop_dgram_verify(d, n, s->locator)) {
        s->n_bad_tag++, bump(&s->n_lost);
        return 0;
    }
    const uint32_t rtt = (uint32_t)now - token;
    if (rtt <= 60000) {   // a token of ours
        coop_lock(&s->lock);
        s->view.ping_ms = rtt ? rtt : 1;
        coop_unlock(&s->lock);
    }
    return 1;
}

// A player message, from either side: the room's, of a session the roster names for its sender, once.
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
    TRACE(s, "rx message kind %u from member %u, %u bytes", kind, h->src, (unsigned)nb);
    if (kind == COOP_K_EVENT || kind == COOP_K_EVENT_ACK)
        return h->dst == s->self && event_receive(s, h->src, session, kind, bytes, nb, now);
    CoopMsgFn fn = __atomic_load_n(&s->msg[kind].fn, __ATOMIC_ACQUIRE);
    if (fn) fn(h->src, bytes, nb, s->msg[kind].user);
    return 1;
}

// (hosting) The frame goes on to every other member it is for, as it came: the host carries what it cannot read.
static void forward_frame(CoopSess *s, int from_member, const uint8_t *frame, size_t n) {
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        if (id == from_member || !s->slot_of[id]) continue;
        CoopTcp *t = coop_tcp_at(s->link, s->slot_of[id]);
        if (t && coop_tcp_send(t, frame, n)) bump(&s->n_tx);
    }
}

// (hosting) A room is a name, a roster and a port: what a player may say on its link is a CONNECT (who I am), a DATA
// (what I have to say, which goes on to the others), a PING, and LEAVE. Everything else a member sends is out of its
// place: only the host says who is in the room, and a member cannot make a JOINED, a ROSTER or a ROOM for itself.
static int host_frame(CoopSess *s, int slot, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, uint64_t now) {
    const int member = s->who[slot];
    if (!member) {   // a link that is still handshaking
        if (h->type != COOP_T_CONNECT) return 0;
        CoopConnect c;
        if (!coop_connect_parse(p, h->len, &c)) {
            s->n_malformed++, bump(&s->n_lost);
            return 0;
        }
        if (c.intent != COOP_INTENT_JOIN) return 0;
        int at = -1;
        for (int i = 0; i < (int)(sizeof s->pend / sizeof s->pend[0]); i++)
            if (s->pend[i].slot == slot) at = i;
        if (at < 0) {   // its first CONNECT: here is the cookie to carry back, and the room
            if (pending_count(s) >= (int)(sizeof s->pend / sizeof s->pend[0])) {
                refuse_link(s, slot, COOP_E_SERVER_FULL, "too many players are being let in at once", c.nonce);
                return 0;
            }
            at = pending_add(s, slot, c.nonce, now);
            if (at < 0) {
                refuse_link(s, slot, COOP_E_SERVER_FULL, "this game cannot make random numbers: co-op needs them", c.nonce);
                return 0;
            }
            answer_connect(s, slot, c.nonce, s->pend[at].cookie);
            return 1;
        }
        if (c.nonce != s->pend[at].nonce) return 0;   // not the attempt this link is answering
        // What proves the link belongs to this room: the keys made from its secret, on the frame and in the CONNECT,
        // and either the cookie this link was given or the rejoin token of the member it used to be. A CONNECT of the
        // burst that carries neither yet is answered again, not refused: the CHALLENGE and ROOM it waits for may be
        // the ones that went missing.
        const int keys = coop_dgram_verify(d, n, s->locator) && !memcmp(c.locator, s->locator, COOP_KEY_LEN);
        const int member_id = keys ? gone_of(s, c.token, now) : 0;
        const int proved = (keys && c.cookie[0] && !memcmp(c.cookie, s->pend[at].cookie, COOP_COOKIE_LEN)) || member_id;
        if (!proved) {
            if (c.token[0] && !member_id) {   // a member id that is no one's here
                say(s, "refused a player: it claims a member id that is not this room's");
                refuse_link(s, slot, COOP_E_BAD_TAG, "that member id is not this room's", c.nonce);
                return 0;
            }
            // Not a refusal: the CHALLENGE and ROOM this CONNECT is answering may be the ones that went missing, and a
            // CONNECT of the burst can reach the host before its own ROOM does. Whoever reaches this port is told the
            // room's secret anyway (docs/coop.md), so answering again gives away nothing.
            answer_connect(s, slot, c.nonce, s->pend[at].cookie);
            return 1;
        }
        take_member(s, slot, &c, now);
        return 1;
    }
    switch (h->type) {
    case COOP_T_CONNECT: {   // who I am, again: the Co-op tab changed this player's name
        CoopConnect c;
        const CoopMemberInfo *m = member_of(s, member);
        if (!coop_connect_parse(p, h->len, &c) || !m || c.session != m->session || !coop_dgram_verify(d, n, s->locator)) {
            s->n_sender++, bump(&s->n_lost);
            return 0;
        }
        if (!strcmp(c.name, m->name)) return 1;
        CoopRoster r = s->roster;
        for (int i = 0; i < r.n; i++)
            if (r.m[i].id == member) copy_str(r.m[i].name, sizeof r.m[i].name, c.name);
        publish_rooms(s, &r);
        return 1;
    }
    case COOP_T_DATA:
        if (h->src != member) {   // a member cannot speak as another
            s->n_sender++, bump(&s->n_lost);
            SAY_LIMITED(s, L_SENDER, now, "member %d sent a message as member %u: thrown away", member, h->src);
            return 0;
        }
        if (h->room == s->room && (h->dst == COOP_DST_ALL || member_of(s, h->dst))) forward_frame(s, member, d, n);
        return on_data(s, d, n, h, p, now);
    case COOP_T_PING: {
        uint8_t token[4];
        size_t k = coop_u32_pack(token, sizeof token, (uint32_t)now);
        if (k) send_slot(s, slot, COOP_T_PONG, s->room, (uint8_t)s->self, (uint8_t)member, token, k, s->locator);
        return 1;
    }
    case COOP_T_LEAVE:
        end_link(s, slot, "left the room");
        return 1;
    default:
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
}

// What a link says, from either side: the host's own room, or the host's answers on the one link this game has.
static void handle_frame(CoopSess *s, int slot, size_t n, uint64_t now) {
    if (s->drop_pct && rnd(s) % 100 < s->drop_pct) {
        s->n_test_drop++;
        return;
    }
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(s->rx, n, &h, &p)) {
        s->n_malformed++, bump(&s->n_lost);
        SAY_LIMITED(s, L_MALFORMED, now, "a frame that is not a co-op frame was thrown away, and the link with it");
        end_link(s, slot, "sent something that is not a co-op frame");
        return;
    }
    if (h.ver != COOP_PROTO) {
        // A game of another co-op version: it answers a CONNECT with an ERROR it cannot word in our format, taken only
        // as the first answer of an attempt so that a stray frame does not end a session.
        if (!s->hosting && h.type == COOP_T_ERROR && coop_dgram_tag_zero(s->rx) && !s->heard_host &&
            (s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING)) {
            bump(&s->n_rx);
            refused(s, COOP_E_PROTO, now);
        } else {
            s->n_sender++, bump(&s->n_lost);
            end_link(s, slot, "speaks another co-op version");
        }
        return;
    }
    TRACE(s, "rx %s from link %d, %u bytes", type_name(h.type), slot, (unsigned)n);
    int ok;
    if (s->hosting) ok = host_frame(s, slot, s->rx, n, &h, p, now);
    else switch (h.type) {
    case COOP_T_CHALLENGE: ok = on_challenge(s, s->rx, n, &h, p, now); break;
    case COOP_T_ROOM: ok = on_room(s, s->rx, n, &h, p, now); break;
    case COOP_T_JOINED: ok = on_joined(s, s->rx, n, &h, p, now); break;
    case COOP_T_ERROR: ok = on_error(s, s->rx, n, &h, p, now); break;
    case COOP_T_ROSTER: ok = on_roster(s, s->rx, n, &h, p, now); break;
    case COOP_T_PONG: ok = on_pong(s, s->rx, n, &h, p, now); break;
    case COOP_T_DATA: ok = on_data(s, s->rx, n, &h, p, now); break;
    default:
        s->n_malformed++, bump(&s->n_lost);
        ok = 0;
        break;
    }
    if (ok) {
        bump(&s->n_rx);
        if (s->hosting) {   // the host is the one that knows who is still answering: every member's frame is heard here
            const int member = s->who[slot];
            if (member) {
                coop_lock(&s->lock);
                s->heard_at[member] = now;
                coop_unlock(&s->lock);
            }
        }
        s->last_rx = now;   // a frame the host made: the host is there
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
    if (kind == COOP_K_EVENT || kind == COOP_K_EVENT_ACK ||
        !__atomic_load_n(&s->pub_joined, __ATOMIC_ACQUIRE) || n > COOP_MAX_MESSAGE || member < 0 ||
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
    uint16_t port = s->pending.port;
    s->drop_pct = s->pending.drop_pct;
    s->retry = s->pending.retry;
    int on = s->pending.on;
    coop_unlock(&s->lock);
    if (strcmp(server, s->server)) {
        copy_str(s->server, sizeof s->server, server);
        s->host[0] = 0;
        s->host_known = 0;
        if (s->server[0] && !coop_server_parse(s->server, s->host, sizeof s->host, &s->host_port))
            say(s, "the coop_server setting is not host or host:port");
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) {
            say_goodbye(s);
            go_left(s, "the host changed", 1);
        } else if (s->state == COOP_CONNECTING) {
            go_left(s, "the host changed", 0);
        }
    }
    if (port != s->port) {
        s->port = port;
        if (s->hosting) {   // the same room, said on the new port: whoever is in it has to connect again
            say(s, "hosting on port %u", (unsigned)port);
            close_link(s);
            if (!ensure_link(s, now)) go_left(s, s->status[0] ? s->status : "cannot listen", 0);
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
        close_link(s);
        s->hosting = 0;
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
    case COOP_CMD_HOST:
        // This game is the room: its own port, a secret of its own unless one was typed (the same room again after a
        // restart, with the code the others were given).
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) say_goodbye(s);
        close_link(s);
        s->hosting = 0;
        char own[COOP_SECRET_LEN + 1];
        if (secret && secret[0]) {
            copy_str(own, sizeof own, secret);
        } else {
            uint8_t r[32];
            if (!coop_random(r, sizeof r) || !coop_secret_from_random(r, own)) {
                go_left(s, "the system's random source failed: co-op needs it", 1);
                break;
            }
        }
        if (!start_hosting(s, own, now)) break;
        break;
    case COOP_CMD_JOIN:
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING) say_goodbye(s);
        if (!s->host[0]) {
            SAY_LIMITED(s, L_NO_SERVER, now, "no host address given (coop_server= in hookshot_options.ini or HOS_COOP_SERVER)");
            go_left(s, "no host address configured", 0);
            break;
        }
        // The host's address is enough: it says its room's secret on the link. A secret typed as well proves this
        // player was told it some other way, which is what a rejoin runs on.
        begin_episode(s, secret && secret[0] ? secret : "", 0, now);
        break;
    }
}

// The room's maker removes a member: its link gets the ERROR that says why, then closes, and the roster says who is
// left. The member's session is the one the published view listed when the maker removed it (progress sync removed that
// member's key at that moment), so a member that rejoined since, or another player given the id, is not the one kicked.
static void do_remove(CoopSess *s, int member, uint64_t session) {
    const CoopMemberInfo *m = member_of(s, member);
    if (!s->hosting || s->state != COOP_JOINED || !m || member == s->self) {
        say(s, "only the room's maker can remove a member, and only one that is in the room");
        return;
    }
    if (session && m->session != session) return;   // (whoever holds that id now is not the one it was asked about)
    const int slot = s->slot_of[member];
    uint8_t payload[128];
    size_t k = 0;
    if (slot) {
        CoopError e = {.reason = COOP_E_REMOVED, .nonce = 0};
        copy_str(e.text, sizeof e.text, "the room's maker removed you");
        k = coop_error_pack(payload, sizeof payload, &e);
        if (k) send_slot(s, slot, COOP_T_ERROR, s->room, (uint8_t)s->self, (uint8_t)member, payload, k, s->locator);
    }
    say(s, "removing %s (member %d) from the room", m->name, member);
    close_said_link(s, slot, "removed by the room's maker");
}

// ---- the step

// (hosting) Who has gone quiet, said as a roster; a link that was accepted and never became a member is let go. The
// host hears every member's frames on its own links, so it is the one that knows who is still there.
static void host_timers(CoopSess *s, uint64_t now) {
    for (int i = 0; i < (int)(sizeof s->pend / sizeof s->pend[0]); i++) {
        const PendingLink p = s->pend[i];
        if (!p.slot || now - p.since < HANDSHAKE_MS) continue;
        say(s, "a player that never finished joining was let go");
        end_link(s, p.slot, "its handshake went nowhere");
    }
    if (s->rename_at && now >= s->rename_at) {   // the name changed: it is one of the roster's lines
        s->rename_at = 0;
        publish_roster(s);
    }
    for (int i = 0; i < s->roster.n; i++) {
        const CoopMemberInfo *m = &s->roster.m[i];
        if (m->id == s->self) continue;
        const uint64_t at = s->heard_at[m->id];
        const int away = !at || now - at >= AWAY_MS;
        if (away == (m->flags & COOP_FLAG_AWAY ? 1 : 0)) continue;
        CoopRoster r = s->roster;
        for (int k = 0; k < r.n; k++)
            if (r.m[k].id == m->id) r.m[k].flags = (uint8_t)((r.m[k].flags & ~COOP_FLAG_AWAY) | (away ? COOP_FLAG_AWAY : 0));
        publish_rooms(s, &r);   // (apply_roster logs who went away, or came back)
        break;   // (the roster was re-numbered: the rest is looked at again next step)
    }
    if (now >= s->next_roster) publish_roster(s);
}

static void stats_line(CoopSess *s, uint64_t now) {
    s->next_stats = now + STATS_MS;
    const uint32_t seen = s->n_rx + s->n_tx + s->n_lost + s->n_test_drop;
    if (seen == s->stats_seen) return;
    s->stats_seen = seen;
    coop_lock(&s->lock);
    const uint32_t ping = s->view.ping_ms;
    coop_unlock(&s->lock);
    say(s, "stats: sent %u, accepted %u, thrown away %u (tag %u, sender %u, not co-op %u, replay %u), dropped by the test %u, outbox full %u, round trip %u ms",
        s->n_tx, s->n_rx, s->n_lost, s->n_bad_tag, s->n_sender, s->n_malformed, s->n_dup, s->n_test_drop,
        __atomic_load_n(&s->n_outbox_full, __ATOMIC_RELAXED), ping);
}

// The host's link is gone, or the host stopped answering: the same player joins the same room again (the rejoin token
// keeps its member id for a minute).
static void lost_host(CoopSess *s, uint64_t now) {
    say(s, "the host is gone; rejoining");
    begin_episode(s, s->want, 1, now);
}

static void timers(CoopSess *s, uint64_t now) {
    switch (s->state) {
    case COOP_CONNECTING:
    case COOP_RECONNECTING:
        connect_timers(s, now);
        break;
    case COOP_JOINED:
        if (s->hosting) {
            host_timers(s, now);
            if (now >= s->next_stats) stats_line(s, now);
            break;
        }
        if (!coop_tcp_alive(coop_tcp_at(s->link, s->own_slot)) || now - s->last_rx >= s->silence_ms) {
            lost_host(s, now);
            break;
        }
        if (s->rename_at && now >= s->rename_at) {   // the name changed: the host learns it with a new CONNECT
            s->rename_at = 0;
            say(s, "joining the room again as %s", s->name);
            begin_episode(s, s->want, 1, now);
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
        if (now >= s->next_stats) stats_line(s, now);
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

// A link that knocked on the port this game hosts on: it becomes a handshaking link, or gets the reason it cannot be
// one before it closes. The room holds eight players, this game one of them.
static void take_knock(CoopSess *s, uint64_t now) {
    CoopAddr from;
    CoopTcp *t = coop_tcp_accept(s->listener, &from);
    if (!t) return;
    const int slot = coop_tcp_add(s->link, t);
    if (!slot) {
        coop_tcp_close(t);
        return;
    }
    char text[64];
    coop_addr_str(&from, text, sizeof text);
    TRACE(s, "a player connected from %s", text);
    int members = 0;
    for (int id = 2; id <= COOP_MAX_MEMBERS; id++) members += s->slot_of[id] != 0;
    if (members + pending_count(s) >= COOP_MAX_MEMBERS) {
        say(s, "refused a player from %s: the room is full", text);
        refuse_link(s, slot, COOP_E_ROOM_FULL, "the room is full", 0);
        return;
    }
    if (pending_count(s) >= (int)(sizeof s->pend / sizeof s->pend[0]))
        refuse_link(s, slot, COOP_E_RATE, "too many players are being let in at once", 0);
    (void)now;
}

// A link the other end closed without saying so: whoever it carried leaves the room the same way.
static void reap_links(CoopSess *s) {
    if (!s->link) return;
    for (int slot = 1; slot <= LINK_SLOTS; slot++)
        if (!coop_tcp_alive(coop_tcp_at(s->link, slot))) end_link(s, slot, "the link closed");
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
    publish(s);   // what a command or a setting changed shows before this thread waits for a frame
    uint32_t wait = stopped ? 0 : s->state == COOP_JOINED ? 8 : s->state == COOP_CONNECTING || s->state == COOP_RECONNECTING ? 50 : 250;
    if (s->link) {
        for (int i = 0; i < 64; i++) {
            int slot = 0;
            const int n = coop_tcp_next(s->link, s->rx, sizeof s->rx, &slot, i ? 0 : wait);
            if (n < 0) {   // (the port this game hosts on has someone knocking)
                take_knock(s, coop_now_ms());
                continue;
            }
            if (n == 0) break;
            handle_frame(s, slot, (size_t)n, coop_now_ms());
            if (!s->link) break;   // a handler ended it
        }
        if (s->link) reap_links(s);
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
    events_tick(s, coop_now_ms());
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
    close_link(s);
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
    s->pending.port = p->port;
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
    if (cmd != COOP_CMD_HOST && cmd != COOP_CMD_JOIN && cmd != COOP_CMD_LEAVE) return 0;
    if (secret && *secret && !coop_secret_normalize(secret, canon)) return 0;   // HOST: the room to host; JOIN: one this player was told
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
    if (kind == COOP_K_EVENT || kind == COOP_K_EVENT_ACK) return 0;
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
    if (g_sess) g_sess->process_session = 1;
    if (!g_sess) hos_log("coop: out of memory");
}

static CoopSess *process_session(void) {
    coop_once(&g_once, make_session, NULL);
    return g_sess;
}

// The player's id towards the host: random for each start of the game, so that nothing ties one session to another,
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
// co-op on, coop_name= is the player's name (empty: "Player-" and four random hex digits), coop_server= the host to
// join ("host", "host:port" or "[v6]:port"; empty: this game hosts on its own port), coop_port= the port this game
// hosts on (0: 43400). The environment wins over each: HOS_COOP, HOS_COOP_NAME, HOS_COOP_SERVER, HOS_COOP_PORT. Off, no
// thread is started and nothing else runs. For tests: HOS_COOP_AUTO=host|join with HOS_COOP_ROOM, a room at start (and
// LAN parties; a host that went away is joined again), and HOS_COOP_DROP, a test's share of the received frames thrown
// away (percent). The host's address reaches the log only with HOS_COOP_TRACE. No HOS_COOP name is
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

// The settings into g_set, and the lines that say what they are (never the room's secret).
// menu: the name and host address the menu gives (coop_configure), over the environment's and the options file's; NULL: those.
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
    const long p = port ? strtol(port, NULL, 10) : hos_ini_option("coop_port", DEFAULT_PORT);
    g_set.port = p > 0 && p <= 65535 ? (uint16_t)p : DEFAULT_PORT;
    if (p <= 0 || p > 65535) hos_log("coop: the port (HOS_COOP_PORT or coop_port=) is not 1 to 65535: %u is used", (unsigned)DEFAULT_PORT);
    const char *drop = getenv("HOS_COOP_DROP");
    g_set.drop = drop ? (unsigned)strtoul(drop, NULL, 10) : 0;
    if (g_set.server[0])
        hos_log("coop: on, as %s; joining %s (%s)", g_set.name, g_set.server, from ? from : "the menu");
    else
        hos_log("coop: on, as %s; hosting on port %u until a host is given", g_set.name, (unsigned)g_set.port);
    if (g_set.drop) hos_log("coop: HOS_COOP_DROP: %u%% of the received frames are thrown away (a test)", g_set.drop > 100 ? 100 : g_set.drop);
    const char *mode = getenv("HOS_COOP_AUTO"), *room = getenv("HOS_COOP_ROOM");
    if (!mode || !*mode || !strcmp(mode, "off")) return;
    if (strcmp(mode, "host") && strcmp(mode, "join")) {
        hos_log("coop: HOS_COOP_AUTO must be off, host or join");
        return;
    }
    g_set.join = !strcmp(mode, "join");
    if (room && *room && !coop_secret_normalize(room, g_set.room)) {
        hos_log("coop: HOS_COOP_ROOM is not a room secret (18 characters, XXXXXX-XXXXXX-XXXXXX)");
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
                    .retry = g_set.automatic, .port = g_set.port, .drop_pct = g_set.drop};
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
        coop_sess_command(s, g_set.join ? COOP_CMD_JOIN : COOP_CMD_HOST, g_set.room);
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
    } else if (on) {   // (off at start: the settings are read now, with the menu's name and host address)
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
    int kicked = v.made && coop_sess_remove(s, member);   // only the game that hosts the room can end a link
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

int coop_send_event(int member, uint8_t kind, const void *bytes, size_t n) {
    CoopSess *s = process_session();
    return s ? coop_sess_send_event(s, member, kind, bytes, n) : COOP_EVENT_UNAVAILABLE;
}

int coop_on_event(uint8_t kind, CoopEventFn fn, void *user) {
    CoopSess *s = process_session();
    return s && coop_sess_on_event(s, kind, fn, user);
}

int coop_member_accepted(int member) {
    CoopSess *s = process_session();
    return s && coop_sess_member_accepted(s, member);
}

void coop_accept_members(const CoopView *view, uint32_t members) {
    CoopSess *s = process_session();
    if (s) coop_sess_accept_members(s, view, members);
}
