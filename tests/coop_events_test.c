// host/coop.c's reliable events, without the game: two sessions in one process, a room this process's own host holds
// (host/coop.c's own WELCOME and ROSTER frames, host/coop.h), joined by a second session. Between them is this file: it
// reads what either session sends, and chooses what reaches the other — everything, a share of it, a part of a big event
// once, a frame written in two halves, two frames in one write, a frame sent a second time. That is the whole point: a
// link is a stream of bytes, and an event that arrives must arrive whole, once, and in order, whatever the pipe did.
//
// The clock here is this file's: coop_sess_step is not used, so the sessions' event timers (host/coop.c's EVENT_SEND_MS,
// EVENT_RETRY_MS, EVENT_ACK_MS) run on a clock this file advances, and a case measures what it says it measures.
//
//   coop_events_test no-loss      every queued event, delivered once, in id order, the queue emptied
//   coop_events_test loss         the same with 30 % of the frames thrown away by the pipe
//   coop_events_test split        a part written in two halves, and two frames in one write
//   coop_events_test parts        a 16 KiB event, one part lost: whole or nothing, that part sent again
//   coop_events_test duplicate    a lost ACK, a part sent again, a recorded frame sent again (its counter is seen)
//   coop_events_test queue        the bounded queue: INVALID, FULL, UNAVAILABLE, a recipient gone, this game leaving
//   coop_events_test acceptance   nothing is claimed delivered before the other player accepts this one
//   coop_events_test cancel       an unfinished event abandoned, later events delivered, the old one cannot return
//   coop_events_test reset-ack    a receiver that starts over: an ACK of its old half cannot roll the new one back
//   coop_events_test codec        the event part and ACK codecs, their lengths and their refusals
//   coop_events_test threads      enqueueing from another thread while this one steps: no torn state
//
// Compiled by tests/test_coop_events.py with host/coop.c, host/coop_net.c, host/coop_proto.c and Monocypher; on Linux
// with AddressSanitizer and UBSan when the compiler has them, and again with ThreadSanitizer for `threads`.
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

#include "coop.h"
#include "coop_proto.h"
#include "coop.c"   // (the session itself, so its event timers and its links can be driven by hand)

void hos_log(const char *fmt, ...) { (void)fmt; }
int hos_ini_option(const char *key, int otherwise) { (void)key; return otherwise; }
int hos_ini_text(const char *key, char *out, size_t n) { (void)key; (void)out; (void)n; return 0; }

static CoopSess *a, *b;                 // a: the room's host, member 1. b: joined to it, member 2.
static CoopTcp *door_a, *door_b, *door_c;  // this file's doors: a's link ends at door_a, b's at door_b, a spare at door_c
static CoopTcp *pipe_a, *pipe_b, *pipe_c;   // ... and this file's ends of those links
static CoopTcp *link_a, *link_b, *link_c;   // the sessions' ends
static uint64_t clock_ms;               // the clock the session's timers are told
static unsigned received, raw_received, forwarded, dropped, read_a, read_b;
static uint32_t lose_parts;   // (bit per part index: what the pipe will not carry)
static uint64_t last_id;
static uint8_t expected[COOP_EVENT_MAX];
static size_t expected_n;
static int expect_pattern;
static unsigned rng = 1234567u;
static unsigned sent_parts[64];
static int lose_part = -1, lost_part, ack_loss;   // (lose_part: throw that part away once; lose_parts: every copy of them)
static uint8_t saved[COOP_MAX_FRAME];   // one recorded frame, for the replay cases
static size_t saved_n;

typedef struct { CoopTcp *to; size_t n; uint8_t bytes[COOP_MAX_FRAME]; } Packet;
static Packet pending[256];
static unsigned npending;

static void receive_event(int from, uint64_t session, uint64_t id, const uint8_t *bytes, size_t n, void *user);
static void pump(unsigned milliseconds, unsigned loss, int reverse);
static unsigned queued(void);

static void receive_event(int from, uint64_t session, uint64_t id, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    assert(from == 1 && session == 101 && id > last_id);   // (in order, once, from the one member this file accepts)
    if (expect_pattern) assert(n == expected_n && !memcmp(bytes, expected, n));
    last_id = id;
    received++;
}
static void receive_raw(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)from; (void)bytes; (void)n; (void)user;
    raw_received++;
}

