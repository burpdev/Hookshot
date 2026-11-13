// The co-op wire format (coop_proto.h): the frames between a hosting game and the games joined to it,
// the roster, the player messages and their envelope, the reliable event parts, the signatures and POS.
// Pure functions on bytes.
#include "coop_proto.h"

#include <string.h>

#include "../third_party/monocypher/monocypher.h"

// The length of the string at s, at most max.
static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

// ---- little-endian fields

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

// ---- reliable event parts and acknowledgements

int coop_event_kind(uint8_t kind) {
    return kind && kind != COOP_K_BYE && kind != COOP_K_EVENT && kind != COOP_K_EVENT_ACK;
}

uint16_t coop_event_parts(size_t total) {
    if (total > COOP_EVENT_MAX) return 0;
    unsigned count = total ? (unsigned)((total + COOP_EVENT_PART - 1) / COOP_EVENT_PART) : 1;
    return (uint16_t)((1u << count) - 1);
}

static size_t event_part_size(const CoopEventPart *p) {
    size_t offset = (size_t)p->part * COOP_EVENT_PART;
    size_t left = p->total - offset;
    return left < COOP_EVENT_PART ? left : COOP_EVENT_PART;
}

size_t coop_event_part_pack(uint8_t *out, size_t cap, const CoopEventPart *p) {
    uint16_t mask = coop_event_parts(p->total);
    if (!p->recipient_session || !p->id || !coop_event_kind(p->kind) || p->part >= 16 || !(mask & (1u << p->part))) return 0;
    size_t n = event_part_size(p);
    if (cap < COOP_EVENT_HEAD + n || (n && !p->bytes)) return 0;
    out[0] = 1, out[1] = p->kind;
    put64(out + 2, p->recipient_session), put64(out + 10, p->id);
    put16(out + 18, p->total), out[20] = p->part;
    if (n) memcpy(out + COOP_EVENT_HEAD, p->bytes, n);
    return COOP_EVENT_HEAD + n;
}

int coop_event_part_parse(const uint8_t *bytes, size_t n, CoopEventPart *out) {
    if (n < COOP_EVENT_HEAD || bytes[0] != 1) return 0;
    CoopEventPart p = {.kind = bytes[1], .recipient_session = get64(bytes + 2), .id = get64(bytes + 10),
                       .total = (uint16_t)get16(bytes + 18), .part = bytes[20], .bytes = bytes + COOP_EVENT_HEAD};
    uint16_t mask = coop_event_parts(p.total);
    if (!p.recipient_session || !p.id || !coop_event_kind(p.kind) || p.part >= 16 || !(mask & (1u << p.part)) ||
        n != COOP_EVENT_HEAD + event_part_size(&p)) return 0;
    *out = p;
    return 1;
}

size_t coop_event_ack_pack(uint8_t *out, size_t cap, const CoopEventAck *a) {
    if (cap < COOP_EVENT_ACK_SIZE || !a->sender_session || !a->id || !a->parts) return 0;
    out[0] = 2;
    put64(out + 1, a->sender_session), put64(out + 9, a->id), put16(out + 17, a->parts), put64(out + 19, a->epoch);
    return COOP_EVENT_ACK_SIZE;
}

int coop_event_ack_parse(const uint8_t *bytes, size_t n, CoopEventAck *out) {
    if (n != COOP_EVENT_ACK_SIZE || bytes[0] != 2) return 0;
    CoopEventAck a = {.sender_session = get64(bytes + 1), .id = get64(bytes + 9),
                     .parts = (uint16_t)get16(bytes + 17), .epoch = get64(bytes + 19)};
    if (!a.sender_session || !a.id || !a.parts) return 0;
    *out = a;
    return 1;
}

// ---- names

// The length of the valid UTF-8 character at p (n bytes left), 0 when the bytes there are not one: no overlong forms,
// no surrogates, nothing above U+10FFFF.
static size_t utf8_len(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!len || len > n) return 0;
    for (size_t i = 1; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;    // overlong
    if (c == 0xED && p[1] > 0x9F) return 0;    // a surrogate
    if (c == 0xF0 && p[1] < 0x90) return 0;    // overlong
    if (c == 0xF4 && p[1] > 0x8F) return 0;    // above U+10FFFF
    return len;
}

// A character a log or a console may act on: C0, DEL, C1 (U+0080-U+009F), and the bidi controls U+061C, U+200E,
// U+200F, U+202A-U+202E and U+2066-U+2069.
static int control_char(const uint8_t *p, size_t len) {
    if (len == 1) return p[0] < 0x20 || p[0] == 0x7F;
    if (len == 2) return (p[0] == 0xC2 && p[1] < 0xA0) || (p[0] == 0xD8 && p[1] == 0x9C);
    if (len == 3 && p[0] == 0xE2 && p[1] == 0x80) return p[2] == 0x8E || p[2] == 0x8F || (p[2] >= 0xAA && p[2] <= 0xAE);
    if (len == 3 && p[0] == 0xE2 && p[1] == 0x81) return p[2] >= 0xA6 && p[2] <= 0xA9;
    return 0;
}

