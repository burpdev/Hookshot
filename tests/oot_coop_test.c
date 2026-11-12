/* The game side of co-op's progress sync without the game: host/oot_coop.c's reader, classification, apply rules,
   table check, guest calls, gate and switch, cut out of the source by tests/test_oot_coop.py (OOT_COOP_PIECES) and run
   on a made-up flag database in a fake guest memory, with a fake getter and fake setters in place of the game's. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "coop.h"
#include "coop_state.h"
#include "../gpu/vk/compat/hookshot_env.h"   /* (env_on, env_value_off: the switches' rule, as host/oot_coop.c has it) */

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); failures++; } } while (0)

/* ---- the fake guest: two regions (the game's data around its singleton slots, and a heap) */
typedef struct { uint64_t x[32]; uint64_t pc; } Ctx;
typedef void (*A64Fn)(Ctx *);
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static int lock_held;
static int TryAcquireSRWLockExclusive(SRWLOCK *l) { (void)l; return !lock_held; }
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; }

static struct { uint64_t base, size; uint8_t *mem; } regions[2] = {{0x84c00000ull, 0x400000, NULL}, {0x10000000ull, 16u << 20, NULL}};
static unsigned stray;   /* reads outside both regions (none expected but the null pointer's page) */
static uint8_t *host_ptr(uint64_t a, uint64_t n) {
    for (int i = 0; i < 2; i++)
        if (a >= regions[i].base && n <= regions[i].size && a - regions[i].base <= regions[i].size - n) return regions[i].mem + (a - regions[i].base);
    return NULL;
}
static void *hos_guest_range(uint64_t a, uint64_t n) { return host_ptr(a, n); }
#define GP(a) ((void *)host_ptr((a), 1))
static uint64_t ld(uint64_t a, unsigned n) {
    uint64_t v = 0;
    const uint8_t *p = host_ptr(a, n);
    if (p) memcpy(&v, p, n);
    else if (a >= 0x1000) stray++;
    return v;
}
static uint64_t recomp_load8(Ctx *c, uint64_t a) { (void)c; return ld(a, 1); }
static uint64_t recomp_load32(Ctx *c, uint64_t a) { (void)c; return ld(a, 4); }
static uint64_t recomp_load64(Ctx *c, uint64_t a) { (void)c; return ld(a, 8); }
static void st(uint64_t a, uint64_t v, unsigned n) {
    uint8_t *p = host_ptr(a, n);
    if (!p) { fprintf(stderr, "FAIL: a store outside the fake guest at %#llx\n", (unsigned long long)a); exit(1); }
    memcpy(p, &v, n);
}
static void recomp_store64_sp(Ctx *c, uint64_t a, uint64_t v) { (void)c; st(a, v, 8); }

static uint64_t heap_at = 0x10000000ull;
static uint64_t galloc(uint64_t n) {
    const uint64_t a = heap_at;
    heap_at += (n + 15) & ~15ull;
    if (!host_ptr(a, n)) { fprintf(stderr, "FAIL: the fake heap is full\n"); exit(1); }
    return a;
}

static char logbuf[1 << 16];
static size_t logat;
static void hos_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (logat < sizeof logbuf) logat += (size_t)vsnprintf(logbuf + logat, sizeof logbuf - logat, fmt, ap);
    va_end(ap);
    if (logat < sizeof logbuf - 1) logbuf[logat++] = '\n', logbuf[logat] = 0;
}
static uint64_t now_ms = 1000000;
static uint64_t coop_now_ms(void) { return now_ms; }
static const char hos_main_build_id[] = "e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348";

/* the game's routines: a getter that reads the fake table (or says otherwise: lie), setters that record their calls */
static int lie;
static struct { uint64_t mgr, value, hash, fn; } sets[16];
static int nsets;
static uint64_t entry_lookup(uint64_t mgr, unsigned base, unsigned stride, uint32_t hash);
static void fake_get(Ctx *c) {
    const uint64_t e = entry_lookup(c->x[0], 0x008, 0x18, (uint32_t)c->x[2]);
    st(c->x[1], e ? (ld(e + 0x16, 1) ^ (lie ? 1 : 0)) : 0, 1);
    c->x[0] = e != 0;
    c->x[3] = c->x[4] = 0xdead;   /* (clobbers what a callee may) */
    c->x[31] -= 0x100;
}
static void fake_set_bool(Ctx *c) {
    if (nsets < 16) sets[nsets].mgr = c->x[0], sets[nsets].value = c->x[1], sets[nsets].hash = c->x[2], sets[nsets++].fn = 0x806a2b20;
    c->x[0] = 0, c->x[5] = 0xdead, c->x[30] = 0xbad;
}
static void fake_set_int(Ctx *c) {
    if (nsets < 16) sets[nsets].mgr = c->x[0], sets[nsets].value = c->x[1], sets[nsets].hash = c->x[2], sets[nsets++].fn = 0x80183b70;
    c->x[0] = 0, c->x[5] = 0xdead;
}
static A64Fn a64_lookup(uint64_t addr) {
    return addr == 0x8022d9c0ull ? fake_get : addr == 0x806a2b20ull ? fake_set_bool : addr == 0x80183b70ull ? fake_set_int : NULL;
}

