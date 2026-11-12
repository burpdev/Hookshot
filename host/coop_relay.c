/* The single-room form of scripts/coop_relay.py. Cookie and rejoin proof precede admission; DATA stays opaque.
   Fixed tables are allocated once at start. The thread never calls the game or allocates for a datagram. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "coop_net.h"
#include "coop_relay.h"
#include "coop.h"
#include "../gpu/vk/compat/hookshot_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hos_log(const char *fmt, ...);
int hos_ini_option(const char *key, int otherwise);

enum {
    RELAY_NETWORKS = 4096, RELAY_NET_SLOTS = 8192, RELAY_HISTORY = 4096,
    RELAY_AWAY = 8, RELAY_DROP = 15, RELAY_REJOIN = 60, RELAY_EMPTY = 600,
    RELAY_ROSTER = 5, RELAY_FAILED_TTL = 120, RELAY_POLL_MS = 20
};
typedef struct { double rate, burst, tokens, stamp; } RelayBucket;
typedef struct {
    int used, live, away, maker;
    uint64_t cid, session;
    uint32_t connect_seq;
    char name[COOP_NAME_MAX + 1];
    uint8_t token[COOP_COOKIE_LEN];
    CoopAddr addr;
    double last_seen;
    RelayBucket packets, bytes, home, away_challenges, home_rejoins;
} RelayMember;
typedef struct { uint8_t prefix[7], kind, state; RelayBucket bucket; } RelayNetwork;
typedef struct {
    uint64_t cid;
    CoopAddr addr;
    uint32_t seq;
    uint8_t locator[COOP_KEY_LEN], reason;
    double when;
} RelayFailed;
typedef struct { uint64_t cid; CoopAddr addr; } RelayBan;
typedef struct {
    double pkt_rate, pkt_burst, byte_rate, byte_burst, join_rate, join_burst;
    double challenge_rate, prefix_rate, prefix_burst, egress_rate, rejoin_rate;
} RelayLimits;
/* Names match the Python relay's counters, including counters used by the shared fake-clock tests. */
enum { R_RX, R_MALFORMED, R_BAD_TAG, R_NO_MEMBER, R_WRONG_ADDR, R_RATE, R_CHALLENGES, R_JOINS,
       R_REJOINS, R_LEAVES, R_DROPS, R_FORWARDED, R_ROOMS, R_KICKS, R_EGRESS, R_REPEATS, R_CHARGES, R_STATS };
struct CoopRelay {
    CoopUdp *socket;
    CoopThread thread;
    int stop;
    uint16_t port;
    uint8_t locator[COOP_KEY_LEN], cookie_key[32];
    uint64_t maker;
    CoopAddr maker_addr;
    uint32_t room, seq;
    int shared, empty;
    double empty_since, next_roster, again[2], next_prune;
    RelayMember members[COOP_MAX_MEMBERS + 1];
    RelayNetwork networks[RELAY_NET_SLOTS];
    unsigned network_count;
    RelayFailed failed[RELAY_HISTORY];
    unsigned failed_next;
    RelayBan banned[RELAY_HISTORY];
    unsigned banned_count;
    uint64_t sessions[RELAY_HISTORY];
    unsigned session_count, session_next;
    RelayBucket challenges, rejoins, egress;
    RelayLimits limits;
    uint64_t stats[R_STATS];
    /* Production sends on socket. The fixture supplies a sink to inspect the same state machine on a fake clock. */
    void (*emit)(struct CoopRelay *, const CoopAddr *, const uint8_t *, size_t);
    void *emit_user;
};

static void relay_bucket_init(RelayBucket *b, double rate, double burst, double now) {
    *b = (RelayBucket){rate, burst, burst, now};
}
static void relay_refill(RelayBucket *b, double now) {
    if (now > b->stamp) {
        b->tokens += (now - b->stamp) * b->rate;
        if (b->tokens > b->burst) b->tokens = b->burst;
        b->stamp = now;
    }
}
static int relay_take(RelayBucket *b, double amount, double now) {
    relay_refill(b, now);
    if (b->tokens < amount) return 0;
    b->tokens -= amount;
    return 1;
}
static int relay_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    unsigned diff = 0;
    for (size_t i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}
