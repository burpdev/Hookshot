// host/coop_proto.c, co-op's wire format, with this fixture: the frames between a game and the room it hosts, the
// player messages they carry, the reliable events, the signatures, and BLAKE2b.
//
//   coop_proto_test <tests/coop_vectors.txt>   every check below; exit 1 when one fails
//   coop_proto_test --write-vectors            write those byte vectors (docs/coop.md, "The link and the wire")
//
// 1. Known answers: BLAKE2b keyed and unkeyed, a key's half and what it signs, the room id as text.
// 2. The byte vectors of tests/coop_vectors.txt: every frame and message parsed from them, its fields checked, and packed
//    again to the same bytes. A change to the codec that moves a byte shows here.
// 3. Round trips and refusals of every packer and parser: too short, too long, a bad version, a name with a byte that is
//    not printable ASCII, a number that is not finite, a roster of more members than a room holds.
// 4. 400000 random buffers, and the vectors with one byte changed, into every parser: no crash, and nothing accepted that
//    its own length rules would not allow.
// 5. The parts an event of a given size has.
#include "../host/coop_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop_proto: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---- bytes to hex and back (four buffers: a printf may name hex() twice on one line)
static char hexbuf[4][16384];
static const char *hex(const uint8_t *b, size_t n) {
    static const char d[] = "0123456789abcdef";
    static int slot;
    char *const buf = hexbuf[slot = (slot + 1) % 4];
    for (size_t i = 0; i < n && 2 * i + 1 < 16384; i++) buf[2 * i] = d[b[i] >> 4], buf[2 * i + 1] = d[b[i] & 15];
    buf[2 * n] = 0;
    return buf;
}
static size_t unhex(const char *text, uint8_t *out, size_t cap) {
    if (!strcmp(text, "-")) return 0;   /* a vector's empty field */
    size_t n = 0;
    while (*text && n < cap) {
        unsigned v = 0;
        if (sscanf(text, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        text += 2;
    }
    return n;
}
static uint64_t num(const char *text) { return strtoull(text, NULL, 10); }

// ---- 1. known answers
static void known_answers(void) {
    uint8_t out[64];

    /* BLAKE2b-512 of "abc", unkeyed: the same as every other BLAKE2b of it */
    coop_blake2b(out, 64, NULL, 0, "abc", 3);
    CHECK(!strcmp(hex(out, 64), "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"),
          "BLAKE2b-512(\"abc\") is %s", hex(out, 64));

    /* Keyed, 8 bytes out: what says who derived it (coop_state.c derives a room's keys with its id) */
    const uint8_t key[16] = { 0x71, 0xae, 0x9c, 0x34, 0x27, 0x87, 0x19, 0x5a, 0xfa, 0x15, 0xf9, 0x0e, 0xfd, 0x0e, 0x18, 0x6d };
    const uint8_t msg[26] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                              0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19 };
    coop_blake2b(out, 8, key, sizeof key, msg, sizeof msg);
    CHECK(strlen(hex(out, 8)) == 16, "a keyed BLAKE2b of 8 bytes is 16 hex: %s", hex(out, 8));
    char short8[17];
    strcpy(short8, hex(out, 8));
    coop_blake2b(out, 8, key, sizeof key, msg, sizeof msg);
    CHECK(!strcmp(hex(out, 8), short8), "the same key and message gave another answer");
    coop_blake2b(out, 8, msg, 1, msg, sizeof msg);   /* another key: another answer */
    CHECK(strcmp(hex(out, 8), short8) != 0, "a different key gave the same derivation");

    /* A key from a seed, and what it signs: the room's rules and each player's identity ride on this (host/coop_state.c) */
    uint8_t seed[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN], sig[COOP_SIG_LEN];
    for (size_t i = 0; i < sizeof seed; i++) seed[i] = (uint8_t)(i * 7 + 1);
    coop_sign_keypair(seed, sk, pk);
    const char signed_text[] = "hookshot/coop rules 3";
    coop_sign(sig, sk, signed_text, sizeof signed_text - 1);
    CHECK(coop_sign_check(sig, pk, signed_text, sizeof signed_text - 1) == 1, "the signature does not check");
    sig[0] ^= 1;
    CHECK(coop_sign_check(sig, pk, signed_text, sizeof signed_text - 1) == 0, "a changed signature checked");
    sig[0] ^= 1;
    CHECK(coop_sign_check(sig, pk, signed_text, sizeof signed_text - 2) == 0, "a shorter message checked");
    uint8_t other_pk[COOP_PK_LEN];
    memcpy(other_pk, pk, sizeof other_pk);
    other_pk[0] ^= 1;
    CHECK(coop_sign_check(sig, other_pk, signed_text, sizeof signed_text - 1) == 0, "another key's public half checked");

    /* The room's 16 bytes as the log and a message show them: 16 hex of the first eight */
    uint8_t room[COOP_ROOM_ID_LEN];
    for (size_t i = 0; i < sizeof room; i++) room[i] = (uint8_t)(0xf0 + i);
    char text[COOP_ROOM_TEXT + 1];
    coop_room_id_hex(room, text);
    CHECK(!strcmp(text, "f0f1f2f3f4f5f6f7"), "the room id reads %s", text);
}

// ---- 2. the byte vectors
static uint8_t vbuf[4096];

static int vector_frame(const char *name, const char *type, const char *src, const char *dst, const char *kind,
                        const char *payload, const char *frame) {
    uint8_t bytes[2048];
    const size_t n = unhex(frame, bytes, sizeof bytes);
    CoopFrame f;
    const uint8_t *payload_out = NULL;
    if (!coop_frame_parse(bytes, n, &f, &payload_out)) { CHECK(0, "%s: the frame does not parse", name); return 0; }
    CHECK(f.type == (uint8_t)num(type) && f.src == (uint8_t)num(src) && f.dst == (uint8_t)num(dst) &&
          f.kind == (uint8_t)num(kind), "%s: the header is %u %u %u %u, not %s %s %s %s", name, f.type, f.src, f.dst, f.kind,
          type, src, dst, kind);
    const size_t want = strcmp(payload, "-") ? strlen(payload) / 2 : 0;
    CHECK(f.len == want, "%s: the payload is %u bytes, not %zu", name, f.len, want);
    CHECK(!strcmp(hex(payload_out, f.len), strcmp(payload, "-") ? payload : ""), "%s: the payload is %s", name, hex(payload_out, f.len));
    uint8_t again[2048];
    CHECK(coop_frame_pack(again, sizeof again, &f, payload_out, f.len) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    CoopFrame bad;
    CHECK(!coop_frame_parse(bytes, n - 1, &bad, &payload_out), "%s: a frame cut short parsed", name);
    return 1;
}

static int vector_hello(const char *name, const char *proto, const char *session, const char *who, const char *payload) {
    uint8_t bytes[512];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    CoopHello h;
    if (!coop_hello_parse(bytes, n, &h)) { CHECK(0, "%s: HELLO does not parse", name); return 0; }
    CHECK(h.proto == (uint8_t)num(proto), "%s: protocol %u", name, h.proto);
    CHECK(h.session == num(session), "%s: session %s", name, session);
    CHECK(!strcmp(hex((const uint8_t *)h.name, strlen(h.name)), strcmp(who, "-") ? who : ""), "%s: name %s", name, h.name);
    uint8_t again[512];
    CHECK(coop_hello_pack(again, sizeof again, &h) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    CoopHello bad;
    CHECK(!coop_hello_parse(bytes, n - 1, &bad), "%s: a HELLO cut short parsed", name);
    return 1;
}

static int vector_welcome(const char *name, const char *member, const char *proto, const char *room, const char *roster_n,
                          const char *payload) {
    uint8_t bytes[2048];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    CoopWelcome w;
    if (!coop_welcome_parse(bytes, n, &w)) { CHECK(0, "%s: WELCOME does not parse", name); return 0; }
    CHECK(w.member == (uint8_t)num(member) && w.proto == (uint8_t)num(proto), "%s: member %u proto %u", name, w.member, w.proto);
    CHECK(!strcmp(hex(w.room_id, COOP_ROOM_ID_LEN), room), "%s: room %s", name, hex(w.room_id, COOP_ROOM_ID_LEN));
    CHECK(w.roster.n == (uint8_t)num(roster_n), "%s: the roster has %u members", name, w.roster.n);
    char id[COOP_ROOM_TEXT + 1];
    coop_room_id_hex(w.room_id, id);
    CHECK(strlen(id) == COOP_ROOM_TEXT, "%s: the room reads %s", name, id);
    uint8_t again[2048];
    const size_t packed = coop_welcome_pack(again, sizeof again, &w);
    CHECK(packed == n && !memcmp(again, bytes, n), "%s: packed again differs (%zu of %zu)", name, packed, n);
    CoopWelcome bad;
    CHECK(!coop_welcome_parse(bytes, n - 1, &bad), "%s: a WELCOME cut short parsed", name);
    return 1;
}

static int vector_roster(const char *name, const char *n_text, const char *payload) {
    uint8_t bytes[2048];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    CoopRoster r;
    size_t used = 0;
    if (!coop_roster_parse(bytes, n, &r, &used)) { CHECK(0, "%s: the roster does not parse", name); return 0; }
    CHECK(r.n == (uint8_t)num(n_text) && used == n, "%s: %u members in %zu bytes", name, r.n, used);
    for (int i = 0; i < r.n; i++) CHECK(r.m[i].id >= 1 && r.m[i].id <= COOP_MAX_MEMBERS, "%s: member %u of %u", name, r.m[i].id, i);
    uint8_t again[2048];
    CHECK(coop_roster_pack(again, sizeof again, &r) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}

static int vector_error(const char *name, const char *reason, const char *text, const char *payload) {
    uint8_t bytes[256];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    CoopError e;
    if (!coop_error_parse(bytes, n, &e)) { CHECK(0, "%s: ERROR does not parse", name); return 0; }
    CHECK(e.reason == (uint8_t)num(reason), "%s: reason %u", name, e.reason);
    CHECK(!strcmp(hex((const uint8_t *)e.text, strlen(e.text)), strcmp(text, "-") ? text : ""), "%s: text %s", name, e.text);
    uint8_t again[256];
    CHECK(coop_error_pack(again, sizeof again, &e) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}

static int vector_kick(const char *name, const char *member, const char *session, const char *payload) {
    uint8_t bytes[64];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    uint8_t id = 0;
    uint64_t sess = 0;
    if (!coop_kick_parse(bytes, n, &id, &sess)) { CHECK(0, "%s: KICK does not parse", name); return 0; }
    CHECK(id == (uint8_t)num(member) && sess == num(session), "%s: member %u session %s", name, id, session);
    uint8_t again[64];
    CHECK(coop_kick_pack(again, sizeof again, id, sess) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}

static int vector_u32(const char *name, const char *value, const char *payload) {
    uint8_t bytes[16];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    uint32_t v = 0;
    if (!coop_u32_parse(bytes, n, &v)) { CHECK(0, "%s: the token does not parse", name); return 0; }
    CHECK(v == (uint32_t)num(value), "%s: the token is %u", name, v);
    uint8_t again[16];
    CHECK(coop_u32_pack(again, sizeof again, v) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}

static int vector_msg(const char *name, const char *session, const char *counter, const char *kind, const char *message,
                      const char *payload) {
    uint8_t bytes[2048];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    uint64_t sess = 0;
    uint32_t cnt = 0;
    uint8_t k = 0;
    const uint8_t *body = NULL;
    size_t body_n = 0;
    if (!coop_msg_parse(bytes, n, &sess, &cnt, &k, &body, &body_n)) { CHECK(0, "%s: the envelope does not parse", name); return 0; }
    CHECK(sess == num(session) && cnt == (uint32_t)num(counter) && k == (uint8_t)num(kind), "%s: session %s counter %s kind %s",
          name, session, counter, kind);
    CHECK(!strcmp(hex(body, body_n), strcmp(message, "-") ? message : ""), "%s: the message is %s", name, hex(body, body_n));
    uint8_t again[2048];
    CHECK(coop_msg_pack(again, sizeof again, sess, cnt, k, body, body_n) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}



static int vector_event(const char *name, const char *kind, const char *recipient, const char *id, const char *total,
                        const char *part, const char *bytes_hex, const char *payload) {
    uint8_t bytes[2048];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    CoopEventPart e;
    if (!coop_event_part_parse(bytes, n, &e)) { CHECK(0, "%s: the event part does not parse", name); return 0; }
    CHECK(e.kind == (uint8_t)num(kind) && e.recipient_session == num(recipient) && e.id == num(id) &&
          e.total == (uint16_t)num(total) && e.part == (uint8_t)num(part), "%s: kind %s id %s total %s part %s",
          name, kind, id, total, part);
    const size_t want = strcmp(bytes_hex, "-") ? strlen(bytes_hex) / 2 : 0;
    CHECK(!strcmp(hex(e.bytes, want), strcmp(bytes_hex, "-") ? bytes_hex : ""), "%s: the part is %s", name, hex(e.bytes, want));
    uint8_t again[2048];
    CHECK(coop_event_part_pack(again, sizeof again, &e) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}

static int vector_event_ack(const char *name, const char *sender, const char *id, const char *parts, const char *epoch,
                            const char *payload) {
    uint8_t bytes[64];
    const size_t n = unhex(payload, bytes, sizeof bytes);
    CoopEventAck a;
    if (!coop_event_ack_parse(bytes, n, &a)) { CHECK(0, "%s: the event ACK does not parse", name); return 0; }
    CHECK(a.sender_session == num(sender) && a.id == num(id) && a.parts == (uint16_t)num(parts) && a.epoch == num(epoch),
          "%s: sender %s id %s parts %s epoch %s", name, sender, id, parts, epoch);
    uint8_t again[64];
    CHECK(coop_event_ack_pack(again, sizeof again, &a) == n && !memcmp(again, bytes, n), "%s: packed again differs", name);
    return 1;
}

static int vector_name_clean(const char *name, const char *raw_hex, const char *want_hex) {
    uint8_t raw[128], out[128], want[128];
    const size_t n = unhex(raw_hex, raw, sizeof raw);
    const size_t wn = unhex(want_hex, want, sizeof want);
    const size_t got = coop_name_clean((char *)out, sizeof out, raw, n);
    CHECK(got == wn && !memcmp(out, want, wn), "%s: cleaned to %s, not %s", name, hex(out, got), want_hex);
    return 1;
}

static int vector_sign(const char *name, const char *seed_hex, const char *msg_hex, const char *want_pk, const char *want_sig) {
    uint8_t seed[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN], sig[COOP_SIG_LEN], msg[256];
    const size_t mn = unhex(msg_hex, msg, sizeof msg);
    if (unhex(seed_hex, seed, sizeof seed) != 32) { CHECK(0, "%s: the seed is not 32 bytes", name); return 0; }
    coop_sign_keypair(seed, sk, pk);
    coop_sign(sig, sk, msg, mn);
    CHECK(!strcmp(hex(pk, COOP_PK_LEN), want_pk), "%s: the public key is %s", name, hex(pk, COOP_PK_LEN));
    CHECK(!strcmp(hex(sig, COOP_SIG_LEN), want_sig), "%s: the signature is %s", name, hex(sig, COOP_SIG_LEN));
    CHECK(coop_sign_check(sig, pk, msg, mn) == 1, "%s: the signature does not check", name);
    return 1;
}

static int vector_roomid(const char *name, const char *raw_hex, const char *want) {
    uint8_t raw[COOP_ROOM_ID_LEN];
    if (unhex(raw_hex, raw, sizeof raw) != COOP_ROOM_ID_LEN) { CHECK(0, "%s: not 16 bytes", name); return 0; }
    char text[COOP_ROOM_TEXT + 1];
    coop_room_id_hex(raw, text);
    CHECK(!strcmp(text, want), "%s: the room reads %s, not %s", name, text, want);
    return 1;
}

// ---- one line's tokens (the fixture walks the same buffer twice)
static char *next_token(char **at) {
    char *p = *at;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (!*p) { *at = p; return NULL; }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    if (*p) *p++ = 0;
    *at = p;
    return start;
}

static int check_vectors(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { CHECK(0, "cannot read the vectors file %s", path); return 1; }
    char line[8192];
    int lines = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *scan = line;
        const char *kind = next_token(&scan);
        if (!kind) continue;
        lines++;
        const char *v[12] = { 0 };   /* the vector's fields, in the order of its kind's line (its bytes last) */
        for (int i = 0; i < 11; i++) {
            v[i] = next_token(&scan);
            if (!v[i]) break;
        }
        int fields = 0;
        if (!strcmp(kind, "frame")) fields = 7;
        else if (!strcmp(kind, "hello")) fields = 5;
        else if (!strcmp(kind, "welcome")) fields = 6;
        else if (!strcmp(kind, "roster")) fields = 3;
        else if (!strcmp(kind, "error")) fields = 4;
        else if (!strcmp(kind, "kick")) fields = 4;
        else if (!strcmp(kind, "u32")) fields = 3;
        else if (!strcmp(kind, "msg")) fields = 6;
        else if (!strcmp(kind, "event")) fields = 8;
        else if (!strcmp(kind, "eventack")) fields = 6;
        else if (!strcmp(kind, "nameclean")) fields = 3;
        else if (!strcmp(kind, "sign")) fields = 5;
        else if (!strcmp(kind, "roomid")) fields = 3;
        else if (!strcmp(kind, "reject")) fields = 3;
        else { CHECK(0, "unknown vector kind %s", kind); continue; }
        if (!v[fields - 1] || v[fields]) { CHECK(0, "%s vector of the wrong number of fields", kind); continue; }

        if (!strcmp(kind, "frame")) vector_frame(v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
        else if (!strcmp(kind, "hello")) vector_hello(v[0], v[1], v[2], v[3], v[4]);
        else if (!strcmp(kind, "welcome")) vector_welcome(v[0], v[1], v[2], v[3], v[4], v[5]);
        else if (!strcmp(kind, "roster")) vector_roster(v[0], v[1], v[2]);
        else if (!strcmp(kind, "error")) vector_error(v[0], v[1], v[2], v[3]);
        else if (!strcmp(kind, "kick")) vector_kick(v[0], v[1], v[2], v[3]);
        else if (!strcmp(kind, "u32")) vector_u32(v[0], v[1], v[2]);
        else if (!strcmp(kind, "msg")) vector_msg(v[0], v[1], v[2], v[3], v[4], v[5]);
        else if (!strcmp(kind, "event")) vector_event(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        else if (!strcmp(kind, "eventack")) vector_event_ack(v[0], v[1], v[2], v[3], v[4], v[5]);
        else if (!strcmp(kind, "nameclean")) vector_name_clean(v[0], v[1], v[2]);
        else if (!strcmp(kind, "sign")) vector_sign(v[0], v[1], v[2], v[3], v[4]);
        else if (!strcmp(kind, "roomid")) vector_roomid(v[0], v[1], v[2]);
        else {
            const size_t n = unhex(v[2], vbuf, sizeof vbuf);
            CoopFrame fr;
            const uint8_t *pl = NULL;
            CoopHello he;
            CoopWelcome we;
            CoopRoster ro;
            CoopError er;
            CoopEventPart ep;
            CoopEventAck ea;
            uint64_t ss;
            uint32_t cc;
            uint8_t kk, id;
            const uint8_t *bb;
            size_t nn, used;
            int refused = 0;
            const char *what = v[0];   /* the parser that must refuse: frame, hello, welcome, roster, error, kick, u32, msg, event, eventack */
            if (!strcmp(what, "frame")) refused = !coop_frame_parse(vbuf, n, &fr, &pl);
            else if (!strcmp(what, "hello")) refused = !coop_hello_parse(vbuf, n, &he);
            else if (!strcmp(what, "welcome")) refused = !coop_welcome_parse(vbuf, n, &we);
            else if (!strcmp(what, "roster")) refused = !coop_roster_parse(vbuf, n, &ro, &used);
            else if (!strcmp(what, "error")) refused = !coop_error_parse(vbuf, n, &er);
            else if (!strcmp(what, "kick")) refused = !coop_kick_parse(vbuf, n, &id, &ss);
            else if (!strcmp(what, "u32")) refused = !coop_u32_parse(vbuf, n, &cc);
            else if (!strcmp(what, "msg")) refused = !coop_msg_parse(vbuf, n, &ss, &cc, &kk, &bb, &nn);
            else if (!strcmp(what, "event")) refused = !coop_event_part_parse(vbuf, n, &ep);
            else if (!strcmp(what, "eventack")) refused = !coop_event_ack_parse(vbuf, n, &ea);
            else CHECK(0, "reject vector of a parser this fixture does not know: %s", what);
            CHECK(refused, "%s: a bad %s was accepted", v[1], what);
        }
    }
    fclose(f);
    CHECK(lines > 40, "the vectors file has only %d vectors", lines);
    printf("coop_proto: %d byte vectors checked\n", lines);
    return 0;
}

// ---- 3. round trips and refusals
static void codecs(void) {
    uint8_t out[4096];

    /* a frame: its header, and a payload of its own */
    const char line[] = "P KokiriForest 1.00 2.00 3.00 0.500 1 0 0";
    const size_t n = coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_DATA, 3, COOP_DST_ALL, COOP_K_LINE, 0 }, line, sizeof line - 1);
    CHECK(n == COOP_FRAME_HDR + sizeof line - 1, "the frame is %zu bytes", n);
    CoopFrame back;
    const uint8_t *payload = NULL;
    CHECK(coop_frame_parse(out, n, &back, &payload) && back.type == COOP_F_DATA && back.src == 3 && back.dst == COOP_DST_ALL &&
          back.kind == COOP_K_LINE && back.len == sizeof line - 1 && !memcmp(payload, line, back.len), "the frame did not come back");
    CHECK(!coop_frame_pack(out, COOP_FRAME_HDR + 4, &(CoopFrame){ COOP_F_DATA, 3, 2, COOP_K_LINE, 0 }, line, sizeof line - 1),
          "a frame that does not fit the buffer was packed");
    static uint8_t big[COOP_MAX_PAYLOAD + 8];
    memset(big, 'x', sizeof big);
    CHECK(!coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_DATA, 3, 2, COOP_K_LINE, 0 }, big, COOP_MAX_PAYLOAD + 1),
          "a payload over COOP_MAX_PAYLOAD was packed");
    const size_t max_f = coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_DATA, 3, 2, COOP_K_LINE, 0 }, big, COOP_MAX_PAYLOAD);
    CHECK(max_f == COOP_MAX_FRAME && coop_frame_parse(out, COOP_MAX_FRAME, &back, &payload) && back.len == COOP_MAX_PAYLOAD,
          "the longest frame is %zu bytes", max_f);

    /* the name: what a player typed, kept to what can travel in a line of words */
    char name[COOP_NAME_MAX + 1];
    CHECK(coop_name_clean(name, sizeof name, (const uint8_t *)"Link", 4) == 4 && !strcmp(name, "Link"), "a plain name changed");
    coop_name_clean(name, sizeof name, (const uint8_t *)"Li\tnk\1\2", 7);
    CHECK(!strcmp(name, "Li?nk??"), "a control byte is not shown as ?: %s", name);
    coop_name_clean(name, sizeof name, (const uint8_t *)"", 0);
    CHECK(name[0] == 0, "an empty name became %s", name);
    const size_t long_n = coop_name_clean(name, sizeof name, (const uint8_t *)"012345678901234567890123456789", 30);
    CHECK(long_n == COOP_NAME_MAX && name[COOP_NAME_MAX] == 0, "a long name is not cut to %d: %zu", COOP_NAME_MAX, long_n);
    coop_name_clean(name, sizeof name, (const uint8_t *)"\xc3\xa9" "cole", 6);   /* two bytes of UTF-8: one character */
    CHECK(!strcmp(name, "\xc3\xa9" "cole"), "a name in UTF-8 changed: %s", name);
    coop_name_clean(name, sizeof name, (const uint8_t *)"\xff\xfe", 2);   /* not UTF-8 at all */
    CHECK(!strcmp(name, "??"), "bytes that are not UTF-8 are not shown as ??: %s", name);

    /* the roster: the host's list of the room, member 1 being the host */
    CoopRoster r = { 0 }, r2;
    r.n = 3;
    r.m[0] = (CoopMemberInfo){ 1, COOP_FLAG_MAKER, 0x1122334455667788ULL, "Host" };
    r.m[1] = (CoopMemberInfo){ 2, 0, 0x1020304050607080ULL, "Link" };
    r.m[2] = (CoopMemberInfo){ 3, COOP_FLAG_AWAY, 0x0f0e0d0c0b0a0908ULL, "Midna" };
    const size_t rn = coop_roster_pack(out, sizeof out, &r);
    size_t used = 0;
    CHECK(rn && coop_roster_parse(out, rn, &r2, &used) && r2.n == 3 && used == rn &&
          !strcmp(r2.m[2].name, "Midna") && r2.m[0].session == r.m[0].session && (r2.m[2].flags & COOP_FLAG_AWAY) &&
          (r2.m[0].flags & COOP_FLAG_MAKER), "the roster did not come back");
    CoopRoster too = { 0 };
    too.n = COOP_MAX_MEMBERS + 1;
    CHECK(!coop_roster_pack(out, sizeof out, &too), "a roster of %u members was packed", too.n);
    coop_roster_pack(out, sizeof out, &r);
    out[0] = COOP_MAX_MEMBERS + 1;   /* a roster that claims more members than a room holds */
    CHECK(!coop_roster_parse(out, rn, &r2, &used) || r2.n <= COOP_MAX_MEMBERS, "a roster claiming 9 members was kept");
    coop_roster_pack(out, sizeof out, &r);
    CHECK(!coop_roster_parse(out, rn - 1, &r2, &used), "a roster cut short parsed");
    CoopRoster full = { 0 };
    full.n = COOP_MAX_MEMBERS;
    for (int i = 0; i < COOP_MAX_MEMBERS; i++)
        full.m[i] = (CoopMemberInfo){ (uint8_t)(i + 1), (uint8_t)(i ? 0 : COOP_FLAG_MAKER), 0x1000ULL + i, "Player" };
    const size_t full_n = coop_roster_pack(out, sizeof out, &full);
    CHECK(full_n && coop_roster_parse(out, full_n, &r2, &used) && r2.n == COOP_MAX_MEMBERS && used == full_n,
          "a room full of %d players did not come back", COOP_MAX_MEMBERS);

    /* HELLO and WELCOME: a joiner's first frame and the host's answer */
    CoopHello h = { COOP_PROTO, 0x0102030405060708ULL, "Link" };
    const size_t hn = coop_hello_pack(out, sizeof out, &h);
    CoopHello h2;
    CHECK(hn == 10 + 4 && coop_hello_parse(out, hn, &h2) && h2.proto == COOP_PROTO && h2.session == h.session &&
          !strcmp(h2.name, "Link"), "HELLO did not come back");
    coop_hello_pack(out, sizeof out, &(CoopHello){ COOP_PROTO, 1, "" });
    CHECK(coop_hello_parse(out, 10, &h2) && h2.name[0] == 0, "a HELLO with no name did not come back");
    CoopHello long_h = { COOP_PROTO, 1, "012345678901234567890123" };   /* exactly COOP_NAME_MAX */
    CHECK(coop_hello_pack(out, sizeof out, &long_h) == 10 + COOP_NAME_MAX && coop_hello_parse(out, 10 + COOP_NAME_MAX, &h2) &&
          strlen(h2.name) == COOP_NAME_MAX, "a HELLO of the longest name is not %d bytes", 10 + COOP_NAME_MAX);
    CoopWelcome w = { 0 };
    w.member = 2, w.proto = COOP_PROTO;
    for (size_t i = 0; i < COOP_ROOM_ID_LEN; i++) w.room_id[i] = (uint8_t)(i + 1);
    w.roster = r;
    const size_t wn = coop_welcome_pack(out, sizeof out, &w);
    CoopWelcome w2;
    CHECK(wn == 18 + rn && coop_welcome_parse(out, wn, &w2) && w2.member == 2 && w2.roster.n == 3 &&
          w2.proto == COOP_PROTO && !memcmp(w2.room_id, w.room_id, COOP_ROOM_ID_LEN), "WELCOME did not come back");

    /* ERROR, KICK, the PING token */
    const char why_long[] = "update required: that game speaks another co-op protocol";
    const size_t why_n = coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_PROTO, "update required: that game speaks another co-op protocol" });
    CoopError e2;
    CHECK(why_n == 2 + strlen(why_long) && coop_error_parse(out, why_n, &e2) && e2.reason == COOP_E_PROTO &&
          !strcmp(e2.text, why_long), "ERROR did not come back (%zu bytes)", why_n);
    const size_t full_e = coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_CLOSED, "012345678901234567890123456789012345678901234567890123456789" });
    CHECK(full_e == 2 + 60 && coop_error_parse(out, full_e, &e2) && strlen(e2.text) == 60, "an ERROR of 60 bytes of text is %zu bytes", full_e);
    const size_t none_e = coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_ROOM_FULL, "" });
    CHECK(none_e == 2 && coop_error_parse(out, none_e, &e2) && e2.reason == COOP_E_ROOM_FULL && e2.text[0] == 0,
          "an ERROR with no text is %zu bytes", none_e);
    coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_REMOVED, "removed" });
    out[1] = 90;   /* a length that reaches past the frame */
    CHECK(!coop_error_parse(out, 9, &e2), "an ERROR whose text length is past its bytes parsed");
    uint8_t id = 0;
    uint64_t sess = 0;
    const size_t kn = coop_kick_pack(out, sizeof out, 7, 0xdeadbeefcafe1234ULL);
    CHECK(kn == 9 && coop_kick_parse(out, kn, &id, &sess) && id == 7 && sess == 0xdeadbeefcafe1234ULL, "KICK did not come back");
    uint32_t tok = 0;
    const size_t tn = coop_u32_pack(out, sizeof out, 0x1234abcd);
    CHECK(tn == 4 && coop_u32_parse(out, tn, &tok) && tok == 0x1234abcd, "the token did not come back");
    CHECK(!coop_u32_parse(out, 3, &tok), "a 3-byte token parsed");

    /* the envelope of a player message: who sent it, which of their messages this is, and what it says */
    const char text[] = "I Obj_Heart_Container 1";
    const size_t mn = coop_msg_pack(out, sizeof out, 0x0102030405060708ULL, 4, COOP_K_LINE, text, sizeof text - 1);
    uint64_t msess = 0;
    uint32_t mcount = 0;
    uint8_t mkind = 0;
    const uint8_t *mbytes = NULL;
    size_t mlen = 0;
    CHECK(mn == COOP_MSG_HDR + 1 + sizeof text - 1 &&
          coop_msg_parse(out, mn, &msess, &mcount, &mkind, &mbytes, &mlen) && msess == 0x0102030405060708ULL && mcount == 4 &&
          mkind == COOP_K_LINE && mlen == sizeof text - 1 && !memcmp(mbytes, text, mlen), "the envelope did not come back");
    out[0] = COOP_MSG_V1 + 1;   /* an envelope of a version this build does not read */
    CHECK(!coop_msg_parse(out, mn, &msess, &mcount, &mkind, &mbytes, &mlen), "an unknown envelope version parsed");
    out[0] = COOP_MSG_V1;
    CHECK(!coop_msg_parse(out, COOP_MSG_HDR, &msess, &mcount, &mkind, &mbytes, &mlen), "an envelope cut short parsed");
    CHECK(!coop_msg_pack(out, sizeof out, 1, 1, COOP_K_LINE, text, COOP_MAX_MESSAGE + 1), "a message over COOP_MAX_MESSAGE was packed");
    const size_t max_m = coop_msg_pack(out, sizeof out, 1, 1, COOP_K_TEST, out + 3000, COOP_MAX_MESSAGE);
    CHECK(max_m == COOP_MSG_HDR + 1 + COOP_MAX_MESSAGE && coop_msg_parse(out, max_m, &msess, &mcount, &mkind, &mbytes, &mlen) &&
          mkind == COOP_K_TEST, "the longest message is %zu bytes", max_m);
    coop_msg_pack(out, sizeof out, 1, 1, COOP_K_NOTICE, "", 0);
    CHECK(coop_msg_parse(out, COOP_MSG_HDR + 1, &msess, &mcount, &mkind, &mbytes, &mlen) && mlen == 0 && mkind == COOP_K_NOTICE,
          "an empty message did not come back");

    /* the reliable events: parts of a thing one player owes another */
    CHECK(coop_event_kind(COOP_K_SECTION) && coop_event_kind(COOP_K_LINE) && coop_event_kind(COOP_K_TEST),
          "coop_event_kind does not know an application kind");
    CHECK(!coop_event_kind(COOP_K_EVENT) && !coop_event_kind(COOP_K_EVENT_ACK) && !coop_event_kind(COOP_K_BYE) &&
          !coop_event_kind(0), "coop_event_kind takes a kind the session already carries");
    CHECK(coop_event_parts(0) == 1 && coop_event_parts(1) == 1 && coop_event_parts(COOP_EVENT_PART) == 1 &&
          coop_event_parts(COOP_EVENT_PART + 1) == 3 && coop_event_parts(1200) == 3 && coop_event_parts(4096) == 15 &&
          coop_event_parts(COOP_EVENT_MAX + 1) == 0, "the parts of 1200 bytes is %x", coop_event_parts(1200));
    static uint8_t event_bytes[1200];
    for (size_t i = 0; i < sizeof event_bytes; i++) event_bytes[i] = (uint8_t)('a' + i % 26);
    CoopEventPart ep = { 0x5566778899aabbccULL, 9, 1200, COOP_K_SECTION, 0, event_bytes };
    const size_t ehn = coop_event_part_pack(out, sizeof out, &ep);
    CoopEventPart ep2;
    CHECK(ehn == COOP_EVENT_HEAD + 1024 && coop_event_part_parse(out, ehn, &ep2) && ep2.id == 9 && ep2.kind == COOP_K_SECTION &&
          ep2.recipient_session == 0x5566778899aabbccULL && ep2.total == 1200 && ep2.part == 0 &&
          !memcmp(ep2.bytes, event_bytes, 1024), "the first event part did not come back");
    CoopEventPart ep3 = { 0x5566778899aabbccULL, 9, 1200, COOP_K_SECTION, 1, event_bytes + 1024 };
    const size_t ehn2 = coop_event_part_pack(out, sizeof out, &ep3);
    CHECK(ehn2 == COOP_EVENT_HEAD + 176 && coop_event_part_parse(out, ehn2, &ep2) && ep2.part == 1 &&
          !memcmp(ep2.bytes, event_bytes + 1024, 176), "the last event part did not come back");
    CHECK(!coop_event_part_pack(out, sizeof out, &(CoopEventPart){ 0x5566778899aabbccULL, 9, 1200, COOP_K_SECTION, 2, event_bytes }),
          "an event part of a total that has no third part was packed");
    CHECK(!coop_event_part_pack(out, sizeof out, &(CoopEventPart){ 0, 9, 12, COOP_K_SECTION, 0, event_bytes }),
          "an event part with no recipient was packed");
    CHECK(!coop_event_part_pack(out, sizeof out, &(CoopEventPart){ 1, 0, 12, COOP_K_SECTION, 0, event_bytes }),
          "an event part with no id was packed");
    CHECK(!coop_event_part_pack(out, sizeof out, &(CoopEventPart){ 1, 9, 12, COOP_K_EVENT_ACK, 0, event_bytes }),
          "an event part of a kind the session already carries was packed");
    CHECK(!coop_event_part_pack(out, sizeof out, &(CoopEventPart){ 1, 9, COOP_EVENT_MAX + 1, COOP_K_SECTION, 0, event_bytes }),
          "an event part of an event larger than COOP_EVENT_MAX was packed");
    CoopEventPart none = { 1, 1, 0, COOP_K_SECTION, 0, NULL };   /* an empty event: one empty part */
    const size_t none_n = coop_event_part_pack(out, sizeof out, &none);
    CHECK(none_n == COOP_EVENT_HEAD && coop_event_part_parse(out, none_n, &ep2) && ep2.total == 0 && ep2.part == 0,
          "an empty event did not come back as one empty part");
    CoopEventAck ea = { 0x0102030405060708ULL, 9, 0x0007, 0x1122334455667788ULL };
    const size_t ean = coop_event_ack_pack(out, sizeof out, &ea);
    CoopEventAck ea2;
    CHECK(ean == COOP_EVENT_ACK_SIZE && coop_event_ack_parse(out, ean, &ea2) && ea2.sender_session == ea.sender_session &&
          ea2.id == 9 && ea2.parts == 0x0007 && ea2.epoch == 0x1122334455667788ULL, "the event ACK did not come back");
    CHECK(!coop_event_ack_parse(out, COOP_EVENT_ACK_SIZE - 1, &ea2), "an event ACK cut short parsed");
}