/* the switch's surroundings: the environment and the options file as the test sets them, the session's hook
   registration recorded, and counters in place of the engine, the scan and the apply */
static const char *env_coop, *env_probe;
static const char *test_getenv(const char *name) {
    return !strcmp(name, "HOS_COOP") ? env_coop : !strcmp(name, "HOS_COOP_PROBE") ? env_probe : NULL;
}
#define getenv test_getenv
static int ini_coop;
static int hos_ini_option(const char *key, int otherwise) { return !strcmp(key, "coop") ? ini_coop : otherwise; }
static struct { int msg, view, tick, remove; CoopViewFn view_fn; } hooks;
int coop_on_message(uint8_t kind, CoopMsgFn fn, void *user) { (void)kind, (void)user; hooks.msg += fn != NULL; return 1; }
int coop_on_view(CoopViewFn fn, void *user) { (void)user; hooks.view++, hooks.view_fn = fn; return 1; }
int coop_on_tick(CoopTickFn fn, void *user) { (void)user; hooks.tick += fn != NULL; return 1; }
int coop_on_remove(CoopRemoveFn fn, void *user) { (void)user; hooks.remove += fn != NULL; return 1; }
static int rules_said, views, scans, applies, probes;
static void read_rules(CoopRules *r, int say) { coop_rules_default(r); rules_said += say; }
static void read_test_aid(void) {}
static void view_changed(const CoopView *v) { (void)v; views++; }
static void on_message(int from, const uint8_t *bytes, size_t n, void *user) { (void)from, (void)bytes, (void)n, (void)user; }
static void on_tick(uint64_t now, void *user) { (void)now, (void)user; }
static int on_remove(int member, void *user) { (void)member, (void)user; return 0; }
static int ensure_engine(void) { return 1; }
static void run_test_aid(Ctx *c, uint64_t m) { (void)c, (void)m; }
static void probe(Ctx *c, uint64_t m) { (void)c, (void)m; probes++; }
static void scan(Ctx *c, uint64_t m, uint64_t now) { (void)c, (void)m, (void)now; scans++; }
static void apply(Ctx *c, uint64_t m) { (void)c, (void)m; applies++; }

#include OOT_COOP_PIECES

/* ---- the made-up database (the layout the source's header comment gives) */
static uint64_t mgr;
typedef struct { unsigned base, stride, cap, max; uint64_t entries, table; unsigned count; } Kind;
static Kind kinds[3];   /* Bool, Int, UInt */

static uint64_t entry_lookup(uint64_t m, unsigned base, unsigned stride, uint32_t hash) {
    const uint64_t k = m + base, table = ld(k + 0x18, 8), entries = ld(k + 0x10, 8);
    const uint32_t cap = (uint32_t)ld(k + 0x24, 4), count = (uint32_t)ld(k + 0x08, 4);
    for (uint32_t i = hash % cap, n = 0; cap && n < cap; n++, i = (i + 1) % cap) {
        const uint32_t h = (uint32_t)ld(table + 8ull * i, 4);
        if (h == hash) { const uint32_t idx = (uint32_t)ld(table + 8ull * i + 4, 4); return idx < count ? entries + (uint64_t)idx * stride : 0; }
        if (!h) return 0;
    }
    return 0;
}
static void kind_init(int t, unsigned base, unsigned stride, unsigned cap, unsigned max) {
    Kind *k = &kinds[t];
    k->base = base, k->stride = stride, k->cap = cap, k->max = max, k->count = 0;
    k->entries = galloc((uint64_t)stride * max), k->table = galloc(8ull * cap);
    st(mgr + base + 0x10, k->entries, 8), st(mgr + base + 0x18, k->table, 8), st(mgr + base + 0x24, cap, 4), st(mgr + base + 0x08, 0, 4);
}
static void table_put(uint64_t table, unsigned cap, uint32_t hash, uint32_t idx) {
    uint32_t i = hash % cap;
    while (ld(table + 8ull * i, 4)) i = (i + 1) % cap;
    st(table + 8ull * i, hash, 4), st(table + 8ull * i + 4, idx, 4);
}
/* a flag: its entry's reset kinds, save file, default and current value */
static uint64_t add(int t, uint32_t hash, uint32_t reset, int32_t file, int32_t def, int32_t cur) {
    Kind *k = &kinds[t];
    const uint64_t e = k->entries + (uint64_t)k->count * k->stride;
    st(e, hash, 4), st(e + 0x08, reset, 4), st(e + 0x10, (uint32_t)file, 4);
    if (t == 0) st(e + 0x15, (uint64_t)def, 1), st(e + 0x16, (uint64_t)cur, 1);
    else st(e + 0x18, (uint32_t)def, 4), st(e + 0x1c, (uint32_t)cur, 4);
    table_put(k->table, k->cap, hash, k->count);
    st(mgr + k->base + 0x08, ++k->count, 4);
    return e;
}
static uint64_t st_entries, st_table;
static unsigned st_count;
static void add_struct(uint32_t hash, const uint32_t (*m)[2], unsigned n) {
    const unsigned cap = 16;
    const uint64_t e = st_entries + (uint64_t)st_count * 0x20, t = galloc(8ull * cap);
    for (unsigned i = 0; i < n; i++) table_put(t, cap, m[i][0], m[i][1]);
    st(e + 0x08, t, 8), st(e + 0x14, cap, 4);
    table_put(st_table, 64, hash, st_count);
    st(mgr + 0xc10, ++st_count, 4);
}