// At most max bytes of raw as valid UTF-8 into out (cap bytes, NUL-terminated): a control character and each byte that is
// not valid UTF-8 become '?', and the text is cut before the character that would not fit.
static size_t text_clean(char *out, size_t cap, const uint8_t *raw, size_t n, size_t max) {
    size_t at = 0, i = 0;
    if (!cap) return 0;
    while (i < n) {
        size_t len = utf8_len(raw + i, n - i), take = len ? len : 1;
        int bad = !len || control_char(raw + i, len);
        if (at + (bad ? 1 : take) > max || at + (bad ? 1 : take) + 1 > cap) break;
        if (bad) out[at++] = '?';
        else memcpy(out + at, raw + i, len), at += len;
        i += take;
    }
    out[at] = 0;
    return at;
}

size_t coop_name_clean(char *out, size_t cap, const uint8_t *raw, size_t n) {
    return text_clean(out, cap, raw, n, COOP_NAME_MAX);
}

size_t coop_roster_pack(uint8_t *out, size_t cap, const CoopRoster *r) {
    size_t at = 1;
    if (!cap || r->n > COOP_MAX_MEMBERS) return 0;
    out[0] = r->n;
    for (int i = 0; i < r->n; i++) {
        size_t nlen = bounded_len(r->m[i].name, COOP_NAME_MAX);
        if (at + 11 + nlen > cap) return 0;
        out[at] = r->m[i].id, out[at + 1] = r->m[i].flags;
        put64(out + at + 2, r->m[i].session);
        out[at + 10] = (uint8_t)nlen;
        memcpy(out + at + 11, r->m[i].name, nlen);
        at += 11 + nlen;
    }
    return at;
}

int coop_roster_parse(const uint8_t *p, size_t n, CoopRoster *r, size_t *used) {
    if (n < 1 || p[0] > COOP_MAX_MEMBERS) return 0;
    size_t at = 1;
    memset(r, 0, sizeof *r);
    r->n = p[0];
    for (int i = 0; i < r->n; i++) {
        if (at + 11 > n) return 0;
        size_t nlen = p[at + 10];
        if (nlen > COOP_NAME_MAX || at + 11 + nlen > n) return 0;
        r->m[i].id = p[at], r->m[i].flags = p[at + 1];
        r->m[i].session = get64(p + at + 2);
        coop_name_clean(r->m[i].name, sizeof r->m[i].name, p + at + 11, nlen);
        at += 11 + nlen;
    }
    if (used) *used = at;
    return 1;
}

size_t coop_error_pack(uint8_t *out, size_t cap, const CoopError *e) {
    size_t tlen = bounded_len(e->text, sizeof e->text - 1);
    if (cap < 2 + tlen) return 0;
    out[0] = e->reason;
    out[1] = (uint8_t)tlen;
    memcpy(out + 2, e->text, tlen);
    return 2 + tlen;
}

int coop_error_parse(const uint8_t *p, size_t n, CoopError *e) {
    if (n < 2 || 2u + p[1] > n) return 0;
    memset(e, 0, sizeof *e);
    e->reason = p[0];
    text_clean(e->text, sizeof e->text, p + 2, p[1], sizeof e->text - 1);   // for the log: printable UTF-8 only
    return 1;
}

size_t coop_kick_pack(uint8_t *out, size_t cap, uint8_t member, uint64_t session) {
    if (cap < 9) return 0;
    out[0] = member;
    put64(out + 1, session);
    return 9;
}

int coop_kick_parse(const uint8_t *p, size_t n, uint8_t *member, uint64_t *session) {
    if (n != 9) return 0;
    *member = p[0];
    *session = get64(p + 1);
    return 1;
}

size_t coop_u32_pack(uint8_t *out, size_t cap, uint32_t v) {
    if (cap < 4) return 0;
    put32(out, v);
    return 4;
}

int coop_u32_parse(const uint8_t *p, size_t n, uint32_t *v) {
    if (n < 4) return 0;
    *v = get32(p);
    return 1;
}

// ---- frames between a hosting game and the games joined to it

