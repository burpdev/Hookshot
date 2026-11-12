// host/coop_proto.c, the co-op wire format, without the game: compiled by tests/test_coop_proto.py with Monocypher.
//
//   coop_proto_test <tests/coop_vectors.txt>   every check below; exit 1 when one fails
//   coop_proto_test --sealed                   the "sealed" vectors' payloads, made by this codec (Python's standard
//                                              library has no XChaCha20-Poly1305: tests/test_coop_relay.py keeps them)
//
// 1. Known answers: XChaCha20-Poly1305 (Monocypher's own test vector); sealing and opening (the largest message round
//    trips; a changed byte, another sender, receiver or room key opens nothing); EdDSA signatures (a changed byte or
//    another key does not check); random secrets (every one valid, every symbol about as often), how one is shown.
// 2. The byte vectors of tests/coop_vectors.txt, which scripts/coop_relay.py is checked against too: every message
//    parses to its fields and packs to the same bytes, every datagram's tag verifies, and every malformed one is refused.
// 3. Random bytes and changed copies of valid datagrams into every parser: nothing crashes, a POS that parses packs to the
//    same bytes, and a changed datagram's tag does not verify.
// 4. The POS counter (coop_pos_fresh): a pose sent again or older is dropped, also 35000 poses back; another session in
//    the member id starts again; a pose forged far ahead holds the sender's own out for COOP_POS_QUIET_MS only.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop_proto.h"
#include "monocypher.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop_proto: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define FAIL_RET(...) do { CHECK(0, __VA_ARGS__); return; } while (0)

static const char ROOM_SECRET[] = "H7K3WMBCDFGH234567";   // tests/coop_vectors.txt's room

// ---- hex and a random source