// (grant, not "accept": winsock2.h declares accept(), and the link header brings it in on Windows)
static void grant(CoopSess *s, uint32_t members) {
    CoopView v;
    coop_sess_view(s, &v);
    coop_sess_accept_members(s, &v, members);
}

/* ---- the room, made by hand: host/coop.c's own frames, over this file's sockets ---- */

static CoopTcp *accept_wait(CoopTcp *door) {
    for (int i = 0; i < 200; i++) {
        CoopAddr from;
        CoopTcp *t = coop_tcp_accept(door, &from);
        if (t) return t;
        coop_sleep_ms(10);
    }
    assert(0 && "nobody reached the door");
}

static void setup(void) {
    CoopParams pa = {.tag = "A", .on = 1, .name = "Host", .client_id = 7, .port = 43400};
    CoopParams pb = {.tag = "B", .on = 1, .server = "127.0.0.1", .name = "Saria", .client_id = 8, .port = 43400};
    a = coop_sess_new(&pa);
    b = coop_sess_new(&pb);
    assert(a && b);
    door_a = coop_tcp_listen(0), door_b = coop_tcp_listen(0);
    assert(door_a && door_b && coop_tcp_port(door_a) && coop_tcp_port(door_b) && coop_tcp_port(door_a) != coop_tcp_port(door_b));
    CoopAddr to;
    assert(coop_net_resolve("127.0.0.1", coop_tcp_port(door_a), &to));
    link_a = coop_tcp_connect(&to, 3000);
    assert(link_a);
    pipe_a = accept_wait(door_a);
    assert(pipe_a);
    assert(coop_net_resolve("127.0.0.1", coop_tcp_port(door_b), &to));
    link_b = coop_tcp_connect(&to, 3000);
    assert(link_b);
    pipe_b = accept_wait(door_b);
    assert(pipe_b);
    door_c = coop_tcp_listen(0);   // (a spare link, for a member that joins the room again)
    assert(door_c && coop_net_resolve("127.0.0.1", coop_tcp_port(door_c), &to));
    link_c = coop_tcp_connect(&to, 3000);
    assert(link_c);
    pipe_c = accept_wait(door_c);
    assert(pipe_c);
    clock_ms = 1000;
    for (int i = 0; i < COOP_EVENT_MAX; i++) expected[i] = (uint8_t)(i * 7u);

    // a: the room's host, member 1, with member 2 already its link's.
    a->hosting = 1, a->made_room = 1, a->self = 1, a->session = 101, a->port = 43400, a->typed_port = 43400;
    a->room_id[0] = 0x11, a->room_id[1] = 0x22, a->room_id[2] = 0x33, a->room_id[3] = 0x44;
    a->link[2] = link_a;
    a->member_session[2] = 202;
    snprintf(a->member_name[2], sizeof a->member_name[2], "Saria");
    a->heard_at[2] = clock_ms;
    set_state(a, COOP_JOINED);
    host_publish(a, clock_ms);   // (its roster, out to member 2 and into its own view)
    publish(a);

    // b: a join that took the host's WELCOME and its roster, from the wire.
    b->link[1] = link_b;
    b->session = 202;   // (this join's own, the one the host's roster lists for member 2)
    b->target_ok = 1;
    snprintf(b->target_text, sizeof b->target_text, "127.0.0.1");
    CoopRoster r;
    host_roster(a, &r, clock_ms);
    CoopWelcome w = {0};
    w.member = 2, w.proto = COOP_PROTO;
    memcpy(w.room_id, a->room_id, sizeof w.room_id);
    w.roster = r;
    uint8_t payload[COOP_MAX_PAYLOAD], frame[COOP_MAX_FRAME];
    size_t n = coop_welcome_pack(payload, sizeof payload, &w);
    CoopFrame f = {COOP_F_WELCOME, 0, 2, 0, 0};
    n = coop_frame_pack(frame, sizeof frame, &f, payload, n);
    assert(n && coop_tcp_send(pipe_b, frame, n));
    assert(coop_sess_on_event(b, COOP_K_TEST, receive_event, NULL));
    assert(coop_sess_on_message(b, COOP_K_TEST, receive_raw, NULL));
    assert(coop_sess_on_event(a, COOP_K_TEST, receive_event, NULL));   // (the host's own handler: member 2's events)
    drain_links(b, clock_ms), publish(b);
    assert(b->state == COOP_JOINED && b->self == 2 && b->roster.n == 2 && b->session);
    grant(b, 1u << 1);   // (b accepts the host, whose session its roster lists)
    drain_links(a, clock_ms), publish(a);
    grant(a, 1u << 2);   // ... and the host accepts b
    assert(coop_sess_member_accepted(a, 2) && coop_sess_member_accepted(b, 1));
    b->session = b->roster.m[1].session;   // (the session the roster lists for member 2 is the one b joins with)
    assert(b->session == 202);
    pump(8, 0, 0);   // (what setup put on the wire is off the wire before a case starts)
    forwarded = dropped = 0;
    lose_part = -1;
}