size_t coop_frame_pack(uint8_t *out, size_t cap, const CoopFrame *f, const void *payload, size_t n) {
    if (n > COOP_MAX_PAYLOAD || cap < COOP_FRAME_HDR + n) return 0;
    out[0] = f->type, out[1] = f->src, out[2] = f->dst, out[3] = f->kind;
    put32(out + 4, (uint32_t)n);
    if (n) memmove(out + COOP_FRAME_HDR, payload, n);   // (the payload may already sit where it goes)
    return COOP_FRAME_HDR + n;
}

int coop_frame_parse(const uint8_t *d, size_t n, CoopFrame *f, const uint8_t **payload) {
    if (n < COOP_FRAME_HDR || n > COOP_FRAME_HDR + COOP_MAX_PAYLOAD) return 0;
    uint32_t len = get32(d + 4);
    if (len != n - COOP_FRAME_HDR) return 0;
    f->type = d[0], f->src = d[1], f->dst = d[2], f->kind = d[3];
    f->len = len;
    *payload = d + COOP_FRAME_HDR;
    return 1;
}

// HELLO: u8 protocol major, u64 session, u8 name length, name. WELCOME: u8 member, u8[16] room id, u8 protocol, roster.
size_t coop_hello_pack(uint8_t *out, size_t cap, const CoopHello *h) {
    size_t nlen = bounded_len(h->name, COOP_NAME_MAX);
    if (cap < 10 + nlen) return 0;
    out[0] = h->proto;
    put64(out + 1, h->session);
    out[9] = (uint8_t)nlen;
    memcpy(out + 10, h->name, nlen);
    return 10 + nlen;
}

int coop_hello_parse(const uint8_t *p, size_t n, CoopHello *h) {
    if (n < 10) return 0;
    size_t nlen = p[9];
    if (nlen > COOP_NAME_MAX || 10 + nlen > n) return 0;
    memset(h, 0, sizeof *h);
    h->proto = p[0];
    h->session = get64(p + 1);
    coop_name_clean(h->name, sizeof h->name, p + 10, nlen);
    return 1;
}

size_t coop_welcome_pack(uint8_t *out, size_t cap, const CoopWelcome *w) {
    if (cap < 18) return 0;
    out[0] = w->member;
    memcpy(out + 1, w->room_id, COOP_ROOM_ID_LEN);
    out[17] = w->proto;
    size_t k = coop_roster_pack(out + 18, cap - 18, &w->roster);
    return k ? 18 + k : 0;
}

int coop_welcome_parse(const uint8_t *p, size_t n, CoopWelcome *w) {
    if (n < 18) return 0;
    memset(w, 0, sizeof *w);
    w->member = p[0];
    memcpy(w->room_id, p + 1, COOP_ROOM_ID_LEN);
    w->proto = p[17];
    return coop_roster_parse(p + 18, n - 18, &w->roster, NULL);
}

// The room's 16 bytes, shown in the log as 8 hex characters: never a secret, only what says which room a signature
// was made for.
void coop_room_id_hex(const uint8_t id[COOP_ROOM_ID_LEN], char out[COOP_ROOM_TEXT + 1]) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[2 * i] = hex[id[i] >> 4], out[2 * i + 1] = hex[id[i] & 15];
    out[16] = 0;
}

// ---- player messages: the payload of a DATA frame
//   u8 envelope version (COOP_MSG_V1), u64 sender's session, u32 counter, u8 kind, bytes
// The hosting game stamps the sender's member id into the frame it forwards, so a member's session and counter are
// what says which join of the sender's a message is of, and a new join (a new session, the counter from 1) starts a
// new window of them. A message of a kind this build does not know is passed to no one; a later envelope version can
// carry other fields, and a new kind (a pose, say) only needs a number.

size_t coop_msg_pack(uint8_t *out, size_t cap, uint64_t session, uint32_t counter, uint8_t kind, const void *bytes, size_t n) {
    if (n > COOP_MAX_MESSAGE || cap < COOP_MSG_HDR + 1 + n) return 0;
    out[0] = COOP_MSG_V1;
    put64(out + 1, session);
    put32(out + 9, counter);
    out[COOP_MSG_HDR] = kind;
    if (n) memcpy(out + COOP_MSG_HDR + 1, bytes, n);
    return COOP_MSG_HDR + 1 + n;
}

int coop_msg_parse(const uint8_t *p, size_t n, uint64_t *session, uint32_t *counter, uint8_t *kind,
                   const uint8_t **bytes, size_t *nout) {
    if (n < COOP_MSG_HDR + 1 || p[0] != COOP_MSG_V1) return 0;
    *session = get64(p + 1);
    *counter = get32(p + 9);
    *kind = p[COOP_MSG_HDR];
    *bytes = p + COOP_MSG_HDR + 1;
    *nout = n - COOP_MSG_HDR - 1;
    return 1;
}

// ---- signatures