// ---- 4. every parser, on bytes that are not frames
static uint32_t rng_state = 0x12345678u;
static uint32_t rng(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static void parse_any(const uint8_t *b, size_t n) {   /* what matters: no crash, and nothing outside its own rules */
    CoopFrame f;
    const uint8_t *pl = NULL;
    CoopHello h;
    CoopWelcome w;
    CoopRoster r;
    CoopError e;
    CoopEventPart ep;
    CoopEventAck ea;
    uint64_t s;
    uint32_t c;
    uint8_t k, id;
    const uint8_t *by;
    size_t nn, used;
    if (coop_frame_parse(b, n, &f, &pl)) {
        CHECK(f.len + COOP_FRAME_HDR == n, "a frame of %zu bytes reported %u payload bytes", n, f.len);
        /* the type byte is the session's to judge: coop.c answers what it knows and ignores what it does not */
    }
    /* Protocol numbers, member ids and roster sense are the session's to judge (host/coop.c: COOP_PROTO, w.member,
       roster_valid): a codec that read them would refuse a frame the session can still answer. What the codec must
       promise is that it never reads past the bytes it was given, and never fills a struct past what it holds. */
    if (coop_hello_parse(b, n, &h)) CHECK(strlen(h.name) <= COOP_NAME_MAX, "a HELLO with a name of %zu characters parsed", strlen(h.name));
    if (coop_welcome_parse(b, n, &w)) {
        CHECK(w.roster.n <= COOP_MAX_MEMBERS, "a WELCOME carrying %u members parsed", w.roster.n);
        for (int i = 0; i < w.roster.n; i++)
            CHECK(strlen(w.roster.m[i].name) <= COOP_NAME_MAX, "a WELCOME member with a name of %zu characters parsed", strlen(w.roster.m[i].name));
    }
    if (coop_roster_parse(b, n, &r, &used)) {
        CHECK(r.n <= COOP_MAX_MEMBERS && used <= n, "a roster of %u members in %zu of %zu bytes parsed", r.n, used, n);
        for (int i = 0; i < r.n; i++)
            CHECK(strlen(r.m[i].name) <= COOP_NAME_MAX, "a roster member with a name of %zu characters parsed", strlen(r.m[i].name));
    }
    if (coop_error_parse(b, n, &e)) CHECK(strlen(e.text) < sizeof e.text, "an ERROR with %zu bytes of text parsed", strlen(e.text));
    if (coop_kick_parse(b, n, &id, &s)) CHECK(n == 9, "a KICK of %zu bytes parsed", n);
    if (coop_u32_parse(b, n, &c)) CHECK(n >= 4, "a token of %zu bytes parsed", n);
    if (coop_msg_parse(b, n, &s, &c, &k, &by, &nn)) CHECK(nn + COOP_MSG_HDR + 1 == n && nn <= COOP_MAX_MESSAGE,
                                                          "a message of %zu bytes in a %zu-byte envelope parsed", nn, n);
    if (coop_event_part_parse(b, n, &ep)) {
        CHECK(ep.part < 16 && ep.total <= COOP_EVENT_MAX, "an event part of %u of %u bytes parsed", ep.part, ep.total);
        CHECK(coop_event_parts(ep.total) & (1u << ep.part), "an event part %u of an event of %u bytes parsed", ep.part, ep.total);
    }
    if (coop_event_ack_parse(b, n, &ea)) CHECK(n == COOP_EVENT_ACK_SIZE, "an event ACK of %zu bytes parsed", n);
    char clean[COOP_NAME_MAX + 1];
    const size_t cn = coop_name_clean(clean, sizeof clean, b, n);
    CHECK(cn <= COOP_NAME_MAX, "a cleaned name of %zu bytes", cn);
}

static void fuzz(const char *path) {
    for (int i = 0; i < 400000; i++) {
        const size_t n = 1 + rng() % 1200;
        for (size_t k = 0; k < n; k++) vbuf[k] = (uint8_t)(rng() >> (k & 3));
        parse_any(vbuf, n);
    }
    printf("coop_proto: 400000 random buffers walked\n");

    /* the real vectors, each with one byte changed: a parser may take it or not, and what it takes its own rules allow */
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[8192];
    int changed = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *scan = line;
        if (!next_token(&scan)) continue;   /* the vector's kind */
        const char *last = NULL, *tok;
        while ((tok = next_token(&scan)) != NULL) last = tok;
        if (!last || strlen(last) < 16) continue;
        const size_t n = unhex(last, vbuf, sizeof vbuf);
        if (!n) continue;
        for (size_t at = 0; at < n; at++) {
            const uint8_t was = vbuf[at];
            vbuf[at] = (uint8_t)(was ^ 0x81);
            parse_any(vbuf, n);
            vbuf[at] = was ^ 1;
            parse_any(vbuf, n);
            vbuf[at] = was;
            changed += 2;
        }
    }
    fclose(f);
    printf("coop_proto: %d changed vectors walked\n", changed);
}