/* ---- the pipe: what either session sends, and what this file lets through ---- */

static uint8_t scratch[COOP_MAX_FRAME + 8];

// (what a side is given to have something in it: a session's own link reads with a timeout too — coop_sess_step waits
//  up to 8 ms in a room — and a pipe that is polled with no patience at all reads nothing on a busy machine)
static unsigned read_ms = 3;

// How many frames this session has reported sending that this side of the pipe has not read yet.
static unsigned outstanding(CoopTcp *from, const unsigned *seen) {
    const unsigned sent = (from == pipe_a ? a : b)->n_tx;
    return sent > *seen ? sent - *seen : 0;
}

// Reads whole frames off one side of the pipe. Returns how many are still unaccounted for, so the next call knows
// whether to wait for the pipe at all: patience for what is in flight, none for a quiet room.
static unsigned read_side(CoopTcp *from, CoopTcp *to, unsigned loss, unsigned wait_ms) {
    unsigned *seen = from == pipe_a ? &read_a : &read_b;
    for (;;) {
        const int ready = coop_tcp_readable(from, wait_ms);
        wait_ms = 0;   // (the patience is for what was just sent, not for every frame after it)
        if (ready <= 0) return outstanding(from, seen);
        size_t n = 0;
        const int whole = coop_tcp_frame(from, scratch, sizeof scratch, &n);
        if (whole != 1) return outstanding(from, seen);   // (not a frame this file can hold, or not whole yet)
        (*seen)++;
        CoopFrame f;
        const uint8_t *p;
        if (!coop_frame_parse(scratch, n, &f, &p)) { forwarded++; continue; }
        uint64_t session;
        uint32_t counter;
        uint8_t kind;
        const uint8_t *body;
        size_t nb;
        int is_event = 0, part = -1;
        if (f.type == COOP_F_DATA && coop_msg_parse(p, f.len, &session, &counter, &kind, &body, &nb) &&
            (kind == COOP_K_EVENT || kind == COOP_K_EVENT_ACK)) {
            is_event = 1;
            if (kind == COOP_K_EVENT) {
                CoopEventPart e;
                assert(coop_event_part_parse(body, nb, &e));
                sent_parts[e.part]++;
                part = e.part;
            }
            if (!saved_n) memcpy(saved, scratch, n), saved_n = n;
        }
        rng = rng * 1664525u + 1013904223u;
        if (is_event && kind == COOP_K_EVENT_ACK && ack_loss) { dropped++; continue; }
        if (part >= 0 && part == lose_part && !lost_part) { lost_part = 1, dropped++; continue; }
        if (part >= 0 && ((lose_parts >> part) & 1u)) { dropped++; continue; }
        if (rng % 100 < loss) { dropped++; continue; }
        assert(npending < 256);
        pending[npending].to = to;
        pending[npending].n = n;
        memcpy(pending[npending].bytes, scratch, n);
        npending++;
    }
}

static void deliver(int reverse) {
    for (unsigned i = 0; i < npending; i++) {
        const Packet *k = &pending[reverse ? npending - 1u - i : i];
        coop_tcp_send(k->to, k->bytes, k->n);
        forwarded++;
    }
    npending = 0;
}