enum { NB = 40 };
static uint32_t plain[NB];

static void make_database(void) {
    heap_at = 0x10000000ull;
    memset(regions[1].mem, 0, regions[1].size);
    mgr = galloc(0xd80);
    const uint64_t holder = galloc(8);
    st(holder, mgr, 8), st(0x84c9e180ull, holder, 8);
    kind_init(0, 0x008, 0x18, 128, 100), kind_init(1, 0x0c8, 0x28, 64, 60), kind_init(2, 0x788, 0x28, 8, 4);
    st_entries = galloc(0x20 * 16), st_table = galloc(8 * 64), st_count = 0;
    st(mgr + 0xc18, st_entries, 8), st(mgr + 0xc20, st_table, 8), st(mgr + 0xc2c, 64, 4), st(mgr + 0xc10, 0, 4);
    for (int i = 0; i < NB; i++) plain[i] = 0x10000001u + 0x1000u * (uint32_t)i, add(0, plain[i], 8, 0, 0, i & 1);
}

/* hashes of the made-up flags (the real ones the source names where it names them) */
enum {
    B_TEMP = 0x20000001, B_DAY = 0x20000002, B_OPTION = 0x20000003, B_FILE1 = 0x20000004, B_NOFILE = 0x20000005,
    B_NORESET = 0x20000006, B_INV = 0x20000007, B_FIRSTGET = 0x20000008, B_COLLIDE = 0x20000008 + 128,
    I_POUCH = 0x30000001, I_OTHER = 0x30000002, I_KEYS = 0x30000003, I_DUNGEON_OTHER = 0x30000004, I_WALLET = 0x30000005,
    I_CAP_SEEDS = 0x30000006, I_CAP_OTHER = 0x30000007, I_UNRELATED = 0x30000008, P_ITEM = 0x40000001, P_QUEST = 0x40000002,
    P_OWN = 0x40000003, P_AMMO0 = 0x40000004, P_AMMO1 = 0x40000005, D_FOREST = 0x40000006, P_LENS = 0x40000007,
    /* the Lens of Truth's count: an ordinary item the game never takes back, so shared, not one of the per-player
       counts beside the masks' (there a Lens one player got would never reach the others) */
    I_LENS = 0x49d1c374,
};

static GFlag *must(uint32_t hash) {
    GFlag *f = flag_by_hash(hash);
    if (!f) fprintf(stderr, "FAIL: flag %08x is not in the table\n", hash), failures++;
    return f;
}