static void relay_prefix(const CoopAddr *a, uint8_t out[7]) {
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
    memset(out, 0, 7);
    if (!memcmp(a->ip, mapped, 12)) out[0] = 4, memcpy(out + 1, a->ip + 12, 3);
    else out[0] = 6, memcpy(out + 1, a->ip, 6);
}
static void relay_prune(CoopRelay *r, double now) {
    for (unsigned i = 0; i < RELAY_NET_SLOTS; i++) {
        RelayNetwork *n = &r->networks[i];
        if (n->state != 1) continue;
        relay_refill(&n->bucket, now);
        if (n->bucket.tokens >= n->bucket.burst) n->state = 2, r->network_count--;
    }
    for (unsigned i = 0; i < RELAY_HISTORY; i++)
        if (r->failed[i].reason && now - r->failed[i].when >= RELAY_FAILED_TTL) r->failed[i].reason = 0;
}
static RelayBucket *relay_network(CoopRelay *r, const CoopAddr *addr, unsigned kind, double now) {
    uint8_t prefix[7];
    relay_prefix(addr, prefix);
    uint32_t hash = kind;
    for (unsigned i = 0; i < 7; i++) hash = (hash ^ prefix[i]) * 16777619u;
    unsigned free_slot = RELAY_NET_SLOTS;
    for (unsigned i = 0; i < RELAY_NET_SLOTS; i++) {
        unsigned at = (hash + i) & (RELAY_NET_SLOTS - 1);
        RelayNetwork *n = &r->networks[at];
        if (n->state == 1) {
            if (n->kind == kind && !memcmp(n->prefix, prefix, 7)) return &n->bucket;
        } else {
            if (free_slot == RELAY_NET_SLOTS) free_slot = at;
            if (!n->state) break;
        }
    }
    if (r->network_count >= RELAY_NETWORKS) {
        relay_prune(r, now);
        if (r->network_count >= RELAY_NETWORKS) return NULL;
        return relay_network(r, addr, kind, now);
    }
    if (free_slot == RELAY_NET_SLOTS) return NULL;
    RelayNetwork *n = &r->networks[free_slot];
    memcpy(n->prefix, prefix, 7);
    n->kind = (uint8_t)kind, n->state = 1;
    relay_bucket_init(&n->bucket, kind == 1 ? r->limits.join_rate : r->limits.prefix_rate,
                      kind == 1 ? r->limits.join_burst : r->limits.prefix_burst, now);
    r->network_count++;
    return &n->bucket;
}
static int relay_challenge(CoopRelay *r, const CoopAddr *addr, RelayMember *m, double now) {
    if (m) {
        uint8_t a[7], b[7];
        relay_prefix(addr, a), relay_prefix(&m->addr, b);
        int home = !memcmp(a, b, 7);
        return relay_take(home ? &m->home : &m->away_challenges, 1, now) &&
               relay_take(home ? &m->home_rejoins : &r->rejoins, 1, now);
    }
    relay_refill(&r->challenges, now);
    if (r->challenges.tokens < 1) return 0; /* No network state for traffic refused globally. */
    RelayBucket *mine = relay_network(r, addr, 2, now);
    return mine && relay_take(mine, 1, now) && relay_take(&r->challenges, 1, now);
}
/* Cookies are opaque on the wire. Keyed BLAKE2b reuses the protocol's primitive instead of adding HMAC-SHA256.
   Address bytes, port, scope and minute are encoded explicitly, never a padded C struct. */