// One step of the clock: the sessions' event timers, then the pipe, then the sessions reading what the pipe wrote.
// A side is waited for only while it has frames the pipe has not read yet: patience when something is in flight, no
// waiting at all when the room is quiet.
static void pump(unsigned milliseconds, unsigned loss, int reverse) {
    uint32_t last_a = 0, last_b = 0;
    unsigned out_a = 0, out_b = 0, quiet = 0;
    for (unsigned tick = 0; tick < milliseconds; tick += 8) {
        clock_ms += 8;
        events_tick(a, clock_ms);
        events_tick(b, clock_ms);
        out_a += a->n_tx - last_a, last_a = a->n_tx;
        out_b += b->n_tx - last_b, last_b = b->n_tx;
        out_a = read_side(pipe_a, pipe_b, loss, out_a ? read_ms : 0);
        out_b = read_side(pipe_b, pipe_a, loss, out_b ? read_ms : 0);
        const int moved = npending;
        deliver(reverse);
        // (a link is a pipe with a length: bytes written a moment ago are not in the other game yet. A session's own
        //  read waits for what is coming the same way, coop_sess_step included, so the pump gives it the same chance
        //  before stepping its links)
        if (moved) {
            if (a->link[2]) coop_tcp_readable(a->link[2], read_ms);
            if (b->link[1]) coop_tcp_readable(b->link[1], read_ms);
        }
        drain_links(a, clock_ms), publish(a);
        drain_links(b, clock_ms), publish(b);
        // (once the room has nothing left to say, the rest of the clock is not worth stepping: quiet for half a
        //  second of fake time, with an empty queue and an empty pipe, and the case has had its answer)
        if (out_a || out_b || npending || queued()) quiet = 0;
        else if ((quiet += 8) >= 500) break;
    }
}

static unsigned queued(void) {
    unsigned n = 0;
    coop_lock(&a->lock);
    for (unsigned i = 0; i < COOP_EVENT_SLOTS; i++) n += a->event_out[i].id != 0;
    coop_unlock(&a->lock);
    return n;
}

// The exact bytes one of the sessions sent for a given queued event, rebuilt from its own state: what a receiver would
// keep and send again, without sniffing the wire.
static size_t pack_part(CoopSess *s, uint64_t id, int to, uint8_t part, uint8_t *out, size_t cap) {
    coop_lock(&s->lock);
    EventOut *e = NULL;
    for (unsigned i = 0; e == NULL && i < COOP_EVENT_SLOTS; i++)
        if (s->event_out[i].id == id) e = &s->event_out[i];
    if (!e) {
        coop_unlock(&s->lock);
        return 0;
    }
    CoopEventPart p = {.recipient_session = e->target[to].session, .id = e->id, .total = e->len, .kind = e->kind,
                       .part = part, .bytes = e->bytes + (size_t)part * COOP_EVENT_PART};
    coop_unlock(&s->lock);
    uint8_t body[COOP_EVENT_HEAD + COOP_EVENT_PART];
    const size_t n = coop_event_part_pack(body, sizeof body, &p);
    return n ? coop_msg_pack(out, cap, s->session, ++s->out_counter, COOP_K_EVENT, body, n) : 0;
}

static void send_data(CoopSess *s, int to, uint8_t kind, const uint8_t *body, size_t n, CoopTcp *where) {
    uint8_t payload[COOP_MAX_PAYLOAD];
    const size_t k = coop_msg_pack(payload, sizeof payload, s->session, ++s->out_counter, kind, body, n);
    if (!k) return;
    CoopFrame f = {COOP_F_DATA, (uint8_t)s->self, (uint8_t)to, 0, 0};
    uint8_t frame[COOP_MAX_FRAME];
    const size_t m = coop_frame_pack(frame, sizeof frame, &f, payload, k);
    if (m) coop_tcp_send(where, frame, m);
}

/* ---- the cases ---- */

static void test_delivery(unsigned loss, int reverse) {
    for (unsigned i = 0; i < COOP_EVENT_SLOTS; i++)
        assert(coop_sess_send_event(a, 2, COOP_K_TEST, &i, sizeof i) == COOP_EVENT_QUEUED);
    pump(20000, loss, reverse);
    assert(received == COOP_EVENT_SLOTS && !queued() && !raw_received);
    if (loss) assert(dropped && forwarded);
    puts("PASS events: every queued event delivered once, in order, with and without a lossy pipe");
}