void coop_sign_keypair(const uint8_t seed[32], uint8_t sk[COOP_SK_LEN], uint8_t pk[COOP_PK_LEN]) {
    uint8_t s[32];
    memcpy(s, seed, sizeof s);   // the library wipes the seed it is given
    crypto_eddsa_key_pair(sk, pk, s);
}

void coop_sign(uint8_t sig[COOP_SIG_LEN], const uint8_t sk[COOP_SK_LEN], const void *msg, size_t n) {
    crypto_eddsa_sign(sig, sk, msg, n);
}

int coop_sign_check(const uint8_t sig[COOP_SIG_LEN], const uint8_t pk[COOP_PK_LEN], const void *msg, size_t n) {
    return crypto_eddsa_check(sig, pk, msg, n) == 0;
}

void coop_blake2b(uint8_t *out, size_t outlen, const void *key, size_t klen, const void *msg, size_t n) {
    crypto_blake2b_keyed(out, outlen, key, klen, msg, n);
}

// ---- POS

static void putf(uint8_t *p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}

// The f32 at p into *f; 0 when it is an infinity or a NaN (all exponent bits set), which a pose never holds.
static int getf(const uint8_t *p, float *f) {
    uint32_t v = get32(p);
    memcpy(f, &v, 4);
    return (v & 0x7f800000u) != 0x7f800000u;
}

// 1 when p[0..n) is printable ASCII: a POS's names reach the log and the game's animation lookup.
static int printable(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7e) return 0;
    return 1;
}

enum { POS_HEAD = 11 };   // version, pseq, t_ms, flags, area length

size_t coop_pos_pack(uint8_t *out, size_t cap, const CoopPos *p) {
    size_t alen = bounded_len(p->area, COOP_POS_TEXT), n = POS_HEAD + alen + 17;
    int slots = p->nslots < COOP_POS_SLOTS ? p->nslots : COOP_POS_SLOTS;
    for (int i = 0; i < slots; i++) n += 1 + bounded_len(p->slot[i].node, COOP_POS_TEXT) + 4;
    if (cap < n) return 0;
    out[0] = COOP_POS_VERSION;
    put32(out + 1, p->pseq);
    put32(out + 5, p->t_ms);
    out[9] = p->flags, out[10] = (uint8_t)alen;
    memcpy(out + POS_HEAD, p->area, alen);
    uint8_t *q = out + POS_HEAD + alen;
    for (int k = 0; k < 3; k++) putf(q + 4 * k, p->pos[k]);
    putf(q + 12, p->yaw);
    q[16] = (uint8_t)slots, q += 17;
    for (int i = 0; i < slots; i++) {
        size_t len = bounded_len(p->slot[i].node, COOP_POS_TEXT);
        q[0] = (uint8_t)len;
        memcpy(q + 1, p->slot[i].node, len);
        putf(q + 1 + len, p->slot[i].frame);
        q += 1 + len + 4;
    }
    return n;
}

int coop_pos_parse(const uint8_t *p, size_t n, CoopPos *out) {
    if (n < POS_HEAD || p[0] != COOP_POS_VERSION || p[10] > COOP_POS_TEXT || n < POS_HEAD + p[10] + 17u ||
        !printable(p + POS_HEAD, p[10]))
        return 0;
    CoopPos r;
    memset(&r, 0, sizeof r);
    r.pseq = get32(p + 1), r.t_ms = get32(p + 5), r.flags = p[9];
    memcpy(r.area, p + POS_HEAD, p[10]);
    size_t at = POS_HEAD + (size_t)p[10];
    int finite = 1;
    for (int k = 0; k < 3; k++) finite &= getf(p + at + 4 * k, &r.pos[k]);
    finite &= getf(p + at + 12, &r.yaw);
    r.nslots = p[at + 16], at += 17;
    if (!finite || r.nslots > COOP_POS_SLOTS) return 0;
    for (int i = 0; i < r.nslots; i++) {
        if (at >= n || p[at] > COOP_POS_TEXT || at + 1 + p[at] + 4 > n || !printable(p + at + 1, p[at]) ||
            !getf(p + at + 1 + p[at], &r.slot[i].frame))
            return 0;
        memcpy(r.slot[i].node, p + at + 1, p[at]);
        at += 1u + p[at] + 4;
    }
    if (at != n) return 0;
    *out = r;
    return 1;
}

int coop_pos_fresh(CoopPosSeen *s, uint64_t session, uint32_t pseq, uint64_t now_ms) {
    int again = !s->any || s->session != session || now_ms - s->at_ms >= COOP_POS_QUIET_MS;
    if (!again && pseq <= s->last) return 0;
    s->any = 1, s->session = session, s->last = pseq, s->at_ms = now_ms;
    return again ? 2 : 1;
}