static int hex_digit(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// "-" is empty. The bytes into out (cap), their count, -1 when text is not hex or does not fit.
static int unhex(const char *text, uint8_t *out, size_t cap) {
    if (!text) return -1;
    if (!strcmp(text, "-")) return 0;
    size_t n = strlen(text);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hex_digit(text[2 * i]), b = hex_digit(text[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2);
}

static void print_hex(const uint8_t *p, size_t n) {
    if (!n) fputs("-", stdout);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) {
    rng_state ^= rng_state >> 12, rng_state ^= rng_state << 25, rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

// ---- 1. known answers, sealing, signatures, secrets

static void check_crypto(void) {
    // XChaCha20-Poly1305 (the library's lock) against a known answer from the library's own test vectors
    uint8_t key[32], nonce[24], out[17], ct[1];
    unhex("8019e8999c815c5723dbfbde05e6c71f118afc0dedb5b9f8dea398b2d764bca6", key, 32);
    unhex("8dfc023a9821939d389e38a072cf1b413bb1517c3fe83abe", nonce, 24);
    unhex("374190382975907a68e8a341faa0772aa0", out, 17);
    uint8_t plain = 0x86, tag[16];
    crypto_aead_lock(ct, tag, key, nonce, NULL, 0, &plain, 1);
    CHECK(!memcmp(tag, out, 16) && ct[0] == out[16], "XChaCha20-Poly1305 known answer");

    // seal and open: the largest message round trips; a changed byte, another sender, receiver or key opens nothing
    uint8_t lk[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN], other[COOP_MSG_KEY_LEN];
    coop_room_keys(ROOM_SECRET, lk, mk);
    coop_room_keys("23BCDF23BCDF23BCDF", NULL, other);
    static uint8_t msg[COOP_MAX_MESSAGE], box[COOP_MAX_PAYLOAD], got[COOP_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 13);
    size_t n = coop_seal(box, sizeof box, mk, 3, COOP_DST_ALL, 0x1234, 77, 0x42, msg, sizeof msg), ng = 0;
    uint8_t kind = 0;
    CHECK(n == COOP_MAX_PAYLOAD, "the largest message fills a datagram (%u)", (unsigned)n);
    CHECK(!coop_seal(box, sizeof box, mk, 3, COOP_DST_ALL, 1, 1, 0x42, msg, sizeof msg + 1), "a larger one is refused");
    CHECK(coop_open(box, n, mk, 3, COOP_DST_ALL, &kind, got, sizeof got, &ng) && kind == 0x42 && ng == sizeof msg && !memcmp(got, msg, ng),
          "sealed and opened");
    int opened = 0;
    for (size_t i = 0; i < n; i += 37) {
        box[i] ^= 0x10;
        opened += coop_open(box, n, mk, 3, COOP_DST_ALL, &kind, got, sizeof got, &ng);
        box[i] ^= 0x10;
    }
    CHECK(!opened, "a changed byte anywhere opens nothing (%d did)", opened);
    CHECK(!coop_open(box, n, mk, 4, COOP_DST_ALL, &kind, got, sizeof got, &ng) && !coop_open(box, n, mk, 3, 2, &kind, got, sizeof got, &ng) &&
              !coop_open(box, n, other, 3, COOP_DST_ALL, &kind, got, sizeof got, &ng),
          "another sender, receiver or room key opens nothing");

    // signatures: a signature checks with its key, not with another, and not over a changed message
    uint8_t seed[32], sk[COOP_SK_LEN], pk[COOP_PK_LEN], sk2[COOP_SK_LEN], pk2[COOP_PK_LEN], sig[COOP_SIG_LEN];
    for (int i = 0; i < 32; i++) seed[i] = (uint8_t)(i * 7 + 1);
    coop_sign_keypair(seed, sk, pk);
    seed[0] ^= 1;
    coop_sign_keypair(seed, sk2, pk2);
    coop_sign(sig, sk, msg, 200);
    int good = coop_sign_check(sig, pk, msg, 200), other_key = coop_sign_check(sig, pk2, msg, 200);
    msg[100] ^= 1;
    int changed = coop_sign_check(sig, pk, msg, 200);
    msg[100] ^= 1;
    sig[5] ^= 1;
    int bad_sig = coop_sign_check(sig, pk, msg, 200);
    CHECK(good && !other_key && !changed && !bad_sig, "EdDSA: a signature checks (%d), with another key (%d), over a changed message (%d), "
          "changed itself (%d)", good, other_key, changed, bad_sig);

    // secrets: from random bytes, every one valid, every symbol about as often
    unsigned counts[27] = {0}, made = 0;
    for (int i = 0; i < 2000; i++) {
        uint8_t r[32];
        char s[COOP_SECRET_LEN + 1], again[COOP_SECRET_LEN + 1];
        for (int k = 0; k < 32; k++) r[k] = (uint8_t)rnd32();
        if (!coop_secret_from_random(r, s)) continue;
        made++;
        CHECK(coop_secret_normalize(s, again) && !strcmp(s, again), "a new secret is one");
        for (int k = 0; k < COOP_SECRET_LEN; k++) counts[strchr("23456789BCDFGHJKMNPRSTVWXYZ", s[k]) - "23456789BCDFGHJKMNPRSTVWXYZ"]++;
    }
    unsigned lo = ~0u, hi = 0;
    for (int k = 0; k < 27; k++) lo = counts[k] < lo ? counts[k] : lo, hi = counts[k] > hi ? counts[k] : hi;
    CHECK(made > 1990 && lo > hi * 3 / 4, "random secrets: %u made, symbols %u..%u times", made, lo, hi);
    char text[COOP_SECRET_TEXT + 1], shortened[8];
    coop_secret_format(ROOM_SECRET, text);
    coop_secret_short(ROOM_SECRET, shortened);
    CHECK(!strcmp(text, "H7K3WM-BCDFGH-234567") && !strcmp(shortened, "H7K\xe2\x80\xa6"), "a secret is shown %s, logged %s", text, shortened);
}

// ---- 2. the byte vectors

static char *next_token(char **at) {
    char *p = *at;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    char *start = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *at = p;
    return start;
}

// "id:flags:session:namehex" entries into r; their count, -1 when malformed.
static int entries_parse(char **at, CoopRoster *r) {
    memset(r, 0, sizeof *r);
    char *t;
    while ((t = next_token(at))) {
        unsigned id, flags;
        unsigned long long session;
        char name_hex[80];
        if (r->n >= COOP_MAX_MEMBERS || sscanf(t, "%u:%u:%llx:%79s", &id, &flags, &session, name_hex) != 4) return -1;
        uint8_t raw[40];
        int k = unhex(name_hex, raw, sizeof raw);
        if (k < 0 || k > COOP_NAME_MAX) return -1;
        r->m[r->n].id = (uint8_t)id, r->m[r->n].flags = (uint8_t)flags, r->m[r->n].session = session;
        memcpy(r->m[r->n].name, raw, (size_t)k);
        r->n++;
    }
    return r->n;
}

static int roster_equal(const CoopRoster *a, const CoopRoster *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->m[i].id != b->m[i].id || a->m[i].flags != b->m[i].flags || a->m[i].session != b->m[i].session || strcmp(a->m[i].name, b->m[i].name))
            return 0;
    return 1;
}

static uint64_t hex64(const char *t) { return t ? strtoull(t, NULL, 16) : 0; }

static void check_vector(const char *kind, char *rest, int line) {
    static uint8_t a[1300], b[1300], c[1300];
    char *name = next_token(&rest);
    if (!name) FAIL_RET("vector line %d: no name", line);
    if (!strcmp(kind, "tag")) {   // tag <name> <key> <message> <BLAKE2b-64>: the datagram tag's primitive, keyed BLAKE2b with 8 bytes out
        int kn = unhex(next_token(&rest), a, 64), mn = unhex(next_token(&rest), b, sizeof b), tn = unhex(next_token(&rest), c, 8);
        uint8_t tag[8];
        if (kn < 0 || mn < 0 || tn != 8) FAIL_RET("vector line %d: bad tag line", line);
        crypto_blake2b_keyed(tag, 8, a, (size_t)kn, b, (size_t)mn);
        CHECK(!memcmp(tag, c, 8), "vector tag %s (line %d)", name, line);
    } else if (!strcmp(kind, "secret")) {   // secret <canonical|-> <text, the rest of the line>
        char out[COOP_SECRET_LEN + 1] = "";
        int ok = coop_secret_normalize(rest, out);
        CHECK(!strcmp(name, "-") ? !ok : ok && !strcmp(out, name), "vector secret \"%s\" is %s (line %d)", rest, name, line);
    } else if (!strcmp(kind, "keys")) {
        uint8_t lk[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN];
        coop_room_keys(name, lk, mk);
        CHECK(unhex(next_token(&rest), a, 16) == 16 && !memcmp(lk, a, 16) && unhex(next_token(&rest), b, 32) == 32 && !memcmp(mk, b, 32),
              "vector keys %s (line %d)", name, line);
    } else if (!strcmp(kind, "dgram") || !strcmp(kind, "badtag")) {
        int bad = !strcmp(kind, "badtag");
        char *secret = next_token(&rest), *tok[7] = {0}, *dg;
        if (!bad)
            for (int i = 0; i < 7; i++) tok[i] = next_token(&rest);
        dg = next_token(&rest);
        int n = unhex(dg, a, sizeof a);
        CoopHdr h;
        const uint8_t *payload;
        if (!secret || (!bad && !tok[6]) || n < 0 || !coop_dgram_parse(a, (size_t)n, &h, &payload))
            FAIL_RET("vector %s %s does not parse (line %d)", kind, name, line);
        uint8_t key[COOP_KEY_LEN];
        int keyed = strcmp(secret, "-") != 0;
        if (keyed) coop_room_keys(secret, key, NULL);
        if (bad) {
            CHECK(!coop_dgram_verify(a, (size_t)n, key), "vector badtag %s verifies (line %d)", name, line);
            return;
        }
        int pn = unhex(tok[6], b, sizeof b);
        CHECK(h.ver == atoi(tok[0]) && h.type == atoi(tok[1]) && h.room == (uint32_t)strtoull(tok[2], NULL, 10) && h.src == atoi(tok[3]) &&
                  h.dst == atoi(tok[4]) && h.seq == (uint32_t)strtoull(tok[5], NULL, 10) && pn == (int)h.len && (!pn || !memcmp(payload, b, (size_t)pn)),
              "vector dgram %s: header fields (line %d)", name, line);
        CHECK(keyed ? coop_dgram_verify(a, (size_t)n, key) : coop_dgram_tag_zero(a), "vector dgram %s: tag (line %d)", name, line);
        size_t k = coop_dgram_pack(c, sizeof c, &h, b, (size_t)(pn < 0 ? 0 : pn), keyed ? key : NULL);
        CHECK(k == (size_t)n && !memcmp(c, a, k), "vector dgram %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "connect")) {
        // connect <name> <intent> <flags> <locator> <cid> <session> <nonce> <namehex|-> <cookie> <token> <payload>
        char *t[10];
        for (int i = 0; i < 10; i++) t[i] = next_token(&rest);
        if (!t[9]) FAIL_RET("vector connect %s is cut (line %d)", name, line);
        uint8_t loc[16], cookie[16], token[16];
        int pn = unhex(t[9], a, sizeof a), nn = unhex(t[6], b, 32);
        CoopConnect cn;
        if (pn < 0 || nn < 0 || unhex(t[2], loc, 16) != 16 || unhex(t[7], cookie, 16) != 16 || unhex(t[8], token, 16) != 16 ||
            !coop_connect_parse(a, (size_t)pn, &cn))
            FAIL_RET("vector connect %s does not parse (line %d)", name, line);
        CHECK(cn.intent == atoi(t[0]) && cn.flags == atoi(t[1]) && !memcmp(cn.locator, loc, 16) && cn.client_id == hex64(t[3]) &&
                  cn.session == hex64(t[4]) && cn.nonce == hex64(t[5]) && strlen(cn.name) == (size_t)nn && !memcmp(cn.name, b, (size_t)nn) &&
                  !memcmp(cn.cookie, cookie, 16) && !memcmp(cn.token, token, 16),
              "vector connect %s: fields (line %d)", name, line);
        size_t k = coop_connect_pack(c, sizeof c, &cn);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector connect %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "challenge")) {
        char *payload = next_token(&rest), *nonce = next_token(&rest), *cookie = next_token(&rest);
        int pn = unhex(payload, a, sizeof a);
        uint64_t got;
        uint8_t ck[16], want[16];
        CHECK(pn > 0 && unhex(cookie, want, 16) == 16 && coop_challenge_parse(a, (size_t)pn, &got, ck) && got == hex64(nonce) && !memcmp(ck, want, 16) &&
                  coop_challenge_pack(c, sizeof c, got, ck) == (size_t)pn && !memcmp(c, a, (size_t)pn),
              "vector challenge %s (line %d)", name, line);
    } else if (!strcmp(kind, "roster") || !strcmp(kind, "joined")) {
        int joined = !strcmp(kind, "joined");
        char *payload = next_token(&rest);
        int pn = unhex(payload, a, sizeof a);
        char *member = joined ? next_token(&rest) : NULL, *nonce = joined ? next_token(&rest) : NULL, *token = joined ? next_token(&rest) : NULL;
        CoopRoster want;
        if (pn < 0 || entries_parse(&rest, &want) < 0 || (joined && !token)) FAIL_RET("vector %s %s is malformed (line %d)", kind, name, line);
        if (joined) {
            CoopJoined j;
            uint8_t tk[16];
            CHECK(unhex(token, tk, 16) == 16 && coop_joined_parse(a, (size_t)pn, &j) && j.member == atoi(member) && j.nonce == hex64(nonce) &&
                      !memcmp(j.token, tk, 16) && roster_equal(&j.roster, &want),
                  "vector joined %s: fields (line %d)", name, line);
            size_t k = coop_joined_pack(c, sizeof c, &j);
            CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector joined %s: packs to the same bytes (line %d)", name, line);
        } else {
            CoopRoster r;
            size_t used = 0;
            CHECK(coop_roster_parse(a, (size_t)pn, &r, &used) && used == (size_t)pn && roster_equal(&r, &want), "vector roster %s: fields (line %d)", name, line);
            size_t k = coop_roster_pack(c, sizeof c, &r);
            CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector roster %s: packs to the same bytes (line %d)", name, line);
        }
    } else if (!strcmp(kind, "error")) {
        char *payload = next_token(&rest), *reason = next_token(&rest), *nonce = next_token(&rest), *text = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), tn = unhex(text, b, 64);
        CoopError e;
        if (!reason || pn < 0 || tn < 0 || !coop_error_parse(a, (size_t)pn, &e)) FAIL_RET("vector error %s does not parse (line %d)", name, line);
        CHECK(e.reason == atoi(reason) && e.nonce == hex64(nonce) && strlen(e.text) == (size_t)tn && !memcmp(e.text, b, (size_t)tn),
              "vector error %s: fields (line %d)", name, line);
        size_t k = coop_error_pack(c, sizeof c, &e);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector error %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "kick")) {
        char *payload = next_token(&rest), *member = next_token(&rest), *session = next_token(&rest);
        int pn = unhex(payload, a, sizeof a);
        uint8_t m;
        uint64_t s;
        CHECK(member && pn > 0 && coop_kick_parse(a, (size_t)pn, &m, &s) && m == atoi(member) && s == hex64(session) &&
                  coop_kick_pack(c, sizeof c, m, s) == (size_t)pn && !memcmp(c, a, (size_t)pn),
              "vector kick %s (line %d)", name, line);
    } else if (!strcmp(kind, "sealed")) {
        // sealed <name> <secret> <src> <dst> <session> <counter> <kind> <message> <payload>
        char *t[8];
        for (int i = 0; i < 8; i++) t[i] = next_token(&rest);
        if (!t[7]) FAIL_RET("vector sealed %s is cut (line %d)", name, line);
        uint8_t mk[COOP_MSG_KEY_LEN], got_kind = 0;
        coop_room_keys(t[0], NULL, mk);
        int mn = unhex(t[6], b, sizeof b), pn = unhex(t[7], a, sizeof a);
        size_t ng = 0;
        uint8_t src = (uint8_t)atoi(t[1]), dst = (uint8_t)atoi(t[2]), kd = (uint8_t)strtoul(t[5], NULL, 0);
        if (mn < 0 || pn < 0) FAIL_RET("vector sealed %s is malformed (line %d)", name, line);
        CHECK(coop_open(a, (size_t)pn, mk, src, dst, &got_kind, c, sizeof c, &ng) && got_kind == kd && ng == (size_t)mn && !memcmp(c, b, ng),
              "vector sealed %s opens (line %d)", name, line);
        size_t k = coop_seal(c, sizeof c, mk, src, dst, hex64(t[3]), (uint32_t)strtoul(t[4], NULL, 0), kd, b, (size_t)mn);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector sealed %s: seals to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "pos")) {
        // pos <name> <payload> <pseq> <t_ms> <flags> <area hex|-> <x> <y> <z> <yaw> <node hex:frame>...
        char *payload = next_token(&rest), *pseq = next_token(&rest), *t_ms = next_token(&rest), *flags = next_token(&rest);
        char *area = next_token(&rest), *f[4];
        for (int i = 0; i < 4; i++) f[i] = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), an = area ? unhex(area, b, COOP_POS_TEXT) : -1;
        CoopPos p;
        if (pn < 0 || an < 0 || !f[3]) FAIL_RET("vector pos %s does not parse (line %d)", name, line);
        if (!coop_pos_parse(a, (size_t)pn, &p)) FAIL_RET("vector pos %s: does not parse (line %d)", name, line);
        int ok = p.pseq == (uint32_t)strtoul(pseq, NULL, 10) && p.t_ms == (uint32_t)strtoul(t_ms, NULL, 10) && p.flags == atoi(flags) &&
                 strlen(p.area) == (size_t)an && !memcmp(p.area, b, (size_t)an) && p.pos[0] == strtof(f[0], NULL) &&
                 p.pos[1] == strtof(f[1], NULL) && p.pos[2] == strtof(f[2], NULL) && p.yaw == strtof(f[3], NULL);
        int ns = 0;
        for (char *s; (s = next_token(&rest)); ns++) {
            char *colon = strchr(s, ':');
            if (!colon || ns >= COOP_POS_SLOTS) FAIL_RET("vector pos %s: bad slot %s (line %d)", name, s, line);
            *colon = 0;
            int nn = unhex(s, c, COOP_POS_TEXT);
            ok &= nn >= 0 && strlen(p.slot[ns].node) == (size_t)nn && !memcmp(p.slot[ns].node, c, (size_t)nn) &&
                  p.slot[ns].frame == strtof(colon + 1, NULL);
        }
        CHECK(ok && p.nslots == ns, "vector pos %s: fields (line %d)", name, line);
        size_t k = coop_pos_pack(c, sizeof c, &p);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector pos %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "reject")) {
        char *what = name, *nm = next_token(&rest), *bytes = next_token(&rest);
        int n = unhex(bytes, a, sizeof a);
        if (!nm || n < 0) FAIL_RET("vector reject %s: bad line (line %d)", what, line);
        CoopHdr h;
        const uint8_t *p;
        CoopConnect cn;
        CoopRoster r;
        CoopJoined j;
        CoopError e;
        CoopPos pos;
        uint64_t u64;
        uint32_t u32;
        uint8_t u8, ck[16];
        int parsed;
        if (!strcmp(what, "dgram")) parsed = coop_dgram_parse(a, (size_t)n, &h, &p);
        else if (!strcmp(what, "connect")) parsed = coop_connect_parse(a, (size_t)n, &cn);
        else if (!strcmp(what, "challenge")) parsed = coop_challenge_parse(a, (size_t)n, &u64, ck);
        else if (!strcmp(what, "roster")) parsed = coop_roster_parse(a, (size_t)n, &r, NULL);
        else if (!strcmp(what, "joined")) parsed = coop_joined_parse(a, (size_t)n, &j);
        else if (!strcmp(what, "error")) parsed = coop_error_parse(a, (size_t)n, &e);
        else if (!strcmp(what, "kick")) parsed = coop_kick_parse(a, (size_t)n, &u8, &u64);
        else if (!strcmp(what, "pos")) parsed = coop_pos_parse(a, (size_t)n, &pos);
        else if (!strcmp(what, "sealed")) parsed = coop_envelope_peek(a, (size_t)n, &u64, &u32);
        else FAIL_RET("vector reject: unknown parser %s (line %d)", what, line);
        CHECK(!parsed, "vector reject %s %s parses (line %d)", what, nm, line);
    } else {
        CHECK(0, "vector line %d: unknown kind %s", line, kind);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *text = malloc(cap);
    for (size_t got; text && (got = fread(text + n, 1, cap - n - 1, f)) > 0;) {
        n += got;
        if (n + 1 == cap) text = realloc(text, cap *= 2);
    }
    fclose(f);
    if (text) text[n] = 0;
    return text;
}