// A link is a stream: half a frame, then the rest of it, and two frames in one write. Nothing may be lost or doubled.
static void test_split(void) {
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, COOP_EVENT_PART) == COOP_EVENT_QUEUED);
    clock_ms += EVENT_SEND_MS;
    events_tick(a, clock_ms);
    read_side(pipe_a, pipe_b, 0, read_ms);
    assert(npending == 1);
    const size_t n = pending[0].n;
    assert(n == COOP_FRAME_HDR + COOP_MSG_HDR + 1 + COOP_EVENT_HEAD + COOP_EVENT_PART);   // (the whole 1024 bytes in one part)
    npending = 0;
    coop_tcp_send(pipe_b, pending[0].bytes, 7);           // (one byte less than a header: the reader must wait)
    coop_tcp_readable(b->link[1], read_ms), drain_links(b, clock_ms), publish(b);
    assert(!received);
    coop_tcp_send(pipe_b, pending[0].bytes + 7, n - 7);   // (the rest of it, joined by the link)
    coop_tcp_readable(b->link[1], read_ms), drain_links(b, clock_ms), publish(b);
    assert(received == 1 && last_id == 1);
    clock_ms += EVENT_ACK_MS;
    events_tick(b, clock_ms);   // (the receiver thanks it, the ordinary way)
    read_side(pipe_b, pipe_a, 0, read_ms);
    deliver(0);
    drain_links(a, clock_ms), publish(a);
    assert(!queued());   // (the first event is done, so the next one gets its turn)

    // Two frames in one write: a second event, and a second copy of the same part, taken one at a time.
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, 8) == COOP_EVENT_QUEUED);
    clock_ms += EVENT_SEND_MS;
    events_tick(a, clock_ms);
    read_side(pipe_a, pipe_b, 0, read_ms);
    assert(npending == 1);
    uint8_t message[COOP_MAX_PAYLOAD];
    const size_t k = pack_part(a, 2, 2, 0, message, sizeof message);
    assert(k);
    uint8_t both[2 * COOP_MAX_FRAME];
    memcpy(both, pending[0].bytes, pending[0].n);
    CoopFrame f = {COOP_F_DATA, 1, 2, 0, 0};
    const size_t m = coop_frame_pack(both + pending[0].n, sizeof both - pending[0].n, &f, message, k);
    assert(m);
    npending = 0;
    coop_tcp_send(pipe_b, both, pending[0].n + m);   // (two whole frames in one write: one link, two frames)
    coop_tcp_readable(b->link[1], read_ms), drain_links(b, clock_ms), publish(b);
    assert(received == 2 && last_id == 2);   // (two frames, one event: the copy of a part it has is not a second event)
    pump(2000, 0, 0);
    assert(received == 2 && !queued());
    puts("PASS events: a frame in two halves, a header split, two frames in one write");
}

static void test_parts(void) {
    expected_n = COOP_EVENT_MAX, expect_pattern = 1;
    lose_part = 5;
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, expected_n) == COOP_EVENT_QUEUED);
    pump(480, 0, 0);   // (every part but the one the pipe threw away is on its way: not one more than it needs)
    assert(lost_part && received == 0);   // a hole never reaches the application
    pump(12000, 30, 0);
    assert(received == 1 && !queued() && sent_parts[5] >= 2);
    puts("PASS events: 16 KiB reassembly, a part lost, whole or nothing");
}

static void test_duplicate(void) {
    ack_loss = 1;
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, NULL, 0) == COOP_EVENT_QUEUED);
    pump(2000, 0, 0);
    assert(received == 1 && queued() == 1 && sent_parts[0] > 1);   // (no ACK: it is sent again)
    // The very frame that was on the wire, sent again: the same envelope, seen before, so it is thrown away.
    coop_tcp_send(pipe_b, saved, saved_n);
    pump(1000, 0, 0);
    assert(received == 1 && b->n_dup);
    // The same part of the same event, with a fresh envelope: the receiver has that part already.
    uint8_t message[COOP_MAX_PAYLOAD];
    const size_t k = pack_part(a, 1, 2, 0, message, sizeof message);
    assert(k);
    send_data(a, 2, COOP_K_EVENT, message, k, pipe_b);
    pump(1000, 0, 0);
    assert(received == 1 && !raw_received);   // (the same part of the same id: delivered once)
    ack_loss = 0;
    pump(1000, 0, 0);
    assert(received == 1 && !queued());
    puts("PASS events: a lost ACK, a part sent again, a seen envelope and a seen part");
}