static void relay_cookie(CoopRelay *r, const CoopAddr *addr, int64_t minute, uint8_t out[16]) {
    uint8_t bytes[30];
    memcpy(bytes, addr->ip, 16);
    bytes[16] = (uint8_t)addr->port, bytes[17] = (uint8_t)(addr->port >> 8);
    coop_u32_pack(bytes + 18, 4, addr->scope);
    for (unsigned i = 0; i < 8; i++) bytes[22 + i] = (uint8_t)((uint64_t)minute >> (i * 8));
    coop_blake2b(out, 16, r->cookie_key, sizeof r->cookie_key, bytes, sizeof bytes);
}
static int relay_cookie_ok(CoopRelay *r, const CoopAddr *a, const uint8_t cookie[16], double now) {
    uint8_t current[16], previous[16];
    relay_cookie(r, a, (int64_t)(now / 60), current);
    relay_cookie(r, a, (int64_t)(now / 60) - 1, previous);
    return relay_equal(current, cookie, 16) | relay_equal(previous, cookie, 16);
}
static void relay_send(CoopRelay *r, const CoopAddr *addr, uint8_t type, uint32_t room, uint8_t dst,
                       const uint8_t *p, size_t n, const uint8_t *key) {
    uint8_t bytes[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = room, .dst = dst, .seq = ++r->seq};
    size_t len = coop_dgram_pack(bytes, sizeof bytes, &h, p, n, key);
    if (len) r->emit(r, addr, bytes, len);
}
static const char *relay_reason(unsigned reason) {
    switch (reason) {
    case COOP_E_NO_ROOM: return "no such room";
    case COOP_E_ROOM_FULL: return "room full";
    case COOP_E_PROTO: return "relay speaks protocol 2";
    case COOP_E_RATE: return "too many joins, try again later";
    case COOP_E_BAD_TAG: return "bad tag";
    case COOP_E_SERVER_FULL: return "this PC hosts only its own room";
    case COOP_E_ROOM_EXISTS: return "a room with this secret is open";
    case COOP_E_REMOVED: return "removed by the room's maker";
    default: return "a session used in this room before";
    }
}
static void relay_error(CoopRelay *r, const CoopAddr *addr, unsigned reason, uint64_t nonce,
                        const uint8_t *key, uint32_t room, uint8_t dst) {
    CoopError e = {.reason = (uint8_t)reason, .nonce = nonce};
    snprintf(e.text, sizeof e.text, "%s", relay_reason(reason));
    uint8_t p[80];
    size_t n = coop_error_pack(p, sizeof p, &e);
    relay_send(r, addr, COOP_T_ERROR, room, dst, p, n, key);
}
static CoopRoster relay_roster(CoopRelay *r) {
    CoopRoster roster = {0};
    for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) {
        RelayMember *m = &r->members[i];
        if (!m->used || !m->live) continue;
        CoopMemberInfo *out = &roster.m[roster.n++];
        out->id = (uint8_t)i, out->session = m->session;
        out->flags = (m->maker ? COOP_FLAG_MAKER : 0) | (m->away ? COOP_FLAG_AWAY : 0);
        memcpy(out->name, m->name, sizeof out->name);
    }
    return roster;
}
static unsigned relay_live(CoopRelay *r) {
    unsigned n = 0;
    for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) n += r->members[i].used && r->members[i].live;
    return n;
}
static void relay_roster_all(CoopRelay *r, RelayMember *skip, int changed, double now) {
    if (changed) r->again[0] = now + .25, r->again[1] = now + .75;
    CoopRoster roster = relay_roster(r);
    uint8_t p[1 + COOP_MAX_MEMBERS * (11 + COOP_NAME_MAX)];
    size_t n = coop_roster_pack(p, sizeof p, &roster);
    for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) {
        RelayMember *m = &r->members[i];
        if (m->used && m->live && m != skip) relay_send(r, &m->addr, COOP_T_ROSTER, r->room, (uint8_t)i, p, n, r->locator);
    }
}
static void relay_joined(CoopRelay *r, RelayMember *m, uint64_t nonce) {
    CoopJoined j = {.member = (uint8_t)(m - r->members), .nonce = nonce, .roster = relay_roster(r)};
    memcpy(j.token, m->token, sizeof j.token);
    uint8_t p[COOP_MAX_PAYLOAD];
    size_t n = coop_joined_pack(p, sizeof p, &j);
    relay_send(r, &m->addr, COOP_T_JOINED, r->room, j.member, p, n, r->locator);
}
static void relay_delete_room(CoopRelay *r) {
    hos_log("coop: relay: room %08x closed", r->room);
    r->room = 0;
    memset(r->members, 0, sizeof r->members);
    r->session_count = r->session_next = r->banned_count = 0;
    r->shared = 0;
}
static void relay_leave(CoopRelay *r, RelayMember *m, double now) {
    hos_log("coop: relay: room %08x: %s (member %u) left", r->room, m->name, (unsigned)(m - r->members));
    m->used = m->live = 0;
    r->stats[R_LEAVES]++;
    if (!relay_live(r)) {
        r->empty = 1, r->empty_since = now;
        if (!r->shared) { relay_delete_room(r); return; }
    }
    relay_roster_all(r, NULL, 1, now);
}
static int relay_session_used(CoopRelay *r, uint64_t session) {
    for (unsigned i = 0; i < r->session_count; i++) if (r->sessions[i] == session) return 1;
    return 0;
}
static void relay_use_session(CoopRelay *r, uint64_t session) {
    if (relay_session_used(r, session)) return;
    r->sessions[r->session_next] = session;
    r->session_next = (r->session_next + 1) % RELAY_HISTORY;
    if (r->session_count < RELAY_HISTORY) r->session_count++;
}
static unsigned relay_refusal(CoopRelay *r, const CoopConnect *c, const CoopAddr *addr, RelayMember *rejoin) {
    int same = !memcmp(c->locator, r->locator, COOP_KEY_LEN);
    if (c->intent == COOP_INTENT_JOIN && (!same || !r->room)) return COOP_E_NO_ROOM;
    if (c->intent == COOP_INTENT_CREATE && same && r->room && !rejoin) return COOP_E_ROOM_EXISTS;
    if (!same || !r->room) {
        if (!same || c->client_id != r->maker || !coop_addr_eq(addr, &r->maker_addr)) return COOP_E_SERVER_FULL;
        return 0;
    }
    for (unsigned i = 0; i < r->banned_count; i++)
        if (c->client_id == r->banned[i].cid || coop_addr_eq(addr, &r->banned[i].addr)) return COOP_E_REMOVED;
    /* Unlike Python's unbounded ban set, a full table closes new admissions. No removed player is forgotten. */
    if (!rejoin && r->banned_count == RELAY_HISTORY) return COOP_E_ROOM_FULL;
    if (relay_session_used(r, c->session) && (!rejoin || c->session != rejoin->session)) return COOP_E_SESSION_USED;
    return 0;
}
static void relay_connect(CoopRelay *r, const CoopHdr *h, const uint8_t *p, const uint8_t *data, size_t n,
                          const CoopAddr *addr, double now) {
    CoopConnect c;
    if (!coop_connect_parse(p, h->len, &c) || (c.intent != COOP_INTENT_CREATE && c.intent != COOP_INTENT_JOIN)) {
        r->stats[R_MALFORMED]++; return;
    }
    RelayMember *rejoin = NULL, *known = NULL;
    int same = r->room && !memcmp(c.locator, r->locator, COOP_KEY_LEN);
    uint8_t zero[16] = {0};
    for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) {
        RelayMember *m = &r->members[i];
        if (!m->used) continue;
        if (same && m->cid == c.client_id && memcmp(c.token, zero, 16) && relay_equal(m->token, c.token, 16)) rejoin = m;
        if (m->live && coop_addr_eq(&m->addr, addr)) known = m;
    }
    if ((!rejoin || !coop_addr_eq(&rejoin->addr, addr) || rejoin->session != c.session) && !relay_cookie_ok(r, addr, c.cookie, now)) {
        if (n < COOP_CONNECT_MIN || !relay_challenge(r, addr, rejoin, now)) return;
        uint8_t cookie[16], payload[24];
        relay_cookie(r, addr, (int64_t)(now / 60), cookie);
        coop_challenge_pack(payload, sizeof payload, c.nonce, cookie);
        r->stats[R_CHALLENGES]++;
        relay_send(r, addr, COOP_T_CHALLENGE, 0, 0, payload, sizeof payload, c.locator);
        return;
    }
    if (!coop_dgram_verify(data, n, c.locator)) {
        r->stats[R_BAD_TAG]++;
        relay_error(r, addr, COOP_E_BAD_TAG, c.nonce, c.locator, 0, 0); return;
    }
    if (same && known && known->cid == c.client_id && known->connect_seq == h->seq) {
        known->last_seen = now;
        relay_joined(r, known, c.nonce); return;
    }
    unsigned reason = relay_refusal(r, &c, addr, rejoin);
    if (reason) {
        for (unsigned i = 0; i < RELAY_HISTORY; i++) {
            RelayFailed *f = &r->failed[i];
            if (f->reason == reason && f->cid == c.client_id && f->seq == h->seq &&
                coop_addr_eq(&f->addr, addr) && !memcmp(f->locator, c.locator, COOP_KEY_LEN)) {
                r->stats[R_REPEATS]++;
                relay_error(r, addr, reason, c.nonce, c.locator, 0, 0); return;
            }
        }
    }
    RelayBucket *budget = relay_network(r, addr, 1, now);
    if (!budget || !relay_take(budget, 1, now)) {
        r->stats[R_RATE]++;
        relay_error(r, addr, COOP_E_RATE, c.nonce, c.locator, 0, 0); return;
    }
    r->stats[R_CHARGES]++;
    if (reason) {
        RelayFailed *f = &r->failed[r->failed_next];
        *f = (RelayFailed){.cid = c.client_id, .addr = *addr, .seq = h->seq, .reason = (uint8_t)reason, .when = now};
        memcpy(f->locator, c.locator, COOP_KEY_LEN);
        r->failed_next = (r->failed_next + 1) % RELAY_HISTORY;
        relay_error(r, addr, reason, c.nonce, c.locator, 0, 0); return;
    }
    if (!r->room) {
        do { if (!coop_random(&r->room, sizeof r->room)) return; } while (!r->room);
        r->next_roster = now + RELAY_ROSTER;
        r->stats[R_ROOMS]++;
        hos_log("coop: relay: room %08x created", r->room);
    }
    relay_use_session(r, c.session);
    RelayMember *m = rejoin;
    if (!m) {
        for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) if (!r->members[i].used) { m = &r->members[i]; break; }
        if (!m) {
            for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) {
                RelayMember *candidate = &r->members[i];
                if (!candidate->live && (!m || candidate->last_seen < m->last_seen)) m = candidate;
            }
        }
        if (!m) { relay_error(r, addr, COOP_E_ROOM_FULL, c.nonce, c.locator, 0, 0); return; }
        uint8_t token[16];
        if (!coop_random(token, sizeof token)) return;
        int maker = c.intent == COOP_INTENT_CREATE;
        for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) if (r->members[i].used) maker = 0;
        memset(m, 0, sizeof *m);
        m->used = 1, m->cid = c.client_id, m->maker = maker;
        memcpy(m->token, token, sizeof token);
        relay_bucket_init(&m->packets, r->limits.pkt_rate, r->limits.pkt_burst, now);
        relay_bucket_init(&m->bytes, r->limits.byte_rate, r->limits.byte_burst, now);
        relay_bucket_init(&m->home, 2, 8, now);
        relay_bucket_init(&m->away_challenges, 2, 8, now);
        double share = r->limits.rejoin_rate / COOP_MAX_MEMBERS;
        relay_bucket_init(&m->home_rejoins, share, share > 8 ? share : 8, now);
        if (!maker) r->shared = 1;
        r->stats[R_JOINS]++;
    } else if (!m->live) r->stats[R_REJOINS]++;
    m->addr = *addr, m->session = c.session, m->connect_seq = h->seq;
    memcpy(m->name, c.name[0] ? c.name : "Player", c.name[0] ? sizeof m->name : 7);
    m->last_seen = now, m->live = 1, m->away = 0;
    if (known && known != m) known->used = known->live = 0;
    r->empty = 0;
    hos_log("coop: relay: room %08x: %s joined as member %u (%u/8)", r->room, m->name, (unsigned)(m - r->members), relay_live(r));
    relay_joined(r, m, c.nonce);
    relay_roster_all(r, m, 1, now);
}
static void relay_handle(CoopRelay *r, const uint8_t *data, size_t n, const CoopAddr *addr, double now) {
    r->stats[R_RX]++;
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(data, n, &h, &p)) { r->stats[R_MALFORMED]++; return; }
    if (h.ver != COOP_PROTO) {
        if (n >= COOP_HDR_LEN + 10 + strlen(relay_reason(COOP_E_PROTO)) && relay_challenge(r, addr, NULL, now))
            relay_error(r, addr, COOP_E_PROTO, 0, NULL, 0, 0);
        return;
    }
    if (h.type == COOP_T_CONNECT) { relay_connect(r, &h, p, data, n, addr, now); return; }
    if (!r->room || h.room != r->room || h.src < 1 || h.src > COOP_MAX_MEMBERS) { r->stats[R_NO_MEMBER]++; return; }
    RelayMember *m = &r->members[h.src];
    if (!m->used || (!m->live && h.type != COOP_T_LEAVE)) { r->stats[R_NO_MEMBER]++; return; }
    if (!coop_addr_eq(&m->addr, addr)) { r->stats[R_WRONG_ADDR]++; return; }
    if (!relay_take(&m->packets, 1, now) || !relay_take(&m->bytes, (double)n, now)) { r->stats[R_RATE]++; return; }
    if (!coop_dgram_verify(data, n, r->locator)) { r->stats[R_BAD_TAG]++; return; }
    if (!m->live) { m->used = 0; return; }
    m->last_seen = now;
    if (m->away) { m->away = 0; relay_roster_all(r, NULL, 1, now); }
    if (h.type == COOP_T_PING && h.len >= 4) relay_send(r, addr, COOP_T_PONG, r->room, h.src, p, 4, r->locator);
    else if (h.type == COOP_T_LEAVE) relay_leave(r, m, now);
    else if (h.type == COOP_T_KICK && m->maker) {
        uint8_t id;
        uint64_t session;
        if (!coop_kick_parse(p, h.len, &id, &session) || id < 1 || id > COOP_MAX_MEMBERS || id == h.src) return;
        RelayMember *target = &r->members[id];
        if (!target->used || target->session != session) return;
        if (r->banned_count == RELAY_HISTORY) return; /* Admissions already closed, never evict a ban. */
        r->banned[r->banned_count++] = (RelayBan){target->cid, target->addr};
        r->stats[R_KICKS]++;
        if (target->live) relay_error(r, &target->addr, COOP_E_REMOVED, 0, r->locator, r->room, id);
        relay_leave(r, target, now);
    } else if (h.type == COOP_T_DATA) {
        unsigned targets = 0;
        for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++)
            targets += i != h.src && r->members[i].used && r->members[i].live && (h.dst == COOP_DST_ALL || h.dst == i);
        if (!targets) return;
        if (!relay_take(&m->packets, targets - 1, now) || !relay_take(&m->bytes, (double)n * (targets - 1), now)) {
            r->stats[R_RATE]++; return;
        }
        if (!relay_take(&r->egress, (double)n * targets, now)) { r->stats[R_EGRESS]++; return; }
        for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++)
            if (i != h.src && r->members[i].used && r->members[i].live && (h.dst == COOP_DST_ALL || h.dst == i))
                r->emit(r, &r->members[i].addr, data, n);
        r->stats[R_FORWARDED] += targets;
    }
}
static void relay_tick(CoopRelay *r, double now) {
    if (r->room) {
        int changed = 0, again = 0;
        for (unsigned i = 1; i <= COOP_MAX_MEMBERS; i++) {
            RelayMember *m = &r->members[i];
            if (!m->used) continue;
            double silent = now - m->last_seen;
            if (m->live && silent >= RELAY_DROP) {
                m->live = 0, changed = 1;
                r->stats[R_DROPS]++;
                hos_log("coop: relay: room %08x: %s (member %u) dropped, silent 15 s", r->room, m->name, i);
            } else if (m->live && !m->away && silent >= RELAY_AWAY) m->away = 1, changed = 1;
            else if (!m->live && silent >= RELAY_REJOIN) m->used = 0;
        }
        if (!relay_live(r) && !r->empty) r->empty = 1, r->empty_since = now;
        if (r->empty && now - r->empty_since >= (r->shared ? RELAY_EMPTY : RELAY_REJOIN)) relay_delete_room(r);
        else {
            for (unsigned i = 0; i < 2; i++) if (r->again[i] && now >= r->again[i]) r->again[i] = 0, again = 1;
            if (changed || again || now >= r->next_roster) {
                relay_roster_all(r, NULL, changed, now);
                r->next_roster = now + RELAY_ROSTER;
            }
        }
    }
    if (now >= r->next_prune) r->next_prune = now + 1, relay_prune(r, now);
}
static void relay_init(CoopRelay *r, double now) {
    r->limits = (RelayLimits){COOP_RELAY_PACKET_RATE, COOP_RELAY_PACKET_BURST, COOP_RELAY_BYTE_RATE,
        COOP_RELAY_BYTE_BURST, 1.0 / 3, 20, 5000, 20, 40, 8 << 20, 1000};
    relay_bucket_init(&r->challenges, r->limits.challenge_rate, r->limits.challenge_rate, now);
    relay_bucket_init(&r->rejoins, r->limits.rejoin_rate, r->limits.rejoin_rate, now);
    relay_bucket_init(&r->egress, r->limits.egress_rate, r->limits.egress_rate, now);
}
static void relay_udp_emit(CoopRelay *r, const CoopAddr *addr, const uint8_t *p, size_t n) {
    coop_udp_send(r->socket, addr, p, n);
}
static void relay_main(void *arg) {
    CoopRelay *r = arg;
    uint8_t bytes[COOP_MAX_DGRAM];
    double tick = 0;
    while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
        CoopAddr from;
        int n = coop_udp_recv(r->socket, bytes, sizeof bytes, &from, RELAY_POLL_MS);
        double now = (double)coop_now_ns() / 1e9;
        if (n > 0) relay_handle(r, bytes, (size_t)n, &from, now);
        if (now - tick >= .25) relay_tick(r, now), tick = now;
        if (n < 0) coop_sleep_ms(RELAY_POLL_MS); /* Socket errors cannot make an unbounded busy loop. */
    }
    coop_udp_close(r->socket);
    r->socket = NULL;
}
int coop_relay_enabled(void) {
    const char *on = getenv("HOS_COOP_HOST");
    return on ? !env_value_off(on) : hos_ini_option("coop_host", 0) != 0;
}
CoopRelay *coop_relay_start(uint16_t port, const uint8_t locator[COOP_KEY_LEN], uint64_t maker, uint16_t maker_port) {
    if (!locator || !maker || !maker_port) return NULL;
    CoopRelay *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    relay_init(r, (double)coop_now_ns() / 1e9);
    r->maker = maker;
    memcpy(r->locator, locator, COOP_KEY_LEN);
    if (!coop_random(r->cookie_key, sizeof r->cookie_key) || !coop_net_resolve("127.0.0.1", maker_port, &r->maker_addr)) { free(r); return NULL; }
    r->socket = coop_udp_open(port);
    if (!r->socket) { free(r); return NULL; }
    r->port = coop_udp_port(r->socket);
    r->emit = relay_udp_emit;
    if (!coop_thread_start(&r->thread, relay_main, r)) { coop_udp_close(r->socket); free(r); return NULL; }
    return r;
}
uint16_t coop_relay_port(const CoopRelay *r) { return r ? r->port : 0; }
int coop_relay_stop(CoopRelay **relay) {
    if (!relay || !*relay) return 1;
    CoopRelay *r = *relay;
    __atomic_store_n(&r->stop, 1, __ATOMIC_RELEASE);
    if (!coop_thread_join(&r->thread, COOP_RELAY_STOP_MS)) return 0;
    memset(r->cookie_key, 0, sizeof r->cookie_key);
    free(r);
    *relay = NULL;
    return 1;
}
const char *coop_relay_host_start(CoopRelay **relay, const char *server, const uint8_t locator[COOP_KEY_LEN],
                                  uint64_t maker, uint16_t maker_port, uint16_t *port) {
    if (!coop_relay_stop(relay)) return "the previous local relay is still stopping";
    char host[128] = "";
    uint16_t chosen = 24680;
    if (server[0] && !coop_server_parse(server, host, sizeof host, &chosen)) return "hosting needs a valid local relay address";
    CoopAddr addresses[16];
    unsigned n = coop_net_local_addresses(addresses, 16, chosen);
    int local = !host[0] || !strcmp(host, "localhost") || !strcmp(host, "127.0.0.1") || !strcmp(host, "::1");
    for (unsigned i = 0; i < n && !local; i++) {
        char text[64], ip[128];
        uint16_t ignored;
        coop_addr_str(&addresses[i], text, sizeof text);
        if (coop_server_parse(text, ip, sizeof ip, &ignored) && !strcmp(ip, host)) local = 1;
    }
    if (!local) return "hosting refused: coop_server must name this PC, not another host";
    *relay = coop_relay_start(chosen, locator, maker, maker_port);
    if (!*relay) return "cannot start the local relay (UDP port in use or system failure)";
    *port = coop_relay_port(*relay);
    hos_log("coop: hosting on this PC, UDP port %u; the room ends when this game leaves or closes", (unsigned)*port);
    for (unsigned i = 0; i < n; i++) {
        char text[64];
        coop_addr_str(&addresses[i], text, sizeof text);
        hos_log("coop: hosting: players on this network can try %s", text);
    }
    hos_log("coop: hosting: internet players need your public address and UDP port %u forwarded to this PC", (unsigned)*port);
    if (!n) hos_log("coop: hosting: no LAN address found; use this PC's network address and port %u", (unsigned)*port);
    return NULL;
}
