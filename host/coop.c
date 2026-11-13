// Co-op session layer (coop.h): one player's game hosts, the others are joined to that game, and the hosting game
// passes every message on. It never touches game state: progress sync and the other players' puppets are layers above
// this one, which send with coop_send and receive with coop_on_message. The game's own nn::socket stays offline.
//
// There is one way in: the host's address and port. No relay to run, no room to name, nothing to type but that
// address. The room is the host's listening socket, and it is gone when the host's game closes.
//
// States: OFF (co-op is off: HOS_COOP or the options file's coop= did not switch it on, and no Host or Join was asked
// for), LEFT (co-op is on, in no room: the status says why), CONNECTING (hosting and the port could not be bound, or
// reaching the host's address), JOINED (in a room: hosting, or joined to one), RECONNECTING (the host went silent;
// reaching it again).
//
// The host is member 1 and the room's maker: it gives every joiner its member id, keeps the roster, and stamps the
// sender's id into every player message it forwards, so a member cannot claim to be another one or make one of the
// host's. A joiner sends HELLO (its name and a session drawn for the join), is answered WELCOME (its id, the room's
// id, the roster) and then takes ROSTER frames and forwarded DATA frames from the host. A join the host refuses
// (its room is full, or the protocol major differs) is answered ERROR and the link closes. There is no encryption on
// this link: whoever can reach the host's port can join, and what a player's save may be told is protected by what
// that player's own key signs (coop_proto.h, coop_state.c), not by the link.
//
// A player message carries its sender's session and a counter from 1 (coop_proto.h): the others take a message only of
// the session the roster lists for its sender and once within a window of 64 counters, so a message recorded before a
// rejoin, or sent again within the window, is refused. Four older ones in a row start the window again, after which a
// recorded message of that session can come again: a layer that must take a message once keeps a counter of its own,
// signed with its key, which only that player can move.
//
// Hosting answers a join with an id at once, so a connection that says nothing holds a place until HELLO_WAIT_MS has
// passed with nothing from it. A member the host has not heard from for AWAY_MS is marked away in the roster (its link
// stays); one silent for SILENCE_MS is dropped. A joined game sends PING every ping_ms, which keeps the path warm and
// measures the round trip, and SILENCE_MS with nothing from the host is RECONNECTING: the same attempt again, with a
// new session and therefore a new member id, and a backoff of 1, 2, 4, 8, then 15 s.
//
// One thread owns the links and the state. Other threads reach it through a mailbox (commands, settings), an outbox
// (messages to send) and a copy of the state they may read; all three sit behind one lock that the thread holds only
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

// (coop_tcp_frame reads a frame's length out of its header, so the link layer's idea of the header and of the largest
//  frame must be the wire format's. tests/test_coop_session.py checks the framing itself, split frames and all.)
_Static_assert((int)COOP_TCP_HDR == (int)COOP_FRAME_HDR, "host/coop_net.h's frame header is not host/coop_proto.h's");
_Static_assert((int)COOP_TCP_MAX_FRAME == (int)COOP_MAX_FRAME, "host/coop_net.h's largest frame is not host/coop_proto.h's");

void hos_log(const char *fmt, ...);

enum {
    PING_MS = 2000,                 // joined: a PING this often, which keeps the path warm and measures the round trip
    SILENCE_MS = 8000,              // joined: this long with nothing from the host is RECONNECTING; hosting: a member
                                    // that silent is dropped
    AWAY_MS = 5000,                 // hosting: a member this long unheard is marked away in the roster
    HELLO_WAIT_MS = 3000,           // hosting: a connection that has not said who it is by then is closed
    ROSTER_EVERY_MS = 5000,         // hosting: the roster again this often, whatever changed
    COOP_MAX_WAIT = 4,              // hosting: joins that may wait to say who they are
    CONNECT_MS = 2000,              // joined: how long a link to the host may take to open
    DEFAULT_PORT = 43400,           // the port hosted on, and joined when the address gives none
    BACKOFF_FIRST_MS = 1000, BACKOFF_MAX_MS = 15000,
    RENAME_MS = 1000,               // a name change is told to the room after this much quiet
    STATS_MS = 60000,               // the periodic counters line
    LOG_EVERY_MS = 30000,           // a repeated cause logs this often
    OUT_SLOTS = 64,                 // messages waiting to be sent
    VIEW_HOOKS = 8,
    TICK_HOOKS = 4,
    WINDOW_AGAIN = 4,               // older messages in a row that start a sender's replay window again
    L_MALFORMED, L_SENDER, L_REPLAY, L_OUTBOX, L_NO_HOST, L_RESOLVE, L_SOCKET, L_PORT, L_FULL, L_CAUSES,
};

typedef struct { uint8_t dst, kind; uint16_t len; uint8_t data[COOP_MAX_MESSAGE]; } OutMsg;
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

// The counters of one sender's session seen last: a counter at most 64 below the highest is taken once, an older one
// is not, unless the sender's own messages keep arriving below the window (window_below). A new session (the roster
// names it) starts a new window. low and low_n: the last counter that opened below it, and how many did in a row.
typedef struct { uint64_t session; uint32_t high, low; uint64_t bits; int valid, low_n; } Window;

struct CoopSess {
    // ---- settings, set by coop_sess_new and coop_sess_configure
    char tag[16];
    uint32_t ping_ms, silence_ms, backoff_first_ms, backoff_max_ms;
    // what the stepping thread works with now
    char server[160], name[COOP_NAME_MAX + 1], host[128];   // server: the host's address as given, host: its host part
    uint16_t port, typed_port;            // hosted on, or joined to (DEFAULT_PORT when nothing says otherwise);
                                        // typed_port: the port that came with the host's address, when one did
    uint64_t client_id;
    unsigned drop_pct;
    int retry;

    // ---- shared, behind lock
    CoopLock lock;
    CoopEvent off_done;
    CoopEvent wake;                       // a command or settings were queued: a session with no link to wait on takes them now
    int cmd;                              // a command waiting (COOP_CMD_HOST / COOP_CMD_JOIN / COOP_CMD_LEAVE), 0 none
    char cmd_text[160];                   // JOIN: the host's address as typed
    int cmd_remove;                       // a member the host removes, 0 none, and its session in the view then
    uint64_t cmd_remove_session;
    int cfg_pending;                      // pending holds settings the thread has not applied
    struct { char server[160], name[COOP_NAME_MAX + 1], why[48]; uint64_t client_id; uint16_t port; unsigned drop_pct;
             int retry, on, host_wanted; } pending;   // host_wanted: Host was asked for at the start of co-op
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
    uint64_t accepted_epoch[COOP_MAX_MEMBERS + 1];   // revocations remembered even across revoke/reaccept between steps
    EventHandler event_handler[256];
    EventOut event_out[COOP_EVENT_SLOTS];
    uint64_t event_id;                          // allocated under lock, never reused in a session
    // Incoming reassembly and pacing are used only by the stepping thread.
    EventIn event_in[COOP_MAX_MEMBERS + 1];
    uint64_t event_next_send;
    unsigned event_turn;