static void test_queue_and_departure(void) {
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, COOP_EVENT_MAX + 1) == COOP_EVENT_INVALID);
    assert(coop_sess_send_event(a, 2, COOP_K_EVENT, NULL, 0) == COOP_EVENT_INVALID);
    assert(coop_sess_send_event(a, -1, COOP_K_TEST, NULL, 0) == COOP_EVENT_INVALID);
    for (int i = 0; i < COOP_EVENT_SLOTS; i++) assert(coop_sess_send_event(a, 0, COOP_K_TEST, expected, sizeof expected) == 1);
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, NULL, 0) == COOP_EVENT_FULL);
    pump(64, 0, 0);
    assert(!received);
    drop_member(a, 2, "test"), host_publish(a, clock_ms), publish(a);   // (the member's link is gone, as in a real departure)
    events_tick(a, clock_ms);
    assert(!queued() && !coop_sess_member_accepted(a, 2));
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, NULL, 0) == COOP_EVENT_UNAVAILABLE);
    a->link[2] = link_c;   // (the same player's game joined again: a new link and a new session)
    a->member_session[2] = 303;
    snprintf(a->member_name[2], sizeof a->member_name[2], "Saria");
    a->heard_at[2] = clock_ms;
    host_publish(a, clock_ms), publish(a);
    assert(!coop_sess_member_accepted(a, 2));   // (a member that joined again is not the one that was accepted)
    grant(a, 1u << 2);
    assert(coop_sess_member_accepted(a, 2));
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, NULL, 0) == COOP_EVENT_QUEUED);
    go_left(a, "test", 0), publish(a);
    assert(!queued() && !coop_sess_member_accepted(a, 2));
    puts("PASS events: a bounded queue, a recipient gone or replaced, this game leaving");
}

static void test_acceptance(void) {
    CoopView stale;
    coop_sess_view(b, &stale);
    grant(b, 0);
    assert(!coop_sess_member_accepted(b, 1));
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, NULL, 0) == COOP_EVENT_QUEUED);
    pump(1000, 0, 0);
    assert(!received && queued() == 1);   // (the parts arrive, and nothing is claimed delivered)
    grant(b, 1u << 1);
    pump(1000, 0, 0);
    assert(received == 1 && !queued());   // (the sender sent them again, as it must until it is thanked)
    CoopRoster changed = b->roster;
    changed.m[0].session = 404;
    apply_roster(b, &changed), publish(b);
    assert(!coop_sess_member_accepted(b, 1));
    coop_sess_accept_members(b, &stale, 1u << 1);
    assert(!coop_sess_member_accepted(b, 1));   // (an acceptance is for the view it was made with)
    puts("PASS events: nothing delivered before the other player is accepted, and not under a stale view");
}

// An unfinished event abandoned: its later ids delivered, the abandoned one cannot come back.
// A 2048-byte event with one of its two parts thrown away: the receiver holds half of it, and nothing is delivered.
static void queue_partial(void) {
    expected_n = 2048, expect_pattern = 1;
    for (size_t i = 0; i < sizeof expected; i++) expected[i] = (uint8_t)(i * 11u + 3);
    lose_part = 1, lost_part = 0, lose_parts = 0;
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, expected_n) == COOP_EVENT_QUEUED);
    pump(200, 0, 0);   // (both parts sent, one of them thrown away, the retry not due yet)
    assert(lost_part && !received && b->event_in[1].parts == 0x01);
    lose_part = -1;
}