static int check_vectors(const char *path) {
    char *text = read_file(path);
    if (!text) {
        CHECK(0, "cannot read the vectors file %s", path);
        return 0;
    }
    int lines = 0, line = 0;
    for (char *p = text, *next; *p; p = next, line++) {
        next = strchr(p, '\n');
        if (next) *next++ = 0;
        else next = p + strlen(p);
        size_t len = strlen(p);
        if (len && p[len - 1] == '\r') p[len - 1] = 0;   // (a checkout with CRLF line endings)
        if (!*p || *p == '#') continue;
        char *rest = p;
        char *kind = next_token(&rest);
        if (!kind) continue;
        check_vector(kind, rest, line + 1);
        lines++;
    }
    free(text);
    CHECK(lines > 60, "the vectors file has %d lines of vectors", lines);
    printf("coop_proto: %d byte vectors checked\n", lines);
    return lines;
}

// ---- 3. random bytes into every parser

static void check_fuzz(void) {
    static uint8_t buf[1400], out[1400], opened[1400];
    uint8_t key[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN];
    coop_room_keys(ROOM_SECRET, key, mk);
    uint8_t seeds[8][400];
    size_t seed_len[8];
    CoopRoster r = {.n = 2};
    r.m[0] = (CoopMemberInfo){1, COOP_FLAG_MAKER, 5, "Alice"}, r.m[1] = (CoopMemberInfo){2, 1, 6, "Bob"};
    uint8_t payload[400];
    CoopHdr h = {.ver = COOP_PROTO, .room = 7, .src = 1, .dst = 2, .seq = 5};
    size_t n = coop_roster_pack(payload, sizeof payload, &r);
    h.type = COOP_T_ROSTER, seed_len[0] = coop_dgram_pack(seeds[0], sizeof seeds[0], &h, payload, n, key);
    CoopJoined j = {.member = 2, .nonce = 9, .roster = r};
    n = coop_joined_pack(payload, sizeof payload, &j);
    h.type = COOP_T_JOINED, seed_len[1] = coop_dgram_pack(seeds[1], sizeof seeds[1], &h, payload, n, key);
    CoopConnect c = {.intent = 2, .client_id = 99, .session = 3, .nonce = 4};
    memcpy(c.locator, key, sizeof key);
    strcpy(c.name, "Bob");
    n = coop_connect_pack(payload, sizeof payload, &c);
    h.type = COOP_T_CONNECT, seed_len[2] = coop_dgram_pack(seeds[2], sizeof seeds[2], &h, payload, n, key);
    CoopError e = {.reason = 1, .nonce = 4};
    strcpy(e.text, "no such room");
    n = coop_error_pack(payload, sizeof payload, &e);
    h.type = COOP_T_ERROR, seed_len[3] = coop_dgram_pack(seeds[3], sizeof seeds[3], &h, payload, n, key);
    n = coop_seal(payload, sizeof payload, mk, 1, 2, 5, 1, COOP_K_TEST, "hello", 5);
    h.type = COOP_T_DATA, seed_len[4] = coop_dgram_pack(seeds[4], sizeof seeds[4], &h, payload, n, key);
    h.type = COOP_T_PONG, seed_len[5] = coop_dgram_pack(seeds[5], sizeof seeds[5], &h, "abcd", 4, key);
    n = coop_kick_pack(payload, sizeof payload, 2, 6);
    h.type = COOP_T_KICK, seed_len[6] = coop_dgram_pack(seeds[6], sizeof seeds[6], &h, payload, n, key);
    CoopPos pos = {.pseq = 7, .t_ms = 1000, .flags = COOP_POS_IN_WORLD, .area = "HyruleField", .pos = {1, 2, 3}, .nslots = 2,
                   .slot = {{"Walk", 12}, {"Lower_Walk", 3.5f}}};
    n = coop_pos_pack(payload, sizeof payload, &pos);   // a POS as the payload, so that changed copies reach its parser
    h.type = COOP_T_DATA, seed_len[7] = coop_dgram_pack(seeds[7], sizeof seeds[7], &h, payload, n, key);
    long parsed = 0, tag_ok = 0, open_ok = 0, changed = 0, changed_ok = 0;
    for (int it = 0; it < 400000; it++) {
        size_t len;
        int mode = it % 4, s = -1;
        if (mode == 0) {   // random bytes
            len = rnd32() % 1300;
            for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rnd32() % 5 ? rnd32() : 0);
        } else {           // a valid datagram with bytes changed, or cut short
            s = (int)(rnd32() % 8);
            len = seed_len[s];
            memcpy(buf, seeds[s], len);
            for (unsigned k = rnd32() % 4; k && len; k--) buf[rnd32() % len] = (uint8_t)rnd32();
            if (mode == 3) len = rnd32() % (len + 1);
        }
        CoopHdr hh;
        const uint8_t *p;
        if (!coop_dgram_parse(buf, len, &hh, &p)) {
            p = buf, hh.len = (uint16_t)len;   // the payload parsers on whatever bytes there are
        } else {
            parsed++;
            int ok = coop_dgram_verify(buf, len, key);
            tag_ok += ok;
            if (s >= 0 && (len != seed_len[s] || memcmp(buf, seeds[s], len))) changed++, changed_ok += ok;
        }
        CoopConnect cn;
        CoopRoster rr;
        CoopJoined jj;
        CoopError ee;
        uint32_t v;
        uint64_t s64;
        uint8_t kind, ck[16];
        size_t no;
        char nameb[COOP_NAME_MAX + 1], secretb[COOP_SECRET_LEN + 1];
        if (coop_connect_parse(p, hh.len, &cn)) coop_connect_pack(out, sizeof out, &cn);
        if (coop_roster_parse(p, hh.len, &rr, NULL)) coop_roster_pack(out, sizeof out, &rr);
        if (coop_joined_parse(p, hh.len, &jj)) coop_joined_pack(out, sizeof out, &jj);
        if (coop_error_parse(p, hh.len, &ee)) coop_error_pack(out, sizeof out, &ee);
        if (coop_challenge_parse(p, hh.len, &s64, ck)) coop_challenge_pack(out, sizeof out, s64, ck);
        coop_kick_parse(p, hh.len, &kind, &s64);
        coop_u32_parse(p, hh.len, &v);
        coop_envelope_peek(p, hh.len, &s64, &v);
        CoopPos pp;   // a POS that parses packs to the same bytes: nothing in it is dropped or changed on the way in
        if (coop_pos_parse(p, hh.len, &pp))
            CHECK(coop_pos_pack(out, sizeof out, &pp) == hh.len && !memcmp(out, p, hh.len), "a POS that parses packs to other bytes");
        open_ok += coop_open(p, hh.len, mk, hh.src, hh.dst, &kind, opened, sizeof opened, &no);
        coop_name_clean(nameb, sizeof nameb, p, hh.len);
        char text[40];
        size_t tl = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, buf, tl);
        text[tl] = 0;
        coop_secret_normalize(text, secretb);
    }
    CHECK(parsed > 1000, "the fuzz reached the payload parsers (%ld datagrams parsed)", parsed);
    CHECK(changed > 1000 && !changed_ok, "a changed datagram's tag no longer verifies (%ld of %ld did)", changed_ok, changed);
    printf("coop_proto: parser fuzz: 400000 inputs, %ld parsed as datagrams (%ld tags verified, %ld changed copies, none verified), "
           "%ld sealed messages opened (unchanged copies)\n", parsed, tag_ok, changed, open_ok);
}