    // ---- the stepping thread's
    CoopState state;
    int hosting;                          // 1: this game holds the room (its own listening socket); 0: joined to server
    int want_role;                        // 0 nothing asked for yet, 1 host the room, 2 join the one at server: what the
                                        // timers keep reaching for
    int process_session;
    CoopTcp *listener;                    // hosting: the room's door
    CoopTcp *link[COOP_MAX_MEMBERS + 1];  // hosting: member id (2..8) to its link; joined: link[1] is the host's
    CoopTcp *wait[COOP_MAX_WAIT];         // hosting: connections that have not said who they are yet
    uint64_t wait_at[COOP_MAX_WAIT];      // ... and the time by which they must
    char wait_text[COOP_MAX_WAIT][64];    // ... and the address they came from, for the log
    uint8_t taken[COOP_MAX_MEMBERS + 1];  // hosting: an id that was given and is not given again while the room lasts
    uint64_t member_session[COOP_MAX_MEMBERS + 1];      // hosting: what each member said its session is
    char member_name[COOP_MAX_MEMBERS + 1][COOP_NAME_MAX + 1];
    uint64_t session;                     // this join's session (in the roster), drawn again for every join
    uint32_t out_counter;                 // this join's envelope counter, from 1
    int self;                             // this player's member id (1 when hosting)
    uint8_t room_id[COOP_ROOM_ID_LEN];    // the host's id for its room; a joiner takes the host's
    char addr[64];                        // hosting: the address a joiner can type, when one stands out
    CoopAddr target;                      // joined: the host's address
    char target_text[64];                 // ... and its text, for the trace
    int target_ok;                        // the host's name has been resolved into target
    int made_room;                        // this session holds the room (its own HOST command): its settings are the rules
    CoopRoster roster;                    // hosting: as the host holds it; joined: the last roster the host sent
    Window rw[COOP_MAX_MEMBERS + 1];
    char status[96];
    int dirty;
    int off_applied;                      // a stop was applied: off_done is set once the view saying so is published
    uint64_t rng;
    uint64_t log_at[L_CAUSES];
    uint32_t n_tx, n_rx, n_lost;          // (read by other threads for the view: written with bump)
    uint32_t n_malformed, n_sender, n_dup, n_test_drop, n_outbox_full;
    uint32_t stats_seen;
    uint64_t last_rx, next_ping, next_stats, next_roster, rename_at, next_try, roster_seq;
    uint32_t backoff;
    uint8_t rx[COOP_MAX_FRAME + 8];       // one frame at a time, from whichever link was read
    int rx_link;                          // ... and which link it came from
};

// ---- logging

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
    case COOP_F_HELLO: return "HELLO";
    case COOP_F_WELCOME: return "WELCOME";
    case COOP_F_ROSTER: return "ROSTER";
    case COOP_F_DATA: return "DATA";
    case COOP_F_PING: return "PING";
    case COOP_F_PONG: return "PONG";
    case COOP_F_BYE: return "BYE";
    case COOP_F_KICK: return "KICK";
    case COOP_F_ERROR: return "ERROR";
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
// since a session or nonce someone could guess would let them answer as the host or take this player's place.
static uint64_t random64(void) {
    uint64_t v = 0;
    if (!coop_random(&v, sizeof v)) return 0;
    return v ? v : 1;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    snprintf(dst, cap, "%s", src ? src : "");
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

// ---- the links: the host's door, the joins, and what travels on them

static void go_left(CoopSess *s, const char *why, int log_it);
static void events_reset(CoopSess *s);
static void deliver_message(CoopSess *s, int from, uint64_t session, uint8_t kind,
                            const uint8_t *bytes, size_t n, uint64_t now);
static void forward_data(CoopSess *s, int from, uint8_t dst, const uint8_t *p, uint32_t n, uint64_t now);
static void take_message(CoopSess *s, int from, const uint8_t *p, uint32_t n, uint64_t now);
static void lost_host(CoopSess *s, uint64_t now);
static void roster_rejoin(CoopSess *s, uint64_t now);
static void sweep_links(CoopSess *s, uint64_t now);

static void drop_member(CoopSess *s, int id, const char *why);

static void close_link(CoopSess *s, int id) {
    if (id < 1 || id > COOP_MAX_MEMBERS || !s->link[id]) return;
    coop_tcp_close(s->link[id]);
    s->link[id] = NULL;
}

static void close_waiting(CoopSess *s, int slot) {
    if (slot < 0 || slot >= COOP_MAX_WAIT || !s->wait[slot]) return;
    coop_tcp_close(s->wait[slot]);
    s->wait[slot] = NULL;
    s->wait_at[slot] = 0;
}

static void close_links(CoopSess *s) {
    if (s->listener) {
        coop_tcp_close(s->listener);
        s->listener = NULL;
    }
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) close_link(s, id);
    for (int i = 0; i < COOP_MAX_WAIT; i++) close_waiting(s, i);
}

// The link a frame for `dst` goes out on: hosting, that member's own link; joined, the host's link (the frame says
// which member it is for, and the host routes it). NULL when there is no such link.
static CoopTcp *route(CoopSess *s, int dst) {
    if (s->hosting) return dst >= 1 && dst <= COOP_MAX_MEMBERS ? s->link[dst] : NULL;
    return s->link[1];
}

// One frame out. COOP_DST_ALL while hosting: every member's link; while joined, the host's link once, with dst left in
// the frame for the host to route. `src` is this session's own member id (0 for a host's frame of its own).
static int send_frame(CoopSess *s, int dst, uint8_t type, uint8_t kind, const void *payload, size_t n, int src) {
    uint8_t frame[COOP_MAX_FRAME];
    CoopFrame f = {type, (uint8_t)src, (uint8_t)dst, kind, 0};
    const size_t k = coop_frame_pack(frame, sizeof frame, &f, payload, n);
    if (!k) return 0;
    int sent = 0;
    if (s->hosting && dst == COOP_DST_ALL) {
        for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
            if (!s->link[id]) continue;
            if (coop_tcp_send(s->link[id], frame, k)) sent = 1;
            else bump(&s->n_lost);
        }
    } else {
        CoopTcp *t = route(s, dst);
        if (t && coop_tcp_send(t, frame, k)) sent = 1;
    }
    if (sent) bump(&s->n_tx);
    TRACE(s, "tx %s to %d, %u bytes", type_name(type), dst, (unsigned)n);
    return sent;
}

// A player message: this join's session and the next counter, then the kind and the bytes (coop_proto.h). Hosting, the
// host stamps its own member id when it forwards a member's message; its own messages carry id 1.
static void send_message(CoopSess *s, uint8_t dst, uint8_t kind, const void *bytes, size_t n) {
    if (s->out_counter == UINT32_MAX) {   // the counter is this join's: a new join starts a new one
        go_left(s, "message counter exhausted; join the room again", 1);
        return;
    }
    uint8_t payload[COOP_MAX_PAYLOAD];
    const size_t k = coop_msg_pack(payload, sizeof payload, s->session, ++s->out_counter, kind, bytes, n);
    if (k) send_frame(s, dst, COOP_F_DATA, kind, payload, k, s->self);
}

// ---- the roster, and what the room looks like from the host

// hosting: this game's own line in the roster (id 1, the maker), then every member that said who it is, by id.
static void host_roster(CoopSess *s, CoopRoster *r, uint64_t now) {
    r->n = 0;
    CoopMemberInfo *self = &r->m[r->n++];
    self->id = 1, self->flags = COOP_FLAG_MAKER, self->session = s->session;
    copy_str(self->name, sizeof self->name, s->name);
    for (int id = 2; id <= COOP_MAX_MEMBERS; id++) {
        if (!s->link[id]) continue;
        CoopMemberInfo *m = &r->m[r->n++];
        m->id = (uint8_t)id;
        m->session = s->member_session[id];
        copy_str(m->name, sizeof m->name, s->member_name[id]);
        const uint64_t at = s->heard_at[id];
        m->flags = (at && now - at >= AWAY_MS) ? COOP_FLAG_AWAY : 0;
    }
}