// An unfinished event abandoned: its later ids delivered, the abandoned one cannot come back.
static void test_cancel(int receiver_reaccepts) {
    queue_partial();
    grant(a, 0);   // (this game's consent was withdrawn: what it had queued for that player is gone)
    assert(!queued() && a->session == 101);
    if (receiver_reaccepts) grant(b, 0), grant(b, 1u << 1);
    grant(a, 1u << 2);
    for (int i = 0; i < 2; i++) assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, expected_n) == COOP_EVENT_QUEUED);
    pump(8000, 30, 0);
    assert(received == 2 && last_id == 3 && !queued());
    // The abandoned event's part, sent again with a fresh envelope: every part carries the sender's oldest live id, so
    // an older id is retired, not revived.
    CoopFrame f;
    const uint8_t *p;
    assert(coop_frame_parse(saved, saved_n, &f, &p));
    send_data(a, 2, COOP_K_EVENT, p + COOP_MSG_HDR + 1, f.len - COOP_MSG_HDR - 1, pipe_b);
    pump(2000, 0, 0);
    assert(received == 2 && last_id == 3);   // (id 1 cannot return after ids 2 and 3)
    puts("PASS events: an abandoned event, acceptance again, and later ids that cannot be overtaken");
}

// A receiver that starts over: the sender learns of it from the ACK's consent epoch, and an ACK of the old half,
// arriving late, cannot roll the new assembly back.
static void test_receiver_reset(int delayed_ack) {
    expected_n = 3072, expect_pattern = 1;   // (three parts: the third is the one held back)
    for (size_t i = 0; i < sizeof expected; i++) expected[i] = (uint8_t)(i * 11u + 3);
    lose_parts = 1u << 2;   // (the third part never gets through: the event stays unfinished, and the queue keeps it)
    assert(coop_sess_send_event(a, 2, COOP_K_TEST, expected, expected_n) == COOP_EVENT_QUEUED);
    pump(200, 0, 0);
    assert(!received && b->event_in[1].parts == 0x03 && a->event_out[0].target[2].acked == 0x03);
    const uint64_t old_epoch = b->event_in[1].consent_epoch, old_id = b->event_in[1].id;
    const uint16_t old_parts = b->event_in[1].parts;
    grant(b, 0);
    events_tick(b, clock_ms);   // (the half it had is thrown away; the sender still believes it was thanked)
    assert(!b->event_in[1].parts);
    grant(b, 1u << 1);
    lose_parts = 0x03u;         // (what the sender re-sends for the new assembly, but the two it thinks it owes)
    pump(900, 0, 0);            // (its missing part arrives onto a brand new assembly; the rest of them do not)
    assert(!received);          // (one part of three is not an event)
    const uint16_t now_parts = a->event_out[0].target[2].acked;
    assert(now_parts && a->event_out[0].target[2].ack_epoch > old_epoch);   // (this assembly's mask, the old one forgotten)
    lose_parts = 0x03u;         // (and the parts it now asks for do not arrive either, so it stays open)
    if (delayed_ack) {
        pump(600, 0, 0);   // (the sender keeps asking for those two, and keeps getting nothing back but its own mask)
        uint8_t body[COOP_EVENT_ACK_SIZE];
        CoopEventAck ack = {.sender_session = 101, .id = old_id, .parts = old_parts, .epoch = old_epoch};
        const size_t n = coop_event_ack_pack(body, sizeof body, &ack);
        assert(n);
        send_data(b, 1, COOP_K_EVENT_ACK, body, n, pipe_a);   // (the old ACK, with its old epoch, arriving late)
        pump(300, 0, 0);
        assert(a->event_out[0].target[2].acked == now_parts);   // (it is not this assembly's answer: nothing is rolled back)
    }
    lose_parts = 0;
    pump(16000, delayed_ack ? 30 : 0, 0);
    assert(received == 1 && last_id == 1 && !queued());
    puts("PASS events: a receiver that starts over, and an ACK of its old half that cannot roll it back");
}

// Revoke and acceptance again with no stepping call in between: the first step after it still discards the assembly.
static void test_reaccept_between_steps(void) {
    queue_partial();
    grant(b, 0), grant(b, 1u << 1);   // (its consent epoch moved; nothing has stepped in between)
    pump(8, 0, 0);
    assert(!received && !b->event_in[1].parts);
    pump(16000, 30, 0);
    assert(received == 1 && last_id == 1 && !queued());
    puts("PASS events: revoke and acceptance again between steps still discard the old assembly");
}