// ---- 4. the POS counter

// The session can deliver a message again, so a pose sent again or recorded earlier in the session must not move a puppet
// back, also in a session long enough for a 16-bit counter to wrap; the first poses of another session in the member id
// are taken whatever their counter; and a pose forged far ahead keeps the sender's own out only until nothing was taken
// for COOP_POS_QUIET_MS.
static void check_pos_counter(void) {
    CoopPosSeen s;
    memset(&s, 0, sizeof s);
    const uint64_t s1 = 0x5e55000000000001ull, s2 = 0x5e55000000000002ull;
    uint64_t t = 1000;
    int took = 0;
    for (uint32_t c = 1; c <= 200; c++, t += 67) took += coop_pos_fresh(&s, s1, c, t) != 0;
    CHECK(took == 200, "POS counter: of 200 poses of one session in order, %d are taken", took);
    int again = coop_pos_fresh(&s, s1, 200, t), older = coop_pos_fresh(&s, s1, 150, t + 10);
    CHECK(!again && !older, "POS counter: a pose sent again (%d) or an older one (%d) of the same session is taken", again, older);
    // another session in member id 1 (another player given it, or the member back in a new session) starts lower
    t += 67;
    int first = coop_pos_fresh(&s, s2, 1, t), second = coop_pos_fresh(&s, s2, 2, t + 67);
    CHECK(first && second, "POS counter: the first poses of another session in the same member id are not taken (%d %d)", first, second);
    // 40000 poses are 44 minutes at 15 a second: one recorded 35000 poses earlier stays older
    t += 134;
    int late = coop_pos_fresh(&s, s2, 40000, t), recorded = coop_pos_fresh(&s, s2, 5000, t + 67);
    CHECK(late && !recorded, "POS counter: after pose 40000 of a session, its pose 5000 sent again is taken (%d %d)", late, recorded);
    // a pose forged 30000 ahead (someone with the room's secret), then the sender's own every 67 ms
    t += 134;
    const uint64_t forged_at = t;
    int forged = coop_pos_fresh(&s, s2, 70000, t), held = 0, back = 0;
    uint64_t back_at = 0;
    uint32_t c = 40001;
    for (; c < 40001 + 15 * 6 && !back; c++) {
        t += 67;
        int f = coop_pos_fresh(&s, s2, c, t);
        if (f) back = f, back_at = t;
        else held++;
    }
    const long long waited = back ? (long long)(back_at - forged_at) : -1;   // -1: never taken again within 6 s
    CHECK(forged && back == 2 && waited >= COOP_POS_QUIET_MS && waited < COOP_POS_QUIET_MS + 67,
          "POS counter: after a pose forged 30000 ahead the sender's own are taken again %lld ms later (%d held out, start again %d), "
          "not %d to %d ms later", waited, held, back, COOP_POS_QUIET_MS, COOP_POS_QUIET_MS + 66);
    int next = coop_pos_fresh(&s, s2, c, t + 67);
    CHECK(next == 1, "POS counter: the sender's next pose after that is not taken (%d)", next);
    printf("coop_proto: POS counter: a pose sent again or older is dropped, also 35000 back; another session starts again; "
           "a pose forged 30000 ahead held the sender's own out %lld ms (%d poses)\n", waited, held);
}