// hosting: the room as it is now, out to every member and into this session's own view.
static void host_publish(CoopSess *s, uint64_t now) {
    CoopRoster r;
    host_roster(s, &r, now);
    s->roster = r;
    s->next_roster = now + ROSTER_EVERY_MS;
    uint8_t payload[COOP_MAX_PAYLOAD];
    const size_t n = coop_roster_pack(payload, sizeof payload, &r);
    if (n) send_frame(s, COOP_DST_ALL, COOP_F_ROSTER, 0, payload, n, 0);
    s->dirty = 1;
}

// hosting: the lowest member id nobody holds (1 is this game's). 0 when the room is full.
static int free_member_id(CoopSess *s) {
    for (int id = 2; id <= COOP_MAX_MEMBERS; id++)
        if (!s->link[id] && !s->taken[id]) return id;
    return 0;
}

// A member is out of the room: its link closed, its identity dropped everywhere by the roster that follows. `why` says
// what happened, for the log and the status.
static void drop_member(CoopSess *s, int id, const char *why) {
    if (id < 2 || id > COOP_MAX_MEMBERS || !s->link[id]) return;
    say(s, "%s (member %u) left the room: %s", s->member_name[id], id, why);
    close_link(s, id);
    s->taken[id] = 1;   // (held until the room ends: a joiner never gets an id that was used, so its replay windows
                        // and its progress identity cannot be taken over by someone else)
}

// ---- going into a room, and out of it

// Out of the room (or never in): the links close and the status says why.
static void go_left(CoopSess *s, const char *why, int log_it) {
    const int was_in = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    if (log_it) say(s, was_in ? "left the room (%s)" : "no room: %s", why);
    if (s->state == COOP_JOINED)
        for (int i = 0; i < 2; i++) send_message(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);   // (a link may already be gone)
    close_links(s);
    s->self = 0, s->made_room = 0, s->hosting = 0, s->session = 0, s->out_counter = 0;   // (no room, so not the room's holder either)
    s->roster.n = 0;
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        s->taken[id] = 0;
        s->member_session[id] = 0;
        s->member_name[id][0] = 0;
    }
    memset(s->rw, 0, sizeof s->rw);
    events_reset(s);
    set_state(s, COOP_LEFT);
    set_status(s, "%s", why);
    s->dirty = 1;
}

// hosting: the room's door, and this game's own join of it (member 1). 0 and a status when the port is taken.
static int start_hosting(CoopSess *s, uint64_t now) {
    if (!s->listener) {
        s->listener = coop_tcp_listen(s->port);
        if (!s->listener) {
            set_status(s, "cannot host on port %u: it is taken", s->port);
            SAY_LIMITED(s, L_PORT, now, "cannot host on port %u: another program holds it", s->port);
            return 0;
        }
        s->port = coop_tcp_port(s->listener);
        if (!coop_random(s->room_id, sizeof s->room_id))
            for (size_t i = 0; i < sizeof s->room_id; i++) s->room_id[i] = (uint8_t)(coop_now_ns() >> (8 * i));
        char id[COOP_ROOM_TEXT + 1];
        coop_room_id_hex(s->room_id, id);
        say(s, "hosting the room on port %u (room %s)", s->port, id);
        TRACE(s, "the room's id is %s", id);
        CoopAddr here[8];
        const unsigned n = coop_net_local_addresses(here, 8, s->port);
        for (unsigned i = 0; i < n; i++) {
            char text[64];
            coop_addr_str(&here[i], text, sizeof text);
            if (!s->addr[0]) copy_str(s->addr, sizeof s->addr, text);
            TRACE(s, "a joiner can type %s", text);
        }
        if (n) say(s, "a joiner can type %s (this PC's address)", s->addr);
    }
    if (!s->session) s->session = random64();
    s->self = 1, s->made_room = 1;
    s->hosting = 1;
    set_state(s, COOP_JOINED);
    set_status(s, "Hosting on port %u", s->port);
    host_publish(s, now);
    s->last_rx = now, s->next_ping = now, s->next_stats = now + STATS_MS;
    return 1;
}

// joined: the host's address, resolved. 0 when it cannot be.
static int resolve_host(CoopSess *s, uint64_t now) {
    if (s->target_ok) return 1;
    if (!s->host[0] || !coop_net_resolve(s->host, s->port, &s->target)) {
        set_status(s, "cannot find %s", s->host[0] ? s->host : "the host's address");
        SAY_LIMITED(s, L_RESOLVE, now, "cannot find the host's address for %s", s->host[0] ? s->host : "(none given)");
        return 0;
    }
    coop_addr_str(&s->target, s->target_text, sizeof s->target_text);
    s->target_ok = 1;
    TRACE(s, "the host is at %s", s->target_text);
    return 1;
}

// joined: a link to the host and this join's HELLO (a new session, the counter from 1). 0 and a status when the host
// cannot be reached.
static int join_host(CoopSess *s, uint64_t now) {
    if (s->link[1]) return 1;
    if (!resolve_host(s, now)) return 0;
    s->link[1] = coop_tcp_connect(&s->target, CONNECT_MS);
    if (!s->link[1]) {
        set_status(s, "cannot reach %s", s->target_text);
        SAY_LIMITED(s, L_SOCKET, now, "cannot reach the host at %s", s->target_text);
        return 0;
    }
    s->session = random64();
    s->out_counter = 0;
    CoopHello h = {COOP_PROTO, 0, {0}};   // (the protocol major the host insists on: a HELLO without it is turned away, as on a reconnect)
    h.session = s->session;
    copy_str(h.name, sizeof h.name, s->name);
    uint8_t payload[COOP_MAX_PAYLOAD];
    const size_t n = coop_hello_pack(payload, sizeof payload, &h);
    send_frame(s, 1, COOP_F_HELLO, 0, payload, n, 0);
    s->last_rx = now;
    set_state(s, COOP_CONNECTING);
    set_status(s, "Joining %s", s->target_text);
    say(s, "joining the room at %s", s->target_text);
    return 1;
}

// ---- the host's side of a link: who is waiting, who is in, what a frame from a member means

// A join the host refuses: the reason in words, then the link closes.
static void refuse(CoopTcp *t, uint8_t reason, const char *text) {
    CoopError e = {reason, ""};
    copy_str(e.text, sizeof e.text, text);
    uint8_t payload[COOP_MAX_PAYLOAD];
    const size_t n = coop_error_pack(payload, sizeof payload, &e);
    CoopFrame f = {COOP_F_ERROR, 0, 0, 0, 0};
    uint8_t frame[COOP_MAX_FRAME];
    const size_t k = coop_frame_pack(frame, sizeof frame, &f, payload, n);
    if (k) coop_tcp_send(t, frame, k);
}