static void test_codecs(void) {
    uint8_t bytes[COOP_EVENT_HEAD + COOP_EVENT_PART];
    CoopEventPart p = {.recipient_session = 7, .id = 8, .kind = COOP_K_TEST, .total = COOP_EVENT_MAX, .part = 15,
                       .bytes = expected}, got;
    size_t n = coop_event_part_pack(bytes, sizeof bytes, &p);
    assert(n == sizeof bytes && coop_event_part_parse(bytes, n, &got));
    assert(got.id == 8 && got.total == COOP_EVENT_MAX && got.part == 15);
    for (size_t i = 0; i < n; i++) assert(!coop_event_part_parse(bytes, i, &got));
    bytes[COOP_EVENT_HEAD - 1] = 16;   // (a part index the size it claims has no room for)
    assert(!coop_event_part_parse(bytes, n, &got));
    bytes[COOP_EVENT_HEAD - 1] = 15, bytes[0] = 2;   // (a recipient that is not the one it was for)
    assert(!coop_event_part_parse(bytes, n, &got));
    p.total = COOP_EVENT_MAX + 1;
    assert(!coop_event_part_pack(bytes, sizeof bytes, &p));
    assert(coop_event_parts(0) == 1 && coop_event_parts(1024) == 1 && coop_event_parts(1025) == 3 &&
           coop_event_parts(COOP_EVENT_MAX) == 0xFFFFu && !coop_event_parts(COOP_EVENT_MAX + 1));
    CoopEventAck ack = {.sender_session = 1, .id = 2, .parts = 3, .epoch = 4}, decoded;
    n = coop_event_ack_pack(bytes, sizeof bytes, &ack);
    assert(n == COOP_EVENT_ACK_SIZE && coop_event_ack_parse(bytes, n, &decoded));
    assert(decoded.epoch == 4);
    for (size_t i = 0; i < n; i++) assert(!coop_event_ack_parse(bytes, i, &decoded));
    assert(!coop_event_ack_parse(bytes, n + 1, &decoded));
    puts("PASS events: the event part and ACK codecs, their lengths and their refusals");
}

static int race_done;
static void race_sender(void *unused) {
    (void)unused;
    for (int i = 0; i < 2000; i++) {
        coop_sess_send_event(a, 2, COOP_K_TEST, &i, sizeof i);
        coop_sess_member_accepted(a, 2);
    }
    __atomic_store_n(&race_done, 1, __ATOMIC_RELEASE);
}
static void test_threads(void) {
    CoopThread t;
    assert(coop_thread_start(&t, race_sender, NULL));
    while (!__atomic_load_n(&race_done, __ATOMIC_ACQUIRE)) {
        pump(8, 0, 0);
        grant(a, 1u << 2);
    }
    assert(coop_thread_join(&t, 5000));
    pump(20000, 0, 0);
    assert(!queued());
    puts("PASS events: enqueueing from another thread while this one steps");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "no-loss")) test_delivery(0, 0);
    else if (!strcmp(argv[1], "loss")) test_delivery(30, 0);
    else if (!strcmp(argv[1], "reverse")) test_delivery(0, 1);
    else if (!strcmp(argv[1], "split")) test_split();
    else if (!strcmp(argv[1], "parts")) test_parts();
    else if (!strcmp(argv[1], "duplicate")) test_duplicate();
    else if (!strcmp(argv[1], "queue")) test_queue_and_departure();
    else if (!strcmp(argv[1], "acceptance")) test_acceptance();
    else if (!strcmp(argv[1], "cancel")) test_cancel(0);
    else if (!strcmp(argv[1], "cancel-accept")) test_cancel(1);
    else if (!strcmp(argv[1], "reset-ack")) test_receiver_reset(0);
    else if (!strcmp(argv[1], "reaccept-step")) test_reaccept_between_steps();
    else if (!strcmp(argv[1], "delayed-ack")) test_receiver_reset(1);
    else if (!strcmp(argv[1], "codec")) test_codecs();
    else if (!strcmp(argv[1], "threads")) test_threads();
    else assert(0);
    coop_sess_free(a), coop_sess_free(b);   // (a session closes the links it holds)
    coop_tcp_close(pipe_a), coop_tcp_close(pipe_b), coop_tcp_close(pipe_c);
    coop_tcp_close(link_c);
    coop_tcp_close(door_a), coop_tcp_close(door_b), coop_tcp_close(door_c);
    return 0;
}