int main(void) {
    for (int i = 0; i < 2; i++) regions[i].mem = calloc(1, regions[i].size);
    Ctx c;
    memset(&c, 0, sizeof c);
    c.x[31] = 0x10f00000ull;

    /* ---- the classification of a made-up table */
    make_database();
    add(0, B_TEMP, 8 | 1, 0, 0, 0);       /* reverts at a scene change */
    add(0, B_DAY, 16 | 2, 0, 0, 0);       /* at a new day */
    add(0, B_OPTION, 4 | 8, 0, 0, 0);     /* an option */
    add(0, B_FILE1, 8, 1, 0, 0);          /* another save file */
    add(0, B_NOFILE, 8, -1, 0, 0);        /* not saved */
    add(0, B_NORESET, 0, 0, 0, 0);        /* never reset by a new game */
    add(0, B_INV, 32, 0, 1, 1);           /* starts true */
    add(0, B_FIRSTGET, 16, 0, 0, 0);
    add(0, B_COLLIDE, 8, 0, 0, 1);        /* the same slot as B_FIRSTGET: found by probing */
    add(0, song_flags[3], 8, 0, 0, 0);
    add(0, heart_reward_flags[2], 8, 0, 0, 0);
    add(0, F_LOADING, 1, -1, 0, 0);
    for (int i = 0; i < 4; i++) add(0, transition_flags[i], 1, -1, 0, 0);
    const uint64_t e_pouch = add(1, I_POUCH, 8, 0, 0, 3);
    add(1, I_OTHER, 8, 0, 0, 7);
    add(1, I_KEYS, 8, 0, 0, 2);
    add(1, I_DUNGEON_OTHER, 8, 0, 0, 1);
    const uint64_t e_wallet = add(1, I_WALLET, 8, 0, 0, 200);
    const uint64_t e_capseeds = add(1, I_CAP_SEEDS, 8, 0, 0, 40);
    add(1, I_CAP_OTHER, 8, 0, 0, 1);
    add(1, I_UNRELATED, 8, 0, 0, 5);
    add(1, I_LENS, 8, 0, 0, 1);
    add(1, F_RUPEES, 8, 0, 0, 99);
    add(1, F_MAGIC_SIZE, 8, 0, 0, 1);
    add(1, quest_counts[0], 8, 0, 0, 1);
    add(1, own_counts[5], 8, 0, 0, 1);
    add(1, ammo[0].count, 8, 0, 0, 20);
    add(1, ammo[1].count, 8, 0, 0, 5);
    const uint64_t e_clock = add(2, F_PLAY_CLOCK, 8, 0, 0, 0);
    {
        const uint32_t first_get[][2] = {{0x5001, B_FIRSTGET}};
        const uint32_t pouch[][2] = {{0x5101, P_ITEM}, {0x5102, P_QUEST}, {0x5103, P_OWN}, {0x5104, P_AMMO0}, {0x5105, P_AMMO1},
                                     {0x5106, P_LENS}};
        const uint32_t item[][2] = {{M_STOCK, I_POUCH}, {0x5201, I_OTHER}};
        const uint32_t quest[][2] = {{M_STOCK, quest_counts[0]}}, own[][2] = {{M_STOCK, own_counts[5]}}, lens[][2] = {{M_STOCK, I_LENS}};
        const uint32_t am0[][2] = {{M_STOCK, ammo[0].count}}, am1[][2] = {{M_STOCK, ammo[1].count}};
        const uint32_t dungeon[][2] = {{0x5301, D_FOREST}}, forest[][2] = {{M_SMALL_KEYS, I_KEYS}, {0x5302, I_DUNGEON_OTHER}};
        const uint32_t capacity[][2] = {{CAP_RUPEES, I_WALLET}, {ammo[0].cap_member, I_CAP_SEEDS}, {0x5401, I_CAP_OTHER}};
        add_struct(S_FIRST_GET, first_get, 1), add_struct(S_POUCH, pouch, 6), add_struct(P_ITEM, item, 2);
        add_struct(P_QUEST, quest, 1), add_struct(P_OWN, own, 1), add_struct(P_AMMO0, am0, 1), add_struct(P_AMMO1, am1, 1);
        add_struct(P_LENS, lens, 1);
        add_struct(S_DUNGEON, dungeon, 1), add_struct(D_FOREST, forest, 2), add_struct(S_CAPACITY, capacity, 3);
    }
    CHECK(build_table(&c, mgr) == 1);
    CHECK(G.count[COOP_CAT_PROGRESS] == NB + 2);   /* the plain ones, B_INV and B_COLLIDE */
    CHECK(G.count[COOP_CAT_SONGS] == 1);
    CHECK(G.count[COOP_CAT_ITEMS] == 7);           /* B_FIRSTGET, I_POUCH, I_LENS, I_WALLET, I_CAP_SEEDS, I_CAP_OTHER, F_MAGIC_SIZE */
    CHECK(G.count[COOP_CAT_QUEST] == 1 && G.count[COOP_CAT_KEYS] == 1 && G.count[COOP_CAT_RUPEES] == 1 && G.count[COOP_CAT_AMMO] == 2);
    static const uint32_t left_out[] = {B_TEMP, B_DAY, B_OPTION, B_FILE1, B_NOFILE, B_NORESET, F_LOADING, I_OTHER,
                                        I_DUNGEON_OTHER, I_UNRELATED, F_PLAY_CLOCK};
    for (size_t i = 0; i < sizeof left_out / sizeof *left_out; i++) CHECK(!flag_by_hash(left_out[i]));
    CHECK(!flag_by_hash(heart_reward_flags[2]) && !flag_by_hash(own_counts[5]) && !flag_by_hash(transition_flags[0]));
    GFlag *f;
    if ((f = must(plain[0]))) CHECK(f->cat == COOP_CAT_PROGRESS && !f->is_int && f->max == 1 && !f->inv);
    if ((f = must(B_INV))) CHECK(f->cat == COOP_CAT_PROGRESS && f->inv && reg_of(f, 1) == 0 && reg_of(f, 0) == 1);
    if ((f = must(B_FIRSTGET))) CHECK(f->cat == COOP_CAT_ITEMS && !f->is_int);
    if ((f = must(B_COLLIDE))) CHECK(f->cat == COOP_CAT_PROGRESS && raw_of(&c, f) == 1);
    if ((f = must(song_flags[3]))) CHECK(f->cat == COOP_CAT_SONGS);
    if ((f = must(I_POUCH))) CHECK(f->cat == COOP_CAT_ITEMS && f->is_int && f->max == INT_MAX_ITEM && f->addr == e_pouch && raw_of(&c, f) == 3);
    if ((f = must(I_LENS))) CHECK(f->cat == COOP_CAT_ITEMS && f->is_int && f->max == INT_MAX_ITEM && raw_of(&c, f) == 1);
    if ((f = must(quest_counts[0]))) CHECK(f->cat == COOP_CAT_QUEST);
    if ((f = must(I_KEYS))) CHECK(f->cat == COOP_CAT_KEYS && f->max == KEY_MAX && f->cap_addr == 0);
    if ((f = must(F_RUPEES))) CHECK(f->cat == COOP_CAT_RUPEES && f->cap_addr == e_wallet);
    if ((f = must(ammo[0].count))) CHECK(f->cat == COOP_CAT_AMMO && f->cap_addr == e_capseeds);
    if ((f = must(ammo[1].count))) CHECK(f->cat == COOP_CAT_AMMO && f->cap_addr == 0);
    if ((f = must(I_CAP_OTHER))) CHECK(f->cat == COOP_CAT_ITEMS);
    if ((f = must(F_MAGIC_SIZE))) CHECK(f->cat == COOP_CAT_ITEMS);
    CHECK(G.npools == 4);
    for (int i = 0; i < G.nflags; i++) CHECK(coop_cat_is_pool(G.flags[i].cat) == (i >= G.nflags - G.npools));
    CHECK(entry_of(&c, mgr, C_BOOL, BOOL_SIZE, B_COLLIDE) && entry_of(&c, mgr, C_BOOL, BOOL_SIZE, B_FIRSTGET) &&
          entry_of(&c, mgr, C_BOOL, BOOL_SIZE, B_COLLIDE) != entry_of(&c, mgr, C_BOOL, BOOL_SIZE, B_FIRSTGET));
    CHECK(!entry_of(&c, mgr, C_BOOL, BOOL_SIZE, 0x7777777u) && !entry_of(&c, mgr, C_BOOL, BOOL_SIZE, 0));
    CHECK(entry_of(&c, mgr, C_UINT, INT_SIZE, F_PLAY_CLOCK) == e_clock);
    /* the engine's limits: a flag of the table in its own category, nothing else */
    CHECK(limit_fn(COOP_CAT_PROGRESS, plain[0], NULL) == 1 && limit_fn(COOP_CAT_ITEMS, plain[0], NULL) == 0);
    CHECK(limit_fn(COOP_CAT_KEYS, I_KEYS, NULL) == KEY_MAX && limit_fn(COOP_CAT_PROGRESS, B_TEMP, NULL) == 0);
    CHECK(limit_fn(COOP_CAT_ITEMS, I_POUCH, NULL) == INT_MAX_ITEM && limit_fn(COOP_CAT_ITEMS, 0x12345u, NULL) == 0);

    /* the schema: the same table, the same schema; a flag classified otherwise, another */
    const uint32_t schema = G.schema;
    CHECK(schema != 0 && build_table(&c, mgr) == 1 && G.schema == schema);
    st(entry_lookup(mgr, C_BOOL, BOOL_SIZE, plain[5]) + E_RESET, 8 | 256, 4);
    CHECK(build_table(&c, mgr) == 1 && G.schema != schema && !flag_by_hash(plain[5]) && G.count[COOP_CAT_PROGRESS] == NB + 1);
    st(entry_lookup(mgr, C_BOOL, BOOL_SIZE, plain[5]) + E_RESET, 8, 4);
    CHECK(build_table(&c, mgr) == 1 && G.schema == schema);

    /* a damaged table: an entry index past the entries is skipped, a container past the guest is not read */
    {
        const uint32_t bogus = 0x7abcdef1u;
        table_put(kinds[0].table, kinds[0].cap, bogus, 9999);
        CHECK(build_table(&c, mgr) == 1 && !flag_by_hash(bogus) && !entry_of(&c, mgr, C_BOOL, BOOL_SIZE, bogus));
        const uint64_t keep = ld(mgr + C_INT + CN_ENTRIES, 8);
        st(mgr + C_INT + CN_ENTRIES, 0x7ffffff000ull, 8);
        const unsigned before = stray;
        CHECK(build_table(&c, mgr) == 1 && G.count[COOP_CAT_ITEMS] == 1 && !flag_by_hash(I_POUCH) && stray == before);
        st(mgr + C_INT + CN_ENTRIES, keep, 8);
        CHECK(build_table(&c, mgr) == 1 && G.count[COOP_CAT_ITEMS] == 7);
    }

    /* ---- what is applied: raised only, within the game's table */
    {
        GFlag *b = must(plain[0]), *inv = must(B_INV), *item = must(I_POUCH), *keys = must(I_KEYS), *rup = must(F_RUPEES);
        int32_t v = -7;
        CoopApply a = {0, COOP_CAT_PROGRESS, plain[0], 1};
        CHECK(apply_value(b, &a, 0, &v) && v == 1);
        CHECK(!apply_value(b, &a, 1, &v));                        /* already set */
        a.value = 2;
        CHECK(!apply_value(b, &a, 0, &v));                        /* a Bool is 1 or nothing */
        a.value = 0;
        CHECK(!apply_value(b, &a, 1, &v));                        /* never lowered */
        a.value = 1, a.cat = COOP_CAT_ITEMS;
        CHECK(!apply_value(b, &a, 0, &v));                        /* classified otherwise here */
        a.cat = COOP_CAT_PROGRESS, a.pool = 1;
        CHECK(!apply_value(b, &a, 0, &v));                        /* not a pool counter */
        CoopApply ai = {0, COOP_CAT_PROGRESS, B_INV, 1};
        CHECK(apply_value(inv, &ai, 1, &v) && v == 0);            /* a flag that starts true: "changed" is false */
        CHECK(!apply_value(inv, &ai, 0, &v));
        CoopApply it = {0, COOP_CAT_ITEMS, I_POUCH, 5};
        CHECK(apply_value(item, &it, 3, &v) && v == 5);
        CHECK(!apply_value(item, &it, 5, &v) && !apply_value(item, &it, 6, &v));   /* only raised */
        it.value = INT_MAX_ITEM;
        CHECK(apply_value(item, &it, 3, &v) && v == INT_MAX_ITEM);
        it.value = INT_MAX_ITEM + 1;
        CHECK(!apply_value(item, &it, 3, &v));                    /* past the game's counters */
        it.value = -1;
        CHECK(!apply_value(item, &it, -5, &v));
        CoopApply k = {0, COOP_CAT_KEYS, I_KEYS, KEY_MAX + 1};
        CHECK(!apply_value(keys, &k, 0, &v));                     /* past the most keys a dungeon is given */
        k.pool = 1, k.value = 1;
        CHECK(apply_value(keys, &k, 2, &v) && v == 1);            /* a pool counter shows the pool: down too */
        k.value = 2;
        CHECK(!apply_value(keys, &k, 2, &v));                     /* nothing to write */
        k.pool = 0, k.value = 3;
        CHECK(!apply_value(keys, &k, 2, &v));                     /* a pool category's value comes as a pool counter */
        CoopApply r = {1, COOP_CAT_RUPEES, F_RUPEES, 1000};
        CHECK(!apply_value(rup, &r, 99, &v));
        CHECK(!apply_value(NULL, &r, 0, &v));                     /* a flag this game does not have */
    }

    /* ---- the Int ring: a set of that hash pending, or a ring that does not read, holds a pool write back */
    {
        const uint64_t ring = galloc(12 * 8);
        st(mgr + C_INT + CN_RING, ring, 8), st(mgr + C_INT + CN_RING_CAP, 8, 4), st(mgr + C_INT + CN_RING_CTR, 0x300000 | 2, 4);
        st(ring + 4, I_OTHER, 4), st(ring + 12 + 4, F_RUPEES, 4);
        CHECK(int_pending(&c, mgr, F_RUPEES) && !int_pending(&c, mgr, I_KEYS));
        st(mgr + C_INT + CN_RING_CTR, 9, 4);
        CHECK(int_pending(&c, mgr, I_KEYS));                      /* more pending than the ring holds: not trusted */
        st(mgr + C_INT + CN_RING_CTR, 0, 4);
        CHECK(!int_pending(&c, mgr, F_RUPEES));
    }

    /* ---- the game's routines: the guest's registers as they were afterwards */
    {
        for (int i = 0; i < 31; i++) c.x[i] = 0x1000 + (uint64_t)i;
        const Ctx before = c;
        nsets = 0;
        CHECK(set_flag(&c, mgr, 0, plain[2], 1) && set_flag(&c, mgr, 1, I_POUCH, 7));
        CHECK(nsets == 2 && sets[0].fn == 0x806a2b20 && sets[0].mgr == mgr && sets[0].value == 1 && sets[0].hash == plain[2]);
        CHECK(sets[1].fn == 0x80183b70 && sets[1].value == 7 && sets[1].hash == I_POUCH);
        CHECK(!memcmp(&c, &before, sizeof c));
        int got = -1;
        CHECK(game_get_bool(&c, mgr, plain[1], &got) == 1 && got == 1 && !memcmp(&c, &before, sizeof c));
        CHECK(game_get_bool(&c, mgr, 0x7777777u, &got) == 0);
    }

    /* ---- the check before anything is shared: the table against the game's getter */
    {
        int n = 0;
        lie = 0;
        CHECK(check_table(&c, mgr, &n) == 0 && n >= CHECK_FLAGS / 2 + 4);
        lie = 1;
        CHECK(check_table(&c, mgr, &n) == n && n >= CHECK_FLAGS / 2);   /* every one differs: a table read otherwise */
        CHECK(strstr(logbuf, "by the game's getter") != NULL);
        lie = 0;
    }

    /* ---- binding a save: the table checked against the getter once per table; one that reads otherwise shares nothing */
    {
        G.mgr = 0, G.nflags = 0, G.bound = 0, G.checked = 0, G.refused = 0;
        lie = 1;
        bind_tables(&c, mgr);
        CHECK(!G.bound && G.refused && strstr(logbuf, "progress sync shares nothing"));
        lie = 0;
        bind_tables(&c, mgr);
        CHECK(!G.bound && G.refused);                             /* the same table: still refused */
        G.mgr = 0;                                                /* the game made its flags anew: checked again */
        bind_tables(&c, mgr);
        CHECK(G.bound && !G.refused && G.nflags == 55 && strstr(logbuf, "the flag table reads as the game's getter does"));
        G.bound = 0, lock_held = 1;
        bind_tables(&c, mgr);
        CHECK(!G.bound);                                          /* the co-op thread holds the engine: the next frame */
        lock_held = 0;
    }

    /* ---- the gate: a save played, Link in the world, the clock running, no load and no scene change */
    {
        const uint64_t holder = galloc(0x500), actor = galloc(0x400), name = galloc(16);
        memcpy(GP(name), "Player", 7);
        st(0x84e571d0ull, holder, 8), st(holder + 0x480, actor, 8), st(actor + 0x20, name, 8);
        const float pos[3] = {12.5f, 3.0f, -40.0f};
        memcpy(GP(actor + 0x31c), pos, sizeof pos);
        G.mgr = mgr, G.clock_entry = 0, G.clock = 0, G.loaded = 0, G.titles = 0, G.bound = 0;
        st(e_clock + I_CURRENT, 500, 4);
        CHECK(!gate(&c, mgr));                                    /* a save's clock appeared: loaded, play from its next tick */
        CHECK(G.loaded == 1);
        now_ms += 1000, st(e_clock + I_CURRENT, 501, 4);
        CHECK(gate(&c, mgr));
        G.bound = 1;
        const uint64_t tr = entry_lookup(mgr, C_BOOL, BOOL_SIZE, transition_flags[2]);
        st(tr + B_CURRENT, 1, 1);
        CHECK(!gate(&c, mgr) && G.bound);                         /* a scene change: paused, the save stays bound */
        st(tr + B_CURRENT, 0, 1);
        CHECK(gate(&c, mgr));
        now_ms += CLOCK_MS + 1;
        CHECK(!gate(&c, mgr));                                    /* the clock stopped (a menu): paused */
        now_ms += 1000, st(e_clock + I_CURRENT, 502, 4);
        CHECK(gate(&c, mgr));
        memcpy(GP(name), "Npc_Zz", 7);
        CHECK(!gate(&c, mgr));                                    /* no Player actor */
        memcpy(GP(name), "Player", 7);
        const float origin[3] = {0, 0, 0};
        memcpy(GP(actor + 0x31c), origin, sizeof origin);
        CHECK(!gate(&c, mgr));                                    /* Link not placed yet */
        memcpy(GP(actor + 0x31c), pos, sizeof pos);
        CHECK(gate(&c, mgr));
        hos_oot_coop_rom_open("rom:/Event/EventFlow/OtherEvent.bfevfl");
        CHECK(gate(&c, mgr) && G.bound);
        hos_oot_coop_rom_open("rom:/Event/EventFlow/TitleEvent.bfevfl");
        now_ms += 1000, st(e_clock + I_CURRENT, 503, 4);
        CHECK(!gate(&c, mgr) && !G.bound && !G.loaded);           /* the title screen: its clock ticks on, nothing is read */
        CHECK(strstr(logbuf, "the title screen came back") != NULL);
        now_ms += 1000, st(e_clock + I_CURRENT, 504, 4);
        CHECK(!gate(&c, mgr));
        now_ms += 1000, st(e_clock + I_CURRENT, 9000, 4);         /* a save is loaded */
        CHECK(!gate(&c, mgr) && G.loaded);
        now_ms += 1000, st(e_clock + I_CURRENT, 9001, 4);
        CHECK(gate(&c, mgr));
        const uint64_t loading = entry_lookup(mgr, C_BOOL, BOOL_SIZE, F_LOADING);
        G.bound = 1;
        st(loading + B_CURRENT, 1, 1);
        CHECK(!gate(&c, mgr) && !G.bound);                        /* a save loading: unbound, bound again after */
        st(loading + B_CURRENT, 0, 1);
        now_ms += 1000, st(e_clock + I_CURRENT, 9002, 4);
        CHECK(gate(&c, mgr));
        hos_oot_coop_rom_open("rom:/Event/EventFlow/TitleEvent.bfevfl");   /* counted with co-op off too: see the switch */
        CHECK(title_starts == 2);
    }

    /* ---- the switch: progress sync on at the game's start (HOS_COOP, or coop= in the options file), or later, when the
       session calls back for the first time (co-op switched on in the Co-op tab); the session's hooks registered always */
    {
#define FRESH(starts) do { \
        G.state = ST_UNSEEN, G.on = 0, G.probe = 0, G.probed = 0, G.bound = 0, G.looked = 0, G.loaded = 0, G.titles = 0; \
        G.clock = 0, G.clock_at = 0, title_starts = (starts), env_coop = env_probe = NULL, ini_coop = 0; \
        memset(&hooks, 0, sizeof hooks), rules_said = views = scans = applies = probes = 0; } while (0)
        uint32_t clock = 20000;
#define TICK() (now_ms += 1000, st(e_clock + I_CURRENT, ++clock, 4))
        CoopView view;
        memset(&view, 0, sizeof view);

        /* off at the start: the present reads nothing, and the hooks are registered all the same */
        FRESH(1);
        TICK(), hos_oot_coop_present(&c);
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.state == ST_OFF && !G.on && !G.bound && !G.looked && !rules_said && !scans);
        CHECK(hooks.msg == 2 && hooks.view == 1 && hooks.tick == 1 && hooks.remove == 1);   /* once, not per present */
        /* the session's thread runs (co-op switched on in the tab) and calls back: sync is on, the settings said once */
        if (hooks.view_fn) hooks.view_fn(&view, NULL), hooks.view_fn(&view, NULL);
        CHECK(G.state == ST_ON && G.on == 1 && rules_said == 1 && views == 2);
        TICK(), hos_oot_coop_present(&c);                        /* the first look: the save's clock appears */
        CHECK(!G.bound && G.loaded);
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.bound && scans == 1 && applies == 1);            /* the save in play is read and shared */
        CHECK(hooks.view == 1);

        /* the options file's coop=1 alone, as the session reads it (host/coop.c coop_init) */
        FRESH(1);
        ini_coop = 1;
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.state == ST_ON && G.on == 1 && rules_said == 1 && hooks.view == 1 && hooks.tick == 1);
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.bound && scans == 1);
        /* the environment wins over the file, both ways */
        FRESH(1);
        ini_coop = 1, env_coop = "0";
        hos_oot_coop_present(&c);
        CHECK(G.state == ST_OFF && !G.on && hooks.view == 1);
        FRESH(1);
        env_coop = "1";
        hos_oot_coop_present(&c);
        CHECK(G.state == ST_ON && G.on == 1);

        /* switched on after the title screen came back: the title screen (Link's actor stays, its clock runs) cannot be
           told from play, so nothing is read until a save is loaded */
        FRESH(2);
        logat = 0, logbuf[0] = 0;
        hos_oot_coop_present(&c);
        if (hooks.view_fn) hooks.view_fn(&view, NULL);
        TICK(), hos_oot_coop_present(&c);
        TICK(), hos_oot_coop_present(&c);
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.on == 1 && !G.bound && !G.loaded && !scans && strstr(logbuf, "switched on after the title screen came back") != NULL);
        clock += 5000, TICK(), hos_oot_coop_present(&c);         /* a save is loaded */
        CHECK(!G.bound && G.loaded);
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.bound && scans == 1);

        /* the read-only probe alone: the table is read and logged, nothing is scanned or applied, sync stays off */
        FRESH(1);
        env_probe = "1";
        TICK(), hos_oot_coop_present(&c);
        TICK(), hos_oot_coop_present(&c);
        CHECK(G.state == ST_ON && !G.on && G.bound && probes == 1 && !scans && !applies && !rules_said && hooks.view == 1);
#undef FRESH
#undef TICK
    }

    if (failures) {
        fprintf(stderr, "%d checks failed; the log:\n%s", failures, logbuf);
        return 1;
    }
    printf("PASS: the classification of a made-up flag table (%d flags, %d pools), the apply rules, the Int ring, the game's "
           "routines called with the registers kept, the check against the getter, the gate, and the switch (the start, the "
           "options file, the session's first callback)\n", G.nflags, G.npools);
    return 0;
}