// ---- the sealed vectors, for tests/test_coop_relay.py's SEALED table

static void print_sealed(void) {
    static const struct { const char *name; uint8_t src, dst; uint64_t session; uint32_t counter; uint8_t kind; const char *message; } v[] = {
        {"test", 3, 255, 0x0123456789abcdefull, 1, 0xf0, "hello"},
        {"empty", 1, 2, 0xfedcba9876543210ull, 0xffffffffu, 0x03, ""},
    };
    uint8_t mk[COOP_MSG_KEY_LEN], box[COOP_MAX_PAYLOAD];
    coop_room_keys(ROOM_SECRET, NULL, mk);
    for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
        size_t m = strlen(v[i].message);
        size_t n = coop_seal(box, sizeof box, mk, v[i].src, v[i].dst, v[i].session, v[i].counter, v[i].kind, v[i].message, m);
        printf("%s %u %u %016llx %u %u ", v[i].name, v[i].src, v[i].dst, (unsigned long long)v[i].session, v[i].counter, v[i].kind);
        print_hex((const uint8_t *)v[i].message, m);
        putchar(' ');
        print_hex(box, n);
        putchar('\n');
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--sealed")) {
        print_sealed();
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: coop_proto_test <tests/coop_vectors.txt> | --sealed\n");
        return 2;
    }
    check_crypto();
    check_vectors(argv[1]);
    check_fuzz();
    check_pos_counter();
    if (fails) {
        fprintf(stderr, "coop_proto: %d checks failed\n", fails);
        return 1;
    }
    printf("coop_proto: passed\n");
    return 0;
}