// A connection that said nothing by now, or one that said who it is: HELLO.
static void on_waiting(CoopSess *s, int slot, const uint8_t *p, uint32_t n, uint64_t now) {
    CoopHello h;
    if (!coop_hello_parse(p, n, &h) || !h.session) {
        say(s, "a join that is not a co-op HELLO was turned away");
        refuse(s->wait[slot], COOP_E_PROTO, "not a co-op join");
        close_waiting(s, slot);
        return;
    }
    const int id = free_member_id(s);
    if (!id) {
        refuse(s->wait[slot], COOP_E_ROOM_FULL, "the room is full");
        say(s, "a join was turned away: the room is full (%u players)", COOP_MAX_MEMBERS);
        close_waiting(s, slot);
        return;
    }
    if (h.proto != COOP_PROTO) {   // (the host's own major: a joiner of another build joins nothing)
        refuse(s->wait[slot], COOP_E_PROTO, "update required: that game speaks another co-op protocol");
        close_waiting(s, slot);
        return;
    }
    CoopTcp *joined = s->wait[slot];   // (the link leaves the waiting row without being closed: it becomes the member's)
    s->wait[slot] = NULL;
    s->wait_at[slot] = 0;
    s->link[id] = joined;   // (moved, not copied: one link, one owner)
    s->member_session[id] = h.session;
    copy_str(s->member_name[id], sizeof s->member_name[id], h.name);
    s->heard_at[id] = now;
    CoopRoster r;
    host_roster(s, &r, now);
    CoopWelcome w = {0};
    w.member = (uint8_t)id, w.proto = COOP_PROTO;
    memcpy(w.room_id, s->room_id, sizeof w.room_id);
    w.roster = r;
    uint8_t payload[COOP_MAX_PAYLOAD];
    const size_t k = coop_welcome_pack(payload, sizeof payload, &w);
    CoopFrame f = {COOP_F_WELCOME, 0, (uint8_t)id, 0, 0};
    uint8_t frame[COOP_MAX_FRAME];
    const size_t n2 = coop_frame_pack(frame, sizeof frame, &f, payload, k);
    if (n2) coop_tcp_send(s->link[id], frame, n2);
    say(s, "%s (member %u) joined the room", h.name, id);
    host_publish(s, now);   // the others learn, and this game's own view shows the room
}

// hosting: a member's player message out to the rest of the room, and into this game's own handlers. The host stamps
// the sender's id (the one its link was given, not the one the frame names) and leaves the envelope as the sender made
// it: the session and counter in it are the sender's, and every game checks them against the roster.
static void forward_data(CoopSess *s, int from, uint8_t dst, const uint8_t *p, uint32_t n, uint64_t now) {
    CoopFrame g = {COOP_F_DATA, (uint8_t)from, dst, 0, 0};
    uint8_t frame[COOP_MAX_FRAME];
    const size_t k = coop_frame_pack(frame, sizeof frame, &g, p, n);
    if (k) {
        for (int id = 2; id <= COOP_MAX_MEMBERS; id++) {
            if (!s->link[id]) continue;
            if (id == from) continue;   // (the sender wrote this line and has it already: sending it back makes
                                        //  their game take its own position and equipment as another player's)
            if (dst != COOP_DST_ALL && dst != id) continue;
            if (coop_tcp_send(s->link[id], frame, k)) bump(&s->n_tx);
        }
    }
    take_message(s, from, p, n, now);   // (this game is a player of its own room too)
}

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
            if (n) send_message(s, (uint8_t)id, COOP_K_EVENT_ACK, bytes, n);
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
    if (n) send_message(s, (uint8_t)dst, COOP_K_EVENT, bytes, n);
}