int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--write-vectors")) {
        /* The byte vectors, made by these functions: tests/coop_vectors.txt. */
        uint8_t out[4096];
        printf("# Byte vectors of the co-op wire format, protocol %d. Written by `tests/coop_proto_test.c --write-vectors`,\n", COOP_PROTO);
        printf("# read back by it through tests/test_coop_proto.py: the codec is pinned to these bytes, so a change that\n");
        printf("# moves one shows as a failure here. Hex is lower case, \"-\" is empty. Fields are tokens; a name, an area and\n");
        printf("# a message are hex of their bytes; a POS's four numbers are hex of their IEEE-754 bits.\n");
        printf("#\n");
        printf("# frame <name> <type> <src> <dst> <kind> <payload> <frame>              parse, check the header, pack again\n");
        printf("# hello <name> <proto> <session> <name> <payload>                        a joiner's HELLO\n");
        printf("# welcome <name> <member> <proto> <room id> <roster members> <payload>  the host's answer\n");
        printf("# roster <name> <members> <payload>                                      the host's list of the room\n");
        printf("# error <name> <reason> <text> <payload>                                 why a join was refused\n");
        printf("# kick <name> <member> <session> <payload>                               the host removed one\n");
        printf("# u32 <name> <value> <payload>                                           a PING or PONG token\n");
        printf("# msg <name> <session> <counter> <kind> <message> <payload>              a player message in its envelope\n");
        printf("# event <name> <kind> <recipient session> <id> <total> <part> <bytes> <payload>\n");
        printf("# eventack <name> <sender session> <id> <parts> <epoch> <payload>         parts received, of this epoch\n");
        printf("# sign <name> <seed> <message> <public key> <signature>                   the key a seed gives, and what it signs\n");
        printf("# roomid <name> <16 bytes> <text>                                         the room as the log shows it\n");
        printf("# nameclean <name> <bytes> <cleaned>                                      a name from untrusted bytes\n");
        printf("# reject <parser> <name> <bytes>                                          the parser must refuse these\n");
        printf("\n");

        CoopRoster r = { 0 };
        r.n = 2;
        r.m[0] = (CoopMemberInfo){ 1, COOP_FLAG_MAKER, 0x1122334455667788ULL, "Host" };
        r.m[1] = (CoopMemberInfo){ 2, COOP_FLAG_AWAY, 0x1020304050607080ULL, "Mid" };
        const size_t rn = coop_roster_pack(out, sizeof out, &r);
        uint8_t roster[256];
        coop_roster_pack(roster, sizeof roster, &r);

        CoopFrame frames[] = { { COOP_F_HELLO, 0, 2, 0, 0 }, { COOP_F_WELCOME, 0, 2, 0, 0 }, { COOP_F_BYE, 4, 0, COOP_K_BYE, 0 } };
        coop_frame_pack(out, sizeof out, &frames[0], NULL, 0);
        printf("frame hello-empty %d 0 2 0 - %s\n", frames[0].type, hex(out, COOP_FRAME_HDR));
        coop_frame_pack(out, sizeof out, &frames[1], NULL, 0);
        printf("frame welcome-empty %d 0 2 0 - %s\n", frames[1].type, hex(out, COOP_FRAME_HDR));
        coop_frame_pack(out, sizeof out, &frames[2], NULL, 0);
        printf("frame bye %d 4 0 %d - %s\n", frames[2].type, frames[2].kind, hex(out, COOP_FRAME_HDR));
        coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_PING, 0, 2, 0, 4 }, "\x11\x22\x33\x44", 4);
        printf("frame ping %d 0 2 0 11223344 %s\n", COOP_F_PING, hex(out, COOP_FRAME_HDR + 4));
        const char line[] = "P KokiriForest 1.50 2.50 -3.50 0.250 1 0 0";
        coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_DATA, 3, COOP_DST_ALL, COOP_K_LINE, 0 }, line, sizeof line - 1);
        printf("frame data-line %d 3 %d %d %s %s\n", COOP_F_DATA, COOP_DST_ALL, COOP_K_LINE, hex((const uint8_t *)line, sizeof line - 1),
               hex(out, COOP_FRAME_HDR + sizeof line - 1));
        coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_ROSTER, 0, COOP_DST_ALL, 0, (uint32_t)rn }, roster, rn);
        printf("frame roster %d 0 %d 0 %s %s\n", COOP_F_ROSTER, COOP_DST_ALL, hex(roster, rn), hex(out, COOP_FRAME_HDR + rn));
        const size_t err_n = coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_ROOM_FULL, "the room is full" });
        coop_frame_pack(out + 1500, 1024, &(CoopFrame){ COOP_F_ERROR, 0, 5, 0, (uint32_t)err_n }, out, err_n);
        printf("frame error %d 0 5 0 %s %s\n", COOP_F_ERROR, hex(out, err_n), hex(out + 1500, COOP_FRAME_HDR + err_n));

        const CoopHello hello = { COOP_PROTO, 0x0102030405060708ULL, "Link" };
        printf("hello link %u %llu %s %s\n", hello.proto, (unsigned long long)hello.session, hex((const uint8_t *)"Link", 4),
               hex(out, coop_hello_pack(out, sizeof out, &hello)));
        const CoopHello anon = { COOP_PROTO, 0x0f1e2d3c4b5a6978ULL, "" };
        printf("hello no-name %u %llu - %s\n", anon.proto, (unsigned long long)anon.session, hex(out, coop_hello_pack(out, sizeof out, &anon)));
        const CoopHello long_name = { COOP_PROTO, 0x1111222233334444ULL, "012345678901234567890123" };   /* the longest a room shows */
        const size_t ln_n = coop_hello_pack(out, sizeof out, &long_name);
        printf("hello long-name %u %llu %s %s\n", long_name.proto, (unsigned long long)long_name.session,
               hex((const uint8_t *)long_name.name, COOP_NAME_MAX), hex(out, ln_n));

        CoopRoster one = { 0 };
        one.n = 1;
        one.m[0] = (CoopMemberInfo){ 1, COOP_FLAG_MAKER, 0x1122334455667788ULL, "Host" };
        printf("roster host-only %u %s\n", one.n, hex(out, coop_roster_pack(out, sizeof out, &one)));
        printf("roster two %u %s\n", r.n, hex(out, coop_roster_pack(out, sizeof out, &r)));
        CoopRoster full = { 0 };
        full.n = COOP_MAX_MEMBERS;
        for (int i = 0; i < COOP_MAX_MEMBERS; i++)
            full.m[i] = (CoopMemberInfo){ (uint8_t)(i + 1), (uint8_t)(i ? COOP_FLAG_AWAY : COOP_FLAG_MAKER), 0x2000ULL + i, "Player" };
        printf("roster full %u %s\n", full.n, hex(out, coop_roster_pack(out, sizeof out, &full)));

        CoopWelcome w = { 0 };
        w.member = 2, w.proto = COOP_PROTO;
        for (size_t i = 0; i < COOP_ROOM_ID_LEN; i++) w.room_id[i] = (uint8_t)(0xf0 + i);
        w.roster = r;
        printf("welcome joined %u %u %s %u %s\n", w.member, w.proto, hex(w.room_id, COOP_ROOM_ID_LEN), w.roster.n,
               hex(out, coop_welcome_pack(out, sizeof out, &w)));
        CoopWelcome wall = { 0 };
        wall.member = 1, wall.proto = COOP_PROTO, wall.roster = full;
        for (size_t i = 0; i < COOP_ROOM_ID_LEN; i++) wall.room_id[i] = (uint8_t)(0x10 + i);
        printf("welcome full-room %u %u %s %u %s\n", wall.member, wall.proto, hex(wall.room_id, COOP_ROOM_ID_LEN), wall.roster.n,
               hex(out, coop_welcome_pack(out, sizeof out, &wall)));

        const char why[] = "update required: that game speaks another co-op protocol";
        printf("error protocol %d %s %s\n", COOP_E_PROTO, hex((const uint8_t *)why, strlen(why)),
               hex(out, coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_PROTO, "update required: that game speaks another co-op protocol" })));
        printf("error full %d %s %s\n", COOP_E_ROOM_FULL, hex((const uint8_t *)"the room is full", 16),
               hex(out, coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_ROOM_FULL, "the room is full" })));
        printf("error removed %d %s %s\n", COOP_E_REMOVED, hex((const uint8_t *)"the host removed you from the room", 34),
               hex(out, coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_REMOVED, "the host removed you from the room" })));
        printf("error no-text %d - %s\n", COOP_E_CLOSED, hex(out, coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_CLOSED, "" })));

        printf("kick seven 7 %llu %s\n", 0xdeadbeefcafe1234ULL, hex(out, coop_kick_pack(out, sizeof out, 7, 0xdeadbeefcafe1234ULL)));
        printf("kick one 1 %llu %s\n", 0ULL, hex(out, coop_kick_pack(out, sizeof out, 1, 0)));
        printf("u32 token %u %s\n", 0x1234abcd, hex(out, coop_u32_pack(out, sizeof out, 0x1234abcd)));
        printf("u32 zero 0 %s\n", hex(out, coop_u32_pack(out, sizeof out, 0)));
        printf("u32 max %u %s\n", 4294967295u, hex(out, coop_u32_pack(out, sizeof out, 0xffffffffu)));

        const char text[] = "I Obj_Heart_Container 1";
        printf("msg heart %llu 4 %u %s %s\n", 0x0102030405060708ULL, COOP_K_LINE, hex((const uint8_t *)text, sizeof text - 1),
               hex(out, coop_msg_pack(out, sizeof out, 0x0102030405060708ULL, 4, COOP_K_LINE, text, sizeof text - 1)));
        const char notice[] = "Midna joined the room";
        printf("msg notice %llu 12 %u %s %s\n", 0x5566778899aabbccULL, COOP_K_NOTICE, hex((const uint8_t *)notice, strlen(notice)),
               hex(out, coop_msg_pack(out, sizeof out, 0x5566778899aabbccULL, 12, COOP_K_NOTICE, notice, strlen(notice))));
        printf("msg empty %llu 0 %u - %s\n", 0x0102030405060708ULL, COOP_K_BYE, hex(out, coop_msg_pack(out, sizeof out, 0x0102030405060708ULL, 0, COOP_K_BYE, "", 0)));
        const char test[] = "TEST 1 2 3";
        printf("msg test %llu 7 %u %s %s\n", 0x1020304050607080ULL, COOP_K_TEST, hex((const uint8_t *)test, sizeof test - 1),
               hex(out, coop_msg_pack(out, sizeof out, 0x1020304050607080ULL, 7, COOP_K_TEST, test, sizeof test - 1)));

        const char body[] = "the room's rules, in its first part";
        CoopEventPart ep = { 0x5566778899aabbccULL, 9, (uint16_t)(sizeof body - 1), COOP_K_SECTION, 0, (const uint8_t *)body };
        printf("event section %u %llu %llu %u %u %s %s\n", ep.kind, (unsigned long long)ep.recipient_session, (unsigned long long)ep.id,
               ep.total, ep.part, hex((const uint8_t *)body, sizeof body - 1), hex(out, coop_event_part_pack(out, sizeof out, &ep)));
        CoopEventPart empty = { 0x0f1e2d3c4b5a6978ULL, 3, 0, COOP_K_DIGEST, 0, NULL };
        printf("event empty %u %llu %llu %u %u - %s\n", empty.kind, (unsigned long long)empty.recipient_session,
               (unsigned long long)empty.id, empty.total, empty.part, hex(out, coop_event_part_pack(out, sizeof out, &empty)));
        CoopEventAck ea = { 0x0102030405060708ULL, 9, 0x0007, 0x1122334455667788ULL };
        printf("eventack seven %llu %llu %u %llu %s\n", (unsigned long long)ea.sender_session, (unsigned long long)ea.id, ea.parts,
               (unsigned long long)ea.epoch, hex(out, coop_event_ack_pack(out, sizeof out, &ea)));
        CoopEventAck first = { 0x5566778899aabbccULL, 1, 1, 0 };
        printf("eventack first %llu %llu %u %llu %s\n", (unsigned long long)first.sender_session, (unsigned long long)first.id,
               first.parts, (unsigned long long)first.epoch, hex(out, coop_event_ack_pack(out, sizeof out, &first)));

        uint8_t seed[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN], sig[COOP_SIG_LEN];
        for (size_t i = 0; i < sizeof seed; i++) seed[i] = (uint8_t)(i * 7 + 1);
        coop_sign_keypair(seed, sk, pk);
        const char rules[] = "hookshot/coop rules 3";
        coop_sign(sig, sk, rules, sizeof rules - 1);
        printf("sign rules %s %s %s %s\n", hex(seed, 32), hex((const uint8_t *)rules, sizeof rules - 1), hex(pk, COOP_PK_LEN), hex(sig, COOP_SIG_LEN));
        for (size_t i = 0; i < sizeof seed; i++) seed[i] = (uint8_t)(31 - i);
        coop_sign_keypair(seed, sk, pk);
        const char counts[] = "pool 3 12 4";
        coop_sign(sig, sk, counts, sizeof counts - 1);
        printf("sign pool %s %s %s %s\n", hex(seed, 32), hex((const uint8_t *)counts, sizeof counts - 1), hex(pk, COOP_PK_LEN), hex(sig, COOP_SIG_LEN));

        uint8_t room[COOP_ROOM_ID_LEN];
        for (size_t i = 0; i < sizeof room; i++) room[i] = (uint8_t)(0xf0 + i);
        char room_text[COOP_ROOM_TEXT + 1];
        coop_room_id_hex(room, room_text);
        printf("roomid eight %s %s\n", hex(room, COOP_ROOM_ID_LEN), room_text);
        memset(room, 0, sizeof room);
        coop_room_id_hex(room, room_text);
        printf("roomid zero %s %s\n", hex(room, COOP_ROOM_ID_LEN), room_text);
        for (size_t i = 0; i < sizeof room; i++) room[i] = (uint8_t)(0xa5 - i);
        coop_room_id_hex(room, room_text);
        printf("roomid high %s %s\n", hex(room, COOP_ROOM_ID_LEN), room_text);

        printf("nameclean link %s %s\n", hex((const uint8_t *)"Link", 4), hex((const uint8_t *)"Link", 4));
        printf("nameclean control %s %s\n", hex((const uint8_t *)"Li\tnk\1\2", 7), hex((const uint8_t *)"Li?nk??", 7));
        printf("nameclean empty - -\n");
        char cut[COOP_NAME_MAX + 1];
        const size_t cut_n = coop_name_clean(cut, sizeof cut, (const uint8_t *)"012345678901234567890123456789", 30);
        printf("nameclean long %s %s\n", hex((const uint8_t *)"012345678901234567890123456789", 30), hex((const uint8_t *)cut, cut_n));
        char utf[COOP_NAME_MAX + 1];
        const size_t utf_n = coop_name_clean(utf, sizeof utf, (const uint8_t *)"\xc3\xa9" "cole", 6);
        printf("nameclean utf8 %s %s\n", hex((const uint8_t *)"\xc3\xa9" "cole", 6), hex((const uint8_t *)utf, utf_n));
        char junk[COOP_NAME_MAX + 1];
        const size_t junk_n = coop_name_clean(junk, sizeof junk, (const uint8_t *)"\xff\xfe\x00", 3);
        printf("nameclean not-utf8 %s %s\n", hex((const uint8_t *)"\xff\xfe\x00", 3), hex((const uint8_t *)junk, junk_n));

        /* near misses: real bytes of each format, cut short or given another version */
        coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_DATA, 3, COOP_DST_ALL, COOP_K_LINE, 0 }, text, 8);
        printf("reject frame short-payload %s\n", hex(out, COOP_FRAME_HDR + 7));
        coop_frame_pack(out, sizeof out, &(CoopFrame){ COOP_F_DATA, 3, COOP_DST_ALL, COOP_K_LINE, 0 }, text, 8);
        out[4] = 0;   /* a length that does not match the bytes there are */
        printf("reject frame wrong-length %s\n", hex(out, COOP_FRAME_HDR + 8));
        coop_hello_pack(out, sizeof out, &hello);
        printf("reject hello cut-short %s\n", hex(out, 10 + 4 - 1));
        coop_welcome_pack(out, sizeof out, &w);
        const size_t wn2 = coop_welcome_pack(out, sizeof out, &w);
        printf("reject welcome cut-short %s\n", hex(out, wn2 - 1));
        coop_roster_pack(out, sizeof out, &r);
        out[0] = COOP_MAX_MEMBERS + 1;   /* more members than a room holds */
        printf("reject roster nine-members %s\n", hex(out, rn));
        coop_kick_pack(out, sizeof out, 7, 0xdeadbeefcafe1234ULL);
        printf("reject kick cut-short %s\n", hex(out, 8));
        coop_u32_pack(out, sizeof out, 0x1234abcd);
        printf("reject u32 three-bytes %s\n", hex(out, 3));
        const size_t mn2 = coop_msg_pack(out, sizeof out, 0x0102030405060708ULL, 4, COOP_K_LINE, text, sizeof text - 1);
        out[0] = COOP_MSG_V1 + 1;   /* an envelope of a version this build does not read */
        printf("reject msg other-version %s\n", hex(out, mn2));
        coop_event_part_pack(out, sizeof out, &ep);
        const size_t ehn3 = coop_event_part_pack(out, sizeof out, &ep);
        printf("reject event cut-short %s\n", hex(out, ehn3 - 1));
        coop_event_part_pack(out, sizeof out, &ep);
        out[20] = 7;   /* a part the total has no room for */
        printf("reject event other-part %s\n", hex(out, ehn3));
        coop_event_ack_pack(out, sizeof out, &ea);
        printf("reject eventack cut-short %s\n", hex(out, COOP_EVENT_ACK_SIZE - 1));
        coop_error_pack(out, sizeof out, &(CoopError){ COOP_E_CLOSED, "closed" });
        out[1] = 40;   /* text longer than the bytes there are */
        printf("reject error long-text %s\n", hex(out, 8));
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: coop_proto_test <tests/coop_vectors.txt> | --write-vectors\n");
        return 2;
    }
    known_answers();
    codecs();
    check_vectors(argv[1]);
    fuzz(argv[1]);
    if (fails) {
        printf("coop_proto: %d checks failed\n", fails);
        return 1;
    }
    printf("PASS: host/coop_proto.c: the room's frames, the player messages, the events and the signatures\n");
    return 0;
}