// The host's roster replaces ours; the log tells who came, went, changed name or went away. 1 when this player's own
// line lists another session than its own: someone reached the host with this player's token, and the session joins
// again to take its number back (roster_rejoin).
static int apply_roster(CoopSess *s, const CoopRoster *r) {
    int taken = 0;
    for (int i = 0; i < r->n; i++) {
        const CoopMemberInfo *m = &r->m[i], *old = member_of(s, m->id);
        if (m->id == s->self) {   // the host's maker mark is not taken for this session's own creation (on_joined)
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
        if (s->state == COOP_JOINED) send_message(s, m.dst, m.kind, m.data, m.len);   // anywhere else it is dropped
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

// ---- what a frame means, on either side

// A message that passed its checks: to the handler of its kind, or to the reliable-event layer. `from` is the member id
// the link it arrived on was given (never what the frame says).
static void deliver_message(CoopSess *s, int from, uint64_t session, uint8_t kind,
                            const uint8_t *bytes, size_t n, uint64_t now) {
    coop_lock(&s->lock);
    s->heard_at[from] = now;
    coop_unlock(&s->lock);
    TRACE(s, "rx message kind %u from member %u, %u bytes", kind, from, (unsigned)n);
    if (kind == COOP_K_EVENT || kind == COOP_K_EVENT_ACK) {
        event_receive(s, from, session, kind, bytes, n, now);
        return;
    }
    CoopMsgFn fn = __atomic_load_n(&s->msg[kind].fn, __ATOMIC_ACQUIRE);
    if (fn) fn(from, bytes, n, s->msg[kind].user);
}

// One message, from whoever sent it (the host forwards a member's, a joiner takes the host's forwarding): its session
// must be the one the roster lists for its sender, and its counter new in that sender's window (coop.h).
static void take_message(CoopSess *s, int from, const uint8_t *p, uint32_t n, uint64_t now) {
    const CoopMemberInfo *m = member_of(s, from);
    uint64_t session;
    uint32_t counter, kind;
    const uint8_t *bytes;
    size_t nb;
    if (!m) {
        s->n_sender++, bump(&s->n_lost);
        SAY_LIMITED(s, L_SENDER, now, "a message from member %u was thrown away: not a member the roster names", from);
        return;
    }
    if (!coop_msg_parse(p, n, &session, &counter, (uint8_t *)&kind, &bytes, &nb)) {
        s->n_malformed++, bump(&s->n_lost);
        SAY_LIMITED(s, L_MALFORMED, now, "a message from member %u that is not a co-op message was thrown away", from);
        return;
    }
    int fresh = session == m->session ? window_check(&s->rw[from], session, counter) : 0;
    if (!fresh) {   // an earlier session's, or seen
        s->n_dup++, bump(&s->n_lost);
        SAY_LIMITED(s, L_REPLAY, now, "a message of member %u that is not of its present session, or seen before, was "
                                      "thrown away", from);
        return;
    }
    if (fresh < 0 && !window_below(&s->rw[from], counter)) {
        s->n_dup++, bump(&s->n_lost);
        SAY_LIMITED(s, L_REPLAY, now, "a message of member %u older than its last 64 was thrown away", from);
        return;
    }
    if (fresh > 0) window_commit(&s->rw[from], session, counter);
    if (kind == COOP_K_BYE) {   // a player that says goodbye on the way out: its id is not given to someone else
        if (s->hosting) {
            drop_member(s, from, "its game left the room");
            host_publish(s, now);
        }
        return;
    }
    deliver_message(s, from, session, (uint8_t)kind, bytes, nb, now);
}

// joined: the host's frame.
static int on_host_frame(CoopSess *s, const CoopFrame *f, const uint8_t *p, uint64_t now) {
    switch (f->type) {
    case COOP_F_WELCOME: {
        CoopWelcome w;
        if (!coop_welcome_parse(p, f->len, &w) || w.member < 1 || w.member > COOP_MAX_MEMBERS || !roster_valid(&w.roster)) {
            s->n_malformed++, bump(&s->n_lost);
            return 0;
        }
        s->self = w.member;
        memcpy(s->room_id, w.room_id, sizeof s->room_id);
        s->roster_seq = now;   // (the frame order is the host's order: a link cannot reorder them)
        apply_roster(s, &w.roster);
        set_state(s, COOP_JOINED);
        set_status(s, "Joined %s as member %u", s->target_text, s->self);
        say(s, "joined the room at %s as member %u", s->target_text, s->self);
        s->backoff = s->backoff_first_ms;
        s->last_rx = now, s->next_ping = now + s->ping_ms, s->next_stats = now + STATS_MS;
        s->dirty = 1;
        return 1;
    }
    case COOP_F_ROSTER: {
        CoopRoster r;
        if (!coop_roster_parse(p, f->len, &r, NULL) || !roster_valid(&r)) {
            s->n_malformed++, bump(&s->n_lost);
            return 0;
        }
        if (s->state != COOP_JOINED) return 0;
        if (apply_roster(s, &r)) roster_rejoin(s, now);   // (a link cannot reorder frames, but a roster listing this
                                                          // player's own id with another session means another game took
                                                          // this join's place: say so and take it back)
        s->last_rx = now;
        return 1;
    }
    case COOP_F_PONG: {
        uint32_t token;
        if (s->state != COOP_JOINED || !coop_u32_parse(p, f->len, &token)) return 0;
        const uint32_t ms = (uint32_t)(now - token);
        coop_lock(&s->lock);
        s->view.ping_ms = ms;
        coop_unlock(&s->lock);
        s->last_rx = now;
        s->dirty = 1;
        return 1;
    }
    case COOP_F_DATA:
        if (s->state != COOP_JOINED) return 0;
        s->last_rx = now;
        take_message(s, f->src, p, f->len, now);   // (the host stamped who sent it)
        return 1;
    case COOP_F_PING:
        return 0;   // (a host does not ping its joiners: it is the one being listened for)
    default:
        s->n_malformed++, bump(&s->n_lost);
        return 0;
    }
}

// hosting: a member's frame. A member may say HELLO (its name or join changed), PING, BYE, or send DATA: nothing else,
// and the id a frame names is checked against the id its link was given, so a member cannot claim to be another one.
static void on_member_frame(CoopSess *s, int id, const CoopFrame *f, const uint8_t *p, uint64_t now) {
    if (f->src && f->src != id) {
        s->n_sender++, bump(&s->n_lost);
        SAY_LIMITED(s, L_SENDER, now, "member %u sent a frame that names member %u: thrown away", id, f->src);
        return;
    }
    s->heard_at[id] = now;
    switch (f->type) {
    case COOP_F_HELLO: {
        CoopHello h;
        if (!coop_hello_parse(p, f->len, &h) || !h.session) return;
        if (h.session != s->member_session[id]) {   // a member that joined again on the link it already has: its new
                                                    // session and name; its replay window starts with it
            say(s, "%s (member %u) joined the room again", h.name, id);
            s->member_session[id] = h.session;
            copy_str(s->member_name[id], sizeof s->member_name[id], h.name);
            s->rw[id] = (Window){0};
            host_publish(s, now);
        } else if (strcmp(s->member_name[id], h.name)) {
            say(s, "member %u is now %s", id, h.name);
            copy_str(s->member_name[id], sizeof s->member_name[id], h.name);
            host_publish(s, now);
        }
        return;
    }
    case COOP_F_PING: {
        CoopFrame g = {COOP_F_PONG, 0, (uint8_t)id, 0, 0};
        uint8_t frame[COOP_MAX_FRAME];
        const size_t k = coop_frame_pack(frame, sizeof frame, &g, p, f->len);   // (echo the sender's own stamp back, untouched:
                                                                                // the two games do not share a clock, so a PONG that
                                                                                // carried this host's time would subtract two clocks and
                                                                                // answer with 0 ms here and 24 days there)
        if (k) coop_tcp_send(s->link[id], frame, k);
        return;
    }
    case COOP_F_BYE:
        drop_member(s, id, "its game left the room");
        host_publish(s, now);
        return;
    case COOP_F_DATA:
        forward_data(s, id, f->dst, p, f->len, now);
        return;
    default:
        s->n_malformed++, bump(&s->n_lost);
    }
}

// A link that said nothing by its time, or that broke: out.
static void sweep_links(CoopSess *s, uint64_t now) {
    for (int i = 0; i < COOP_MAX_WAIT; i++) {
        if (s->wait[i] && (coop_tcp_readable(s->wait[i], 0) < 0 || now >= s->wait_at[i])) {
            if (now >= s->wait_at[i]) say(s, "a join that never said who it is was turned away");
            close_waiting(s, i);
        }
    }
    if (!s->hosting) {
        if (s->link[1] && coop_tcp_readable(s->link[1], 0) < 0) {
            say(s, "the link to the host closed");
            lost_host(s, now);
        }
        return;
    }
    int changed = 0;
    for (int id = 2; id <= COOP_MAX_MEMBERS; id++) {
        if (!s->link[id]) continue;
        if (coop_tcp_readable(s->link[id], 0) < 0) {
            drop_member(s, id, "its link closed");
            changed = 1;
            continue;
        }
        const uint64_t at = s->heard_at[id];
        if (at && now - at >= s->silence_ms) {
            char why[64];
            snprintf(why, sizeof why, "nothing was heard from it for %u s", (unsigned)(s->silence_ms / 1000));
            drop_member(s, id, why);
            changed = 1;
        }
    }
    if (changed) host_publish(s, now);
    else if (now >= s->next_roster) host_publish(s, now);   // (the away marks and the names, whatever changed)
}

// Every link that has something waiting, read in turn: this is where everything arrives. 1 when a frame was taken.
static int drain_links(CoopSess *s, uint64_t now) {
    int got = 0;
    if (s->hosting && s->listener && coop_tcp_readable(s->listener, 0) == 1) {
        CoopAddr from;
        CoopTcp *t = coop_tcp_accept(s->listener, &from);
        if (t) {
            int slot = -1;
            for (int i = 0; i < COOP_MAX_WAIT; i++) if (!s->wait[i]) { slot = i; break; }
            if (slot < 0) {
                refuse(t, COOP_E_ROOM_FULL, "the room is full");
                coop_tcp_close(t);
            } else {
                coop_addr_str(&from, s->wait_text[slot], sizeof s->wait_text[slot]);
                s->wait[slot] = t;
                s->wait_at[slot] = now + HELLO_WAIT_MS;
                TRACE(s, "a join from %s is waiting for its HELLO", s->wait_text[slot]);
            }
            got = 1;
        }
    }
    for (int i = 0; i < COOP_MAX_WAIT; i++) {
        if (!s->wait[i]) continue;
        const int ready = coop_tcp_readable(s->wait[i], 0);
        if (ready == 0) continue;
        got = 1;
        if (ready < 0) {
            close_waiting(s, i);
            continue;
        }
        size_t n = 0;
        const int whole = coop_tcp_frame(s->wait[i], s->rx, sizeof s->rx, &n);
        if (whole == 0) continue;   // (its HELLO is not whole yet: HELLO_WAIT_MS is the time it gets)
        if (whole < 0) {
            say(s, "a join whose bytes are not co-op frames was turned away");
            close_waiting(s, i);
            continue;
        }
        CoopFrame f;
        const uint8_t *p;
        if (!coop_frame_parse(s->rx, n, &f, &p) || f.type != COOP_F_HELLO) {
            on_waiting(s, i, p ? p : s->rx + COOP_FRAME_HDR, p ? f.len : 0, now);
            got = 1;
            continue;
        }
        on_waiting(s, i, p, f.len, now);
        got = 1;
    }
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++) {
        if (!s->link[id]) continue;
        if (id == 1 && s->hosting) continue;
        for (;;) {
            const int ready = coop_tcp_readable(s->link[id], 0);
            if (ready == 0) break;
            got = 1;
            if (ready < 0) {
                if (s->hosting) {
                    drop_member(s, id, "its link closed");
                    host_publish(s, now);
                } else {
                    say(s, "the link to the host closed");
                    lost_host(s, now);
                }
                break;
            }
            // (a link is a stream: one read is not one frame. coop_tcp_frame takes whole frames and leaves the rest
            //  of a split one here for the next time, so a 1168-byte frame that the network cut in two is not lost)
            size_t n = 0;
            const int whole = coop_tcp_frame(s->link[id], s->rx, sizeof s->rx, &n);
            if (whole == 0) break;
            if (whole < 0) {   // (a frame longer than this reader can hold: whatever it is, it is not this room's stream)
                say(s, "member %u sent bytes that are not co-op frames", id);
                bump(&s->n_malformed), bump(&s->n_lost);
                if (s->hosting) {
                    drop_member(s, id, "its stream was not co-op's");
                    host_publish(s, now);
                } else
                    lost_host(s, now);
                break;
            }
            if (s->drop_pct && rnd(s) % 100 < s->drop_pct) {   // (HOS_COOP_DROP: a test's share of what arrives)
                s->n_test_drop++;
                continue;
            }
            CoopFrame f;
            const uint8_t *p;
            if (!coop_frame_parse(s->rx, n, &f, &p)) {
                s->n_malformed++, bump(&s->n_lost);
                SAY_LIMITED(s, L_MALFORMED, now, "a frame that is not a co-op frame was thrown away");
                bump(&s->n_lost);
                continue;
            }
            TRACE(s, "rx %s from %d, %u bytes", type_name(f.type), id, (unsigned)f.len);
            if (s->hosting) {
                if (f.type == COOP_F_HELLO || f.type == COOP_F_PING || f.type == COOP_F_BYE || f.type == COOP_F_DATA)
                    on_member_frame(s, id, &f, p, now);
                bump(&s->n_rx);
            } else if (on_host_frame(s, &f, p, now)) {
                bump(&s->n_rx);
            }
        }
    }
    return got;
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
    s->drop_pct = s->pending.drop_pct;
    s->retry = s->pending.retry;
    const int host_wanted = s->pending.host_wanted;
    const int on = s->pending.on;
    const uint16_t port_now = s->pending.port ? s->pending.port : (s->typed_port ? s->typed_port : DEFAULT_PORT);
    const int port_changed = s->port != port_now;
    s->port = port_now;
    coop_unlock(&s->lock);
    if (on && s->state == COOP_OFF) {   // switched on: the room is what Host or Join asked for, if anything did
        set_state(s, COOP_LEFT);
        set_status(s, "%s", "");
        if (host_wanted == 1) {
            s->want_role = 1;
            s->next_try = now;
        } else if (host_wanted == 2) {
            s->want_role = 2;
            s->next_try = now;
        }
    }
    if (port_changed && s->state != COOP_OFF)   // (the Port row, or another start's coop_port=): the room moves to the
        go_left(s, "the port changed", 1);      // new port, and a room wanted there is opened or reached again
    if (strcmp(server, s->server)) {
        copy_str(s->server, sizeof s->server, server);
        s->target_ok = 0;
        s->host[0] = 0;
        s->typed_port = 0;
        uint16_t port = 0;
        if (s->server[0] && !coop_server_parse(s->server, s->host, sizeof s->host, &port))
            say(s, "the coop_server setting is not host or host:port");
        else if (port && port != DEFAULT_PORT) s->typed_port = port;   // (typed with the address: it wins over coop_port=)
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING || s->state == COOP_CONNECTING)
            go_left(s, "the host's address changed", 1);
    }
    if (strcmp(name, s->name)) {
        copy_str(s->name, sizeof s->name, name);
        s->dirty = 1;   // (the view shows the name in use, in a room or not)
        if (s->state == COOP_JOINED) s->rename_at = now + RENAME_MS;   // told to the room once the typing has stopped
    }
    if (!on) {
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING)
            say(s, "left the room (%s)", why[0] ? why : "co-op turned off");
        if (s->state == COOP_JOINED)
            for (int i = 0; i < 2; i++) send_message(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
        close_links(s);
        s->self = 0, s->made_room = 0, s->hosting = 0, s->session = 0, s->out_counter = 0, s->want_role = 0;
        s->roster.n = 0;
        s->rename_at = 0;
        set_state(s, COOP_OFF);
        set_status(s, "%s", "");
        s->dirty = 1;
        s->off_applied = 1;
    }
}

// Host, Join or Leave from the menu (coop_command). Whatever was going on before ends: one room at a time.
static void do_command(CoopSess *s, int cmd, const char *text, uint64_t now) {
    if (s->state == COOP_OFF) return;
    switch (cmd) {
    case COOP_CMD_LEAVE:
        s->want_role = 0;
        if (s->state == COOP_LEFT) break;
        if (s->state == COOP_JOINED) {
            for (int i = 0; i < 2; i++) send_message(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
            say(s, s->hosting ? "stopped hosting" : "left the room");
        } else {
            say(s, "stopped trying to reach the room");
        }
        go_left(s, "you left the room", 0);
        break;
    case COOP_CMD_HOST:
        if (s->state == COOP_JOINED && s->hosting) {   // the same button again: stop hosting
            s->want_role = 0;
            go_left(s, "you stopped hosting", 0);
            break;
        }
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING)
            for (int i = 0; i < 2; i++) send_message(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
        close_links(s);
        s->want_role = 1;
        s->backoff = s->backoff_first_ms;
        s->next_try = now;
        break;
    case COOP_CMD_JOIN: {
        char host[128];
        uint16_t port = 0;
        if (!text || !*text) text = s->server;   // (no address typed: the one in the options file)
        if (!text || !*text || !coop_server_parse(text, host, sizeof host, &port)) {
            go_left(s, "type the host's address first", 0);
            say(s, "Join needs a host's address: host, host:port or [v6]:port");
            break;
        }
        if (s->state == COOP_JOINED && !s->hosting && !strcmp(s->host, host) && s->port == (port ? port : s->port)) {
            s->want_role = 0;   // the same button again: leave the room it joined
            go_left(s, "you left the room", 0);
            break;
        }
        if (s->state == COOP_JOINED || s->state == COOP_RECONNECTING)
            for (int i = 0; i < 2; i++) send_message(s, COOP_DST_ALL, COOP_K_BYE, NULL, 0);
        close_links(s);
        copy_str(s->server, sizeof s->server, text);
        copy_str(s->host, sizeof s->host, host);
        if (port) s->port = port;
        s->target_ok = 0;
        s->want_role = 2;
        s->backoff = s->backoff_first_ms;
        s->next_try = now;
        break;
    }
    }
}

// The host's Remove a player, with the member's session so that a removal asked for later cannot remove whoever has the
// id then: the session the published view listed when it was asked for (progress sync removed that member's key at that
// moment), so that a member that rejoined since, or another player given the id, is not the one removed.
static void do_remove(CoopSess *s, int member, uint64_t session) {
    const CoopMemberInfo *m = member_of(s, member);
    if (s->state != COOP_JOINED || !s->made_room || !m || member == s->self) {
        say(s, "only the game that hosts can remove a player, and only one in the room");
        return;
    }
    const uint64_t at = session ? session : m->session;
    uint8_t payload[16];
    const size_t k = coop_kick_pack(payload, sizeof payload, (uint8_t)member, at);
    CoopFrame f = {COOP_F_KICK, 0, COOP_DST_ALL, 0, 0};
    uint8_t frame[COOP_MAX_FRAME];
    const size_t n = coop_frame_pack(frame, sizeof frame, &f, payload, k);
    for (int id = 2; id <= COOP_MAX_MEMBERS; id++)
        if (s->link[id] && n) coop_tcp_send(s->link[id], frame, n);
    say(s, "removing %s (member %u) from the room", m->name, member);
    drop_member(s, member, "the host removed it");
    host_publish(s, coop_now_ms());
}

// ---- reaching the room again

// joined: the host is gone (the link closed, or nothing was heard from it). Same address, a new join of it, with a
// backoff: 1, 2, 4, 8, then 15 s. A joined game does not give up on its own: the player leaves with the menu.
static void lost_host(CoopSess *s, uint64_t now) {
    if (s->state == COOP_RECONNECTING) return;
    close_link(s, 1);
    set_state(s, COOP_RECONNECTING);
    set_status(s, "the host is gone; trying again");
    s->self = 0;
    s->backoff = s->backoff_first_ms;
    s->next_try = now + s->backoff;
    s->dirty = 1;
}

// A roster that lists this player's own id with another session: another game took this join's place (a join under the
// same address twice, or a link that was taken over). Out and back in, with a new session.
static void roster_rejoin(CoopSess *s, uint64_t now) {
    say(s, "this join was taken over: joining the room again");
    close_link(s, 1);
    s->self = 0;
    s->session = 0;
    set_state(s, COOP_CONNECTING);
    set_status(s, "joining the room again");
    s->next_try = now;
    s->dirty = 1;
}

// CONNECTING or RECONNECTING: the next attempt, with the backoff. 0 when it could not be made.
static int retry_room(CoopSess *s, uint64_t now) {
    if (now < s->next_try) return 0;
    const int ok = s->want_role == 1 ? start_hosting(s, now) : join_host(s, now);
    if (!ok) {
        s->next_try = now + s->backoff;
        s->backoff = s->backoff * 2 > s->backoff_max_ms ? s->backoff_max_ms : s->backoff * 2;
        if (!s->retry) {   // a session that was not asked to keep trying ends the attempt here
            go_left(s, "the room could not be reached", 0);
            s->want_role = 0;
        }
    } else {
        s->backoff = s->backoff_first_ms;
    }
    return ok;
}

static void timers(CoopSess *s, uint64_t now) {
    switch (s->state) {
    case COOP_LEFT:
    case COOP_CONNECTING:
    case COOP_RECONNECTING:
        if (s->want_role) retry_room(s, now);
        break;
    case COOP_JOINED:
        if (!s->hosting && now - s->last_rx >= s->silence_ms) {
            say(s, "nothing from the host for %u s", (unsigned)(s->silence_ms / 1000));
            lost_host(s, now);
            break;
        }
        if (s->rename_at && now >= s->rename_at) {   // the name changed: the room learns it
            s->rename_at = 0;
            say(s, "joining the room as %s", s->name);
            if (s->hosting) {
                host_publish(s, now);
            } else {
                CoopHello h = {COOP_PROTO, s->session, {0}};
                copy_str(h.name, sizeof h.name, s->name);
                uint8_t payload[COOP_MAX_PAYLOAD];
                const size_t n = coop_hello_pack(payload, sizeof payload, &h);
                if (n) send_frame(s, 1, COOP_F_HELLO, 0, payload, n, 0);
                s->last_rx = now;
            }
            break;
        }
        sweep_links(s, now);
        if (s->state != COOP_JOINED) break;
        if (!s->hosting && now >= s->next_ping) {
            uint8_t token[4];
            const size_t n = coop_u32_pack(token, sizeof token, (uint32_t)now);
            if (n) send_frame(s, 1, COOP_F_PING, 0, token, n, s->self);
            s->next_ping = now + s->ping_ms;
        }
        if (now >= s->next_stats) {
            s->next_stats = now + STATS_MS;
            const uint32_t seen = s->n_rx + s->n_tx + s->n_lost + s->n_test_drop;
            if (seen != s->stats_seen) {
                s->stats_seen = seen;
                coop_lock(&s->lock);
                const uint32_t ping = s->view.ping_ms;
                coop_unlock(&s->lock);
                say(s, "stats: sent %u, accepted %u, thrown away %u (sender %u, not co-op %u, replay %u), dropped by "
                       "the test %u, outbox full %u, round trip %u ms",
                    s->n_tx, s->n_rx, s->n_lost, s->n_sender, s->n_malformed, s->n_dup, s->n_test_drop,
                    __atomic_load_n(&s->n_outbox_full, __ATOMIC_RELAXED), ping);
            }
        }
        break;
    default:
        break;
    }
}

// ---- the step

static void view_rebuild(CoopSess *s) {
    coop_lock(&s->lock);
    CoopView *v = &s->view;
    const int in_room = s->state == COOP_JOINED || s->state == COOP_RECONNECTING;
    v->state = s->state;
    v->hosting = s->hosting;
    v->port = s->port;
    copy_str(v->addr, sizeof v->addr, s->hosting ? s->addr : s->target_text);
    v->self = in_room ? s->self : 0;
    v->made = in_room && s->made_room;
    v->session = in_room ? s->session : 0;
    copy_str(v->name, sizeof v->name, s->name);
    if (in_room) {   // (the room's id as text: what a signature says the rules were made for)
        char id[COOP_ROOM_TEXT + 1];
        coop_room_id_hex(s->room_id, id);
        copy_str(v->room, sizeof v->room, id);
    } else v->room[0] = 0;
    v->maker = in_room ? 1 : 0;
    v->n = in_room ? s->roster.n : 0;
    const uint64_t now = coop_now_ms();
    for (int i = 0; i < v->n; i++) {
        const CoopMemberInfo *m = &s->roster.m[i];
        v->member[i].id = m->id;
        v->member[i].away = m->flags & COOP_FLAG_AWAY ? 1 : 0;
        v->member[i].maker = m->flags & COOP_FLAG_MAKER ? 1 : 0;
        v->member[i].session = m->session;
        copy_str(v->member[i].name, sizeof v->member[i].name, m->name);
        const uint64_t at = s->heard_at[m->id];
        v->member[i].heard_ms = at && now >= at ? (uint32_t)(now - at) : UINT32_MAX;
    }
    copy_str(v->status, sizeof v->status, s->status);
    if (s->state != COOP_JOINED) v->ping_ms = 0;
    v->tx = __atomic_load_n(&s->n_tx, __ATOMIC_RELAXED);
    v->rx = __atomic_load_n(&s->n_rx, __ATOMIC_RELAXED);
    v->lost = __atomic_load_n(&s->n_lost, __ATOMIC_RELAXED);
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
    char text[sizeof s->cmd_text];
    memcpy(text, s->cmd_text, sizeof text);
    s->cmd = 0, s->cmd_remove = 0;
    coop_unlock(&s->lock);
    apply_settings(s, now);
    const int stopped = s->off_applied;   // (a stop carried out in this step: no wait after it, its thread may end now)
    if (cmd) do_command(s, cmd, text, now);
    if (remove) do_remove(s, remove, remove_session);
    flush_outbox(s);
    publish(s);   // what a command or a setting changed shows before this thread waits for a frame
    const uint32_t wait = stopped ? 0 : s->state == COOP_JOINED ? 8
                              : s->state == COOP_LEFT ? 250 : 50;
    int got = drain_links(s, coop_now_ms());
    if (!got && wait && !stopped) coop_event_wait_ns(&s->wake, (uint64_t)wait * 1000000u);   // (a command, new settings
                                            // or a stop end the wait)
    timers(s, coop_now_ms());
    flush_outbox(s);
    drain_links(s, coop_now_ms());
    publish(s);
    TickHook ticks[TICK_HOOKS];
    coop_lock(&s->lock);
    const int nt = s->n_ticks;
    memcpy(ticks, s->ticks, sizeof ticks);
    coop_unlock(&s->lock);
    for (int i = 0; i < nt; i++) ticks[i].fn(coop_now_ms(), ticks[i].user);
    events_tick(s, coop_now_ms());
    publish(s);
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
    s->backoff_first_ms = p->backoff_first_ms ? p->backoff_first_ms : BACKOFF_FIRST_MS;
    s->backoff_max_ms = p->backoff_max_ms ? p->backoff_max_ms : BACKOFF_MAX_MS;
    s->port = p->port ? p->port : DEFAULT_PORT;
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
    close_links(s);
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
    s->pending.host_wanted = p->host_wanted;
    s->pending.why[0] = 0;
    if (!p->on) coop_event_reset(&s->off_done);
    s->cfg_pending = 1;
    coop_unlock(&s->lock);
    coop_event_set(&s->wake);
}

// A stop for why (the room's "left the room (why)" line), as coop_sess_stop.
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

// Host, Join or Leave. JOIN's text is the host's address; HOST and LEAVE take none. A JOIN with no address joins the
// one the settings give (coop_server=).
int coop_sess_command(CoopSess *s, CoopCmd cmd, const char *text) {
    if (cmd != COOP_CMD_HOST && cmd != COOP_CMD_JOIN && cmd != COOP_CMD_LEAVE) return 0;
    char addr[160] = "";
    if (cmd == COOP_CMD_JOIN && text && *text) copy_str(addr, sizeof addr, text);
    coop_lock(&s->lock);
    s->cmd = (int)cmd;
    copy_str(s->cmd_text, sizeof s->cmd_text, addr);
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

// ---- the session's hooks and its view

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
// co-op on, coop_name= is the player's name (empty: "Player-" and four random hex digits), coop_server= the host's
// address to join ("host", "host:port" or "[v6]:port"), coop_port= the port to host on or to join (0: 43400). The
// environment wins over each: HOS_COOP, HOS_COOP_NAME, HOS_COOP_SERVER, HOS_COOP_PORT. Off, no thread is started and
// nothing else runs. For tests: HOS_COOP_AUTO=host|join, a room this game holds or joins at start, and HOS_COOP_DROP, a
// test's share of the received frames thrown away (percent). The host's address reaches the log only with HOS_COOP_TRACE.
// No HOS_COOP name is taken from hookshot_env.txt, a file players pass around (host/hos_main.c env_line_allowed): the
// environment itself only.
int hos_ini_option(const char *key, int otherwise);          // host/nn_applet.c: hookshot_options.ini
int hos_ini_text(const char *key, char *out, size_t size);

static struct {
    char name[COOP_NAME_MAX + 1], server[160];
    int on, automatic, host_first;   // host_first: 1 this game hosts at start, 2 it joins the room at server
    uint16_t port;
    unsigned drop;
} g_set;                        // what the process's session is to be: coop_init's settings, coop_configure's changes
static CoopLock g_set_lock;     // g_set, g_stop and g_ended; taken before the session's own lock, never after it

// The coop thread runs while co-op is switched on. Switching it off (coop_configure) or the game closing (coop_shutdown)
// queues the session's stop, asks the thread to end (g_stop) and waits for it, at most STOP_WAIT_MS. The thread ends
// once it has carried the stop out: goodbye said, the links closed, the view that says "off" published. Held up for
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

// The settings into g_set, and the lines that say what they are (the host's address is logged only with HOS_COOP_TRACE).
// menu: the name and host address the menu gives (coop_configure), over the environment's and the options file's; NULL:
// those.
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
    if (p < 0 || p > 65535) hos_log("coop: the port (HOS_COOP_PORT or coop_port=) is not 1 to 65535: %u is used", DEFAULT_PORT);
    const char *drop = getenv("HOS_COOP_DROP");
    g_set.drop = drop ? (unsigned)strtoul(drop, NULL, 10) : 0;
    hos_log("coop: on, as %s, on port %u; %s%s", g_set.name, g_set.port ? g_set.port : DEFAULT_PORT,
            from ? "the host to join is from " : "no host given yet (coop_server= in hookshot_options.ini, HOS_COOP_SERVER,"
                 " or the Co-op tab)",
            from ? from : "");
    if (g_set.drop) hos_log("coop: HOS_COOP_DROP: %u%% of the received frames are thrown away (a test)", g_set.drop > 100 ? 100 : g_set.drop);
    const char *mode = getenv("HOS_COOP_AUTO");
    if (!mode || !*mode || !strcmp(mode, "off")) return;
    if (strcmp(mode, "host") && strcmp(mode, "join")) {
        hos_log("coop: HOS_COOP_AUTO must be off, host or join");
        return;
    }
    g_set.automatic = 1;
    g_set.host_first = strcmp(mode, "join") ? 1 : 2;
    if (g_set.host_first == 2 && !g_set.server[0]) {
        hos_log("coop: HOS_COOP_AUTO=join needs a host's address (HOS_COOP_SERVER or coop_server=)");
        g_set.automatic = 0;
        return;
    }
    hos_log("coop: HOS_COOP_AUTO=%s at start", mode);
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
        const int end = g_stop && !s->cfg_pending;   // (not when co-op was switched on again meanwhile,
                                                     // or a later stop waits)
        coop_unlock(&s->lock);
        if (end) g_ended = 1;
        coop_unlock(&g_set_lock);
        if (end) return;   // (the stop was carried out: the links are closed and the view says off)
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
        coop_sess_command(s, g_set.host_first == 1 ? COOP_CMD_HOST : COOP_CMD_JOIN, g_set.server);
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
        if (server) copy_str(g_set.server, sizeof g_set.server, server);
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
        if (wait) configure_locked(s);   // (the session's stop: BYE, the links closed)
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

void coop_set_port(uint16_t port) {
    CoopSess *s = process_session();
    if (!s) return;
    coop_once(&g_set_once, make_locks, NULL);
    coop_lock(&g_life);
    coop_lock(&g_set_lock);
    g_set.port = port;
    if (g_ready && g_set.on) configure_locked(s);   // (a session that runs takes it on its next step)
    coop_unlock(&g_set_lock);
    coop_unlock(&g_life);
}

int coop_command(CoopCmd cmd, const char *text) {
    CoopSess *s = process_session();
    if (!s || !__atomic_load_n(&g_on, __ATOMIC_ACQUIRE)) return 0;   // (a command right after coop_configure(1) is queued)
    return coop_sess_command(s, cmd, text);
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
    int kicked = v.made && coop_sess_remove(s, member);   // (only the room's maker drops a member, and only its own Host does that)
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
