/* (hookshot) Co-op progress sync, the game side (oot_coop.h), for the supported build e0f3ccfa.

   The flag database is the game's GameDataMgr (**0x84c9e180, as host/hos_main.c's debug tools reach it): typed
   containers of 0x60 bytes from +8: Bool at +0x008, Int at +0x0c8, UInt at +0x788. A container holds its entry count
   (+0x08), entries (+0x10), an open-addressing table of {u32 hash, u32 entry} slots probed from hash % capacity (+0x18,
   capacity +0x24) and a ring of pending sets (+0x28 capacity, +0x30 slots, +0x38 counter, its low 20 bits the count). A
   Bool entry is 0x18 bytes (+0x08 the reset kinds, +0x10 the save file, -1 none, +0x15 default, +0x16 current value), an
   Int or UInt entry 0x28 (+0x08, +0x10 the same, +0x18 default, +0x1c current). Structs group flags: the struct table
   (+0xc10 count, +0xc18 entries of 0x20, +0xc20 slots, +0xc2c capacity) maps a struct's hash to an entry whose member
   table (+0x08, capacity +0x14) holds {u32 member hash, u32 flag or struct hash} slots. Flags are named by MurmurHash3 of
   their names; the names stay out of this file, the hashes it needs are numbers below. Before anything is shared, the
   table is checked against the game's own getter (f_8022d9c0, as oot_gamedata_bool calls it): a build whose table reads
   otherwise shares nothing.

   Writes go through the game's own set routines, SetBool (f_806a2b20) and SetInt (f_80183b70), with x0 the manager, x1
   the value and x2 the hash: they queue the change on the ring, and the game's per-frame drain writes the current and
   saved values and its own bookkeeping, as for a change the game made. Never a direct write of the table (as
   oot_gamedata_set_bool does for the debug tools' age toggle): that would bypass the queue. Reads are of the entries'
   current values.

   What is shared (docs/coop.md has the reasons):
   - progress: every Bool that goes to progress.sav (save file 0) and is reset only by a new game or a load (reset kinds
     8, 16 and 32; flags with kind 1 or 256 revert at scene changes, 2 at a new day, 4 are options), except the songs,
     the first-get markers and the heart rewards below. Its register is 1 once the flag differs from its default (some
     start true and count down). Story, world and dungeon flags are one category: the game's flags carry no mark of which
     is which, and their names do not reach the runtime.
   - items: the item counts of the pouch struct (except the lists below), the capacity struct (wallet, bag and quiver
     sizes), the magic meter's size, and the first-get struct's markers; the highest count wins.
   - songs: 13 first-get markers. quest: 11 pouch counts (medallions, stones, the stone of agony, the gerudo card).
   - keys: the small-key count of each dungeon (the dungeon struct's member); rupees: the wallet's count; ammunition: 7
     pouch counts. These three are counters the game spends: shared, they are pools (coop_state.h), else per player.
   - per player always: the trade sequence, rented masks, the knife that breaks, amiibo items and one unknown seed item
     (a highest-count merge would hand back what the game took away); heart pieces and gold skulltula tokens, and the 11
     heart reward flags (their pickups are mostly per-object flags this layer does not write, so a shared count would
     pay a piece twice); life, the magic in the meter, bottle contents, equipment worn, the time of day, Link's place.

   Threads: the game's presenting thread reads and applies (hos_oot_coop_present), never waiting on the co-op thread (a
   frame in which the engine is busy reads nothing); the co-op thread (host/coop.c's hooks) carries the engine's
   messages, the offer, the acceptance and the copy of the save.

   Progress sync is on with co-op: from the first present when HOS_COOP=1 or coop=1 in the options file asks for it (as
   host/coop.c's session reads them), else from the session's first callback, when the Co-op tab switches co-op on
   later (sync_on). Other switches (read at the first present): HOS_COOP_PROGRESS, _ITEMS,
   _SONGS, _QUEST (shared|own), HOS_COOP_KEYS, _RUPEES, _AMMO (pool|own) the rules a room this game makes uses,
   HOS_COOP_ACCEPT=always (every room's offer accepted: tests; the environment only, hookshot_env.txt cannot set it),
   HOS_COOP_TRACE (a line per value sent, received and applied), and for tests HOS_COOP_PROBE=1 (with or without co-op:
   the table, its check against the getter and the gate logged; nothing written unless HOS_COOP_TEST_SET) and
   HOS_COOP_TEST_SET="hash=value@frame,..." (that flag set through the game's routine at that frame of play; the
   environment only). */
#include "coop_net.h"   /* (first: winsock2.h before windows.h) */
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "hos.h"
#include "coop.h"
#include "coop_state.h"
#include "oot_coop.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

extern const char *hos_user_dir;                     /* (nn_fs.c) */
int hos_ini_option(const char *key, int otherwise);  /* (nn_applet.c: the options file) */

#define GAMEDATA_SLOT 0x84c9e180ULL   /* GameDataMgr's holder (host/hos_main.c OOT_GAMEDATA_SLOT), dereferenced twice */
#define GET_BOOL_FN 0x8022d9c0ULL     /* GameDataMgr::GetBool(mgr, out, hash): 1 when found (oot_gamedata_bool) */
#define SET_BOOL_FN 0x806a2b20ULL     /* SetBool(mgr, value, hash) */
#define SET_INT_FN 0x80183b70ULL      /* SetInt(mgr, value, hash) */
#define PLAYER_PTR 0x84e571d0ULL      /* [PLAYER_PTR] + 0x480: the Player actor, none before the first load */

enum {
    C_BOOL = 0x008, C_INT = 0x0c8, C_UINT = 0x788,
    CN_COUNT = 0x08, CN_ENTRIES = 0x10, CN_TABLE = 0x18, CN_CAP = 0x24, CN_RING_CAP = 0x28, CN_RING = 0x30, CN_RING_CTR = 0x38,
    BOOL_SIZE = 0x18, INT_SIZE = 0x28,
    E_RESET = 0x08, E_FILE = 0x10, B_DEFAULT = 0x15, B_CURRENT = 0x16, I_CURRENT = 0x1c,
    ST_COUNT = 0xc10, ST_ENTRIES = 0xc18, ST_TABLE = 0xc20, ST_CAP = 0xc2c, ST_SIZE = 0x20, STE_MEMBERS = 0x08, STE_MCAP = 0x14,
    MGR_SIZE = 0xd80,
    RESET_NEW_GAME = 8 | 16 | 32, RESET_TEMPORARY = 1 | 2 | 4 | 256,
    SLICES = 4,             /* a quarter of the flags is read each frame: every flag every 4 frames */
    APPLY_PER_FRAME = 16,
    INT_MAX_ITEM = 999,     /* the highest item count or counter the room may set: the game's counters show three digits */
    KEY_MAX = 15,           /* the most small keys of one dungeon the room may set (the most any dungeon has is 9) */
    CLOCK_MS = 2500,        /* the game clock advanced this recently: the game is being played */
    CHECK_FLAGS = 24,       /* Bool flags read both ways (the table, the game's getter) before a save is shared */
};

/* Struct and member hashes (MurmurHash3 of the names the game uses). */
#define S_POUCH 0x3da094e3u      /* item counts, one struct per item */
#define M_STOCK 0x22344481u      /* an item's count */
#define S_FIRST_GET 0xade0ef78u  /* "got it once" markers, one per item */
#define S_DUNGEON 0xf5a32cdcu    /* one struct per dungeon: map, compass, boss key, small keys */
#define M_SMALL_KEYS 0x1af87831u
#define S_CAPACITY 0xf80f8077u   /* wallet, bomb bag, quiver, seed bag, stick and nut capacities */
#define F_RUPEES 0xa77921d7u
#define F_MAGIC_SIZE 0x12d63a97u
#define F_PLAY_CLOCK 0xe53eb6d1u /* UInt: seconds played, advanced while the game runs (on the title screen too) */
#define F_LOADING 0xf5deef0au    /* Bool: a save is being loaded */
#define CAP_RUPEES 0x3b58ca14u   /* the capacity member of the wallet */

/* Scene changes in progress (Bool, not saved): nothing is read or written while one is set. */
static const uint32_t transition_flags[] = {0x9a9506e7, 0xf8633c7f, 0xf96989b6, 0xe19dfdd6};
static const uint32_t song_flags[] = {0x01c20002, 0xb492ac33, 0xd3f8bfcc, 0x165dd1ad, 0x03b3450e, 0x2b39dd4f, 0xdc0a0b54,
                                      0x49041ab9, 0x3fa63986, 0xc2fb95f4, 0x802a36a8, 0xd6320ba4, 0x5e86fb4d};
static const uint32_t heart_reward_flags[] = {0x6b8fc751, 0x1d1793e9, 0xb5127e75, 0xe41e1597, 0x98ca7aab, 0xf1891f6e,
                                              0x3f43245b, 0x1657d7d8, 0xa666baa8, 0x5ff7aa30, 0xf2fab510};
static const uint32_t quest_counts[] = {0x692d57cb, 0x5ce254a6, 0x0151a68f, 0x0c7a5fd0, 0x204d2063, 0x235c7d9a,
                                        0xcbd1e58c, 0x7d69ad47, 0xb8e3bf45, 0xf3b14ce2, 0xfeb4d56b};
static const uint32_t own_counts[] = {0x07f10aaf, 0x0165e7ca, 0x51d89ef0, 0x07de3e6f, 0x58c9bc78, 0xa862d276, 0x2815c82a,
                                      0x897cf7eb, 0x919ea324, 0x5c41a6b1, 0xded8e8d3, 0x24596e8e, 0xb6ca6093, 0x6be5c3b0,
                                      0x7e607033, 0x0e94a8df, 0xfc508da7, 0xb5c10899, 0x47b2d775, 0xf046629e, 0x96459cd3,
                                      0x924e9a46, 0xf5e647da, 0x486c59a4, 0x20820479, 0x396a5c02, 0x8d02f5ea,
                                      0x600e29eb, 0x51ec97b3, 0x04735978};
/* Ammunition counts and the capacity member that caps each (0: none known). */
static const struct { uint32_t count, cap_member; } ammo[] = {
    {0xdb1bcd4f, 0xc0f40041}, {0xdf7c517a, 0}, {0x518ec74b, 0x59663556}, {0xf30d9f54, 0x83d2e406},
    {0x5fadf7fc, 0x483d0f84}, {0xb9494411, 0xf9e54315}, {0xb003124d, 0},
};

static int in_list(const uint32_t *list, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++)
        if (list[i] == h) return 1;
    return 0;
}
#define COOP_FLAG_IN(list, h) in_list(list, sizeof list / sizeof list[0], h)

typedef struct {
    uint32_t hash;
    uint8_t is_int, cat, inv;
    uint32_t max;           /* the highest register value the room may set: 1 for a Bool, KEY_MAX or INT_MAX_ITEM for an Int */
    int32_t last;           /* the raw value last reported */
    uint64_t addr;          /* the entry */
    uint64_t cap_addr;      /* a pool counter's capacity entry (Int), 0 none */
} GFlag;

/* A set of hashes (open addressing, 0 empty, cap a power of two), for the struct memberships while the table is built. */
typedef struct { uint32_t *h; unsigned cap; } HSet;
static void hset_add(HSet *s, uint32_t h) {
    if (!h || !s->h) return;
    unsigned i = h & (s->cap - 1), k = 0;
    while (s->h[i] && s->h[i] != h && ++k < s->cap) i = (i + 1) & (s->cap - 1);
    if (k < s->cap) s->h[i] = h;
}
static int hset_has(const HSet *s, uint32_t h) {
    if (!h || !s->h) return 0;
    unsigned i = h & (s->cap - 1);
    for (unsigned k = 0; s->h[i] && k < s->cap; k++, i = (i + 1) & (s->cap - 1))
        if (s->h[i] == h) return 1;
    return 0;
}
/* The struct memberships the classification asks about. */
typedef struct { HSet first_get, pouch, keys, capacity; } Sets;

typedef struct { uint32_t hash; int32_t value; uint32_t frame; int done; } TestSet;
enum { PUB_CONTRIBUTORS = 24 };
enum { ST_UNSEEN, ST_STARTING, ST_OFF, ST_ON };

static struct {
    int state;              /* ST_*: the present's one read (atomic) */
    int on;                 /* progress sync is on (atomic: sync_on, on the game thread or the co-op thread) */
    int probe, trace;
    TestSet test[8];        /* HOS_COOP_TEST_SET */
    int ntest;

    SRWLOCK lock;           /* around eng (the game thread only tries it) */
    CoopSync *eng;
    uint64_t self_id;

    /* the game thread's */
    uint64_t mgr, bool_entries, int_entries;
    GFlag *flags;           /* flags first, then the pool counters */
    int *by_hash;           /* indices of flags in hash order */
    int nflags, npools, cap;
    unsigned count[COOP_CAT_N];
    int bound, gate, slice, checked, refused;
    int loaded;             /* a save was loaded since the title screen last started (its clock runs too) */
    int looked;             /* the gate has looked at the game once since sync was switched on */
    int titles;             /* title_starts when last seen */
    int player;             /* the Player actor stands in the world (player_in_world) */
    uint32_t clock;
    uint64_t clock_at, clock_entry, loading_entry, transition_entry[4];
    uint32_t frames;        /* frames played since the save was bound */
    unsigned applied[COOP_CAT_N], applied_total;
    uint64_t applied_log_at;
    uint32_t schema;        /* of the table: games merge only with the same */
    int probed;

    /* the co-op thread's (G.lock for what the game thread reads too): nothing from a room is applied before the player
       accepted what the room is (the engine's offer: identities, rules and their key, the save) and a copy of the save
       exists to go back to; coop_sync_take checks it again under the lock */
    CoopView view;
    unsigned view_seen;     /* the view's version last handled */
    int view_any;
    char first_rooms[8][COOP_SECRET_TEXT + 1];   /* rooms whose first copy of the save was made in this start */
    unsigned nfirst;
    uint32_t logged;        /* the offer last logged */
    int logged_unid, said_disagree;
    unsigned rules_seen, rejected_seen, ignored_seen, own_key_seen, key_changes_seen, key_moves_seen;
    uint64_t checked_at, own_key_said, spent_seen, spent_at;
    int open;               /* the current offer is accepted and the copy exists: the game thread applies (atomic) */
    int copy_busy_said;

    /* the maker's key for a room (the co-op thread's) */
    char key_code[COOP_SECRET_TEXT + 1];
    uint8_t room_key[32];
    int has_room_key, said_random;

    /* what the menu reads and asks without waiting for G.lock: copies the co-op thread publishes at each step under
       pub_lock, held only for a copy, and requests the co-op thread carries out at its next step (dropped after 2 s) */
    SRWLOCK pub_lock;
    CoopOffer pub;
    int pub_has, pub_maker;
    CoopContributor pub_contrib[PUB_CONTRIBUTORS];
    int pub_ncontrib;
    uint64_t pub_contrib_at;
    uint32_t want_accept;   /* atomic */
    uint64_t want_reset;    /* atomic: when (ms) */
    char want_remove[24];   /* pub_lock */
    uint64_t want_remove_at;
} G = {.lock = SRWLOCK_INIT, .pub_lock = SRWLOCK_INIT};

#define TRACE_ON (G.trace)

/* ---- reading the database (a guest pointer the game holds is read only inside the guest window, above its first MB) */

static int rd_ok(uint64_t a, uint64_t n) { return a >= 0x100000 && hos_guest_range(a, n) != NULL; }

/* The entry of hash in the container at base (stride bytes per entry), 0 when absent. */
static uint64_t entry_of(Ctx *c, uint64_t mgr, unsigned base, unsigned stride, uint32_t hash) {
    const uint64_t k = mgr + base, table = recomp_load64(c, k + CN_TABLE), entries = recomp_load64(c, k + CN_ENTRIES);
    const uint32_t cap = (uint32_t)recomp_load32(c, k + CN_CAP), count = (uint32_t)recomp_load32(c, k + CN_COUNT);
    if (!hash || !cap || !count || !rd_ok(table, 8ull * cap) || !rd_ok(entries, (uint64_t)count * stride)) return 0;
    for (uint32_t i = hash % cap, n = 0; n < cap; n++, i = i + 1 == cap ? 0 : i + 1) {
        const uint32_t h = (uint32_t)recomp_load32(c, table + 8ull * i);
        if (h == hash) {
            const uint32_t idx = (uint32_t)recomp_load32(c, table + 8ull * i + 4);
            return idx < count ? entries + (uint64_t)idx * stride : 0;
        }
        if (!h) return 0;
    }
    return 0;
}

static uint64_t struct_of(Ctx *c, uint64_t mgr, uint32_t hash) {
    const uint64_t table = recomp_load64(c, mgr + ST_TABLE), entries = recomp_load64(c, mgr + ST_ENTRIES);
    const uint32_t cap = (uint32_t)recomp_load32(c, mgr + ST_CAP), count = (uint32_t)recomp_load32(c, mgr + ST_COUNT);
    if (!hash || !cap || !count || !rd_ok(table, 8ull * cap) || !rd_ok(entries, (uint64_t)count * ST_SIZE)) return 0;
    for (uint32_t i = hash % cap, n = 0; n < cap; n++, i = i + 1 == cap ? 0 : i + 1) {
        const uint32_t h = (uint32_t)recomp_load32(c, table + 8ull * i);
        if (h == hash) {
            const uint32_t idx = (uint32_t)recomp_load32(c, table + 8ull * i + 4);
            return idx < count ? entries + (uint64_t)idx * ST_SIZE : 0;
        }
        if (!h) return 0;
    }
    return 0;
}

/* fn(member hash, flag or struct hash, user) for each member of struct entry e. */
static void members(Ctx *c, uint64_t e, void (*fn)(Ctx *c, uint32_t m, uint32_t v, void *u), void *u) {
    const uint64_t t = recomp_load64(c, e + STE_MEMBERS);
    const uint32_t cap = (uint32_t)recomp_load32(c, e + STE_MCAP);
    if (!cap || cap > 65536 || !rd_ok(t, 8ull * cap)) return;
    for (uint32_t i = 0; i < cap; i++) {
        const uint32_t m = (uint32_t)recomp_load32(c, t + 8ull * i);
        if (m) fn(c, m, (uint32_t)recomp_load32(c, t + 8ull * i + 4), u);
    }
}

typedef struct { uint64_t mgr; HSet *set; uint32_t want; } Walk;
static void add_member(Ctx *c, uint32_t m, uint32_t v, void *u) { (void)c, (void)m, hset_add(((Walk *)u)->set, v); }
static void pick(Ctx *c, uint32_t m, uint32_t v, void *u) {
    (void)c;
    Walk *w = u;
    if (m == w->want) hset_add(w->set, v);
}
/* A struct of structs: the flag of member want in each. */
static void add_nested(Ctx *c, uint32_t m, uint32_t v, void *u) {
    (void)m;
    Walk *w = u;
    const uint64_t e = struct_of(c, w->mgr, v);
    if (e) members(c, e, pick, w);
}

typedef struct { uint64_t mgr; uint32_t member; uint64_t flag; } FindMember;
static void find_member(Ctx *c, uint32_t m, uint32_t v, void *u) {
    FindMember *f = u;
    if (m == f->member) f->flag = entry_of(c, f->mgr, C_INT, INT_SIZE, v);
}
/* The Int entry a struct member names, 0 when absent. */
static uint64_t member_entry(Ctx *c, uint64_t mgr, uint32_t s, uint32_t member) {
    const uint64_t e = struct_of(c, mgr, s);
    FindMember f = {mgr, member, 0};
    if (e) members(c, e, find_member, &f);
    return f.flag;
}

/* ---- the classification (the header comment's rules): the category of a flag, -1 when it is not shared */

typedef struct { int cat; uint8_t inv; uint32_t max, cap_member; } Class;

/* Bool (is_int 0) or Int flag hash with its entry's reset kinds and save file (Bool) and its default (Bool: inv = the
   default is true). cap_member: the capacity struct's member that caps a pool counter (0 none). */
static Class classify(int is_int, uint32_t h, uint32_t reset, int32_t file, int default_true, const Sets *s) {
    Class k = {-1, 0, 0, 0};
    if (!is_int) {
        if (file != 0 || reset & RESET_TEMPORARY || !(reset & RESET_NEW_GAME) || COOP_FLAG_IN(heart_reward_flags, h)) return k;
        k.cat = COOP_FLAG_IN(song_flags, h) ? COOP_CAT_SONGS : hset_has(&s->first_get, h) ? COOP_CAT_ITEMS : COOP_CAT_PROGRESS;
        k.inv = default_true != 0;
    } else if (h == F_RUPEES) {
        k.cat = COOP_CAT_RUPEES, k.cap_member = CAP_RUPEES;
    } else if (hset_has(&s->keys, h)) {
        k.cat = COOP_CAT_KEYS;
    } else if (hset_has(&s->pouch, h)) {
        if (COOP_FLAG_IN(own_counts, h)) return k;
        k.cat = COOP_FLAG_IN(quest_counts, h) ? COOP_CAT_QUEST : COOP_CAT_ITEMS;
        for (size_t i = 0; i < sizeof ammo / sizeof ammo[0]; i++)
            if (ammo[i].count == h) k.cat = COOP_CAT_AMMO, k.cap_member = ammo[i].cap_member;
    } else if (hset_has(&s->capacity, h) || h == F_MAGIC_SIZE) {
        k.cat = COOP_CAT_ITEMS;
    }
    if (k.cat >= 0) k.max = !is_int ? 1 : k.cat == COOP_CAT_KEYS ? KEY_MAX : INT_MAX_ITEM;
    return k;
}

static GFlag *add_flag(void) {
    if (G.nflags == G.cap) {
        const int nc = G.cap ? G.cap * 2 : 4096;
        GFlag *nf = realloc(G.flags, (size_t)nc * sizeof *nf);
        if (!nf) return NULL;
        G.flags = nf, G.cap = nc;
    }
    GFlag *f = &G.flags[G.nflags++];
    memset(f, 0, sizeof *f);
    return f;
}

static int cmp_pool_last(const void *a, const void *b) {
    const GFlag *x = a, *y = b;
    const int px = coop_cat_is_pool(x->cat), py = coop_cat_is_pool(y->cat);
    return px != py ? px - py : (x->addr > y->addr) - (x->addr < y->addr);   /* (a total order: qsort is not stable) */
}

static int cmp_by_hash(const void *a, const void *b) {
    const uint32_t x = G.flags[*(const int *)a].hash, y = G.flags[*(const int *)b].hash;
    return x < y ? -1 : x > y;
}

/* The classified table of the manager's flags. 0 when the database is not readable. */
static int build_table(Ctx *c, uint64_t mgr) {
    G.nflags = G.npools = 0;
    memset(G.count, 0, sizeof G.count);
    Sets s;
    HSet *sets[] = {&s.first_get, &s.pouch, &s.keys, &s.capacity};
    for (int i = 0; i < 4; i++) sets[i]->cap = 1024, sets[i]->h = calloc(1024, sizeof *sets[i]->h);
    uint64_t e;
    Walk w = {mgr, &s.first_get, 0};
    if ((e = struct_of(c, mgr, S_FIRST_GET))) members(c, e, add_member, &w);
    w.set = &s.pouch, w.want = M_STOCK;
    if ((e = struct_of(c, mgr, S_POUCH))) members(c, e, add_nested, &w);
    w.set = &s.keys, w.want = M_SMALL_KEYS;
    if ((e = struct_of(c, mgr, S_DUNGEON))) members(c, e, add_nested, &w);
    w.set = &s.capacity;
    if ((e = struct_of(c, mgr, S_CAPACITY))) members(c, e, add_member, &w);

    for (int type = 0; type < 2; type++) {
        const unsigned base = type ? C_INT : C_BOOL, stride = type ? INT_SIZE : BOOL_SIZE;
        const uint64_t k = mgr + base, table = recomp_load64(c, k + CN_TABLE), entries = recomp_load64(c, k + CN_ENTRIES);
        const uint32_t cap = (uint32_t)recomp_load32(c, k + CN_CAP), count = (uint32_t)recomp_load32(c, k + CN_COUNT);
        if (!cap || !count || count > 1000000 || cap > 4000000 || !rd_ok(table, 8ull * cap) ||
            !rd_ok(entries, (uint64_t)count * stride))
            continue;
        for (uint32_t i = 0; i < cap; i++) {
            const uint32_t h = (uint32_t)recomp_load32(c, table + 8ull * i), idx = (uint32_t)recomp_load32(c, table + 8ull * i + 4);
            if (!h || idx >= count) continue;
            const uint64_t a = entries + (uint64_t)idx * stride;
            const Class cl = classify(type, h, (uint32_t)recomp_load32(c, a + E_RESET), (int32_t)recomp_load32(c, a + E_FILE),
                                      !type && recomp_load8(c, a + B_DEFAULT) != 0, &s);
            if (cl.cat < 0) continue;
            GFlag *f = add_flag();
            if (!f) break;
            f->hash = h, f->is_int = (uint8_t)type, f->cat = (uint8_t)cl.cat, f->inv = cl.inv, f->addr = a, f->max = cl.max;
            f->cap_addr = cl.cap_member ? member_entry(c, mgr, S_CAPACITY, cl.cap_member) : 0;
            G.count[cl.cat]++;
        }
    }
    for (int i = 0; i < 4; i++) free(sets[i]->h);
    qsort(G.flags, (size_t)G.nflags, sizeof *G.flags, cmp_pool_last);   /* the scan reads flags in slices, the pools every frame */
    for (int i = 0; i < G.nflags; i++) G.npools += coop_cat_is_pool(G.flags[i].cat);
    free(G.by_hash);
    G.by_hash = malloc((size_t)(G.nflags ? G.nflags : 1) * sizeof *G.by_hash);
    if (!G.by_hash) return G.nflags = 0;
    for (int i = 0; i < G.nflags; i++) G.by_hash[i] = i;
    qsort(G.by_hash, (size_t)G.nflags, sizeof *G.by_hash, cmp_by_hash);
    /* the schema: this layer's version, the game's build, and the table (every hash with its category, type and limit),
       so that two builds that classify or limit differently do not merge (FNV-1a) */
    uint64_t sh = 0xcbf29ce484222325ull;
    for (const char *p = "hookshot-coop-sync/1 "; *p; p++) sh = (sh ^ (uint8_t)*p) * 0x100000001b3ull;
    for (const char *p = hos_main_build_id; *p; p++) sh = (sh ^ (uint8_t)*p) * 0x100000001b3ull;
    for (int i = 0; i < G.nflags; i++) {
        const GFlag *f = &G.flags[G.by_hash[i]];
        const uint64_t row[2] = {f->hash | (uint64_t)f->cat << 32 | (uint64_t)f->is_int << 40, f->max};
        for (int b = 0; b < 16; b++) sh = (sh ^ (uint8_t)(row[b / 8] >> 8 * (b % 8))) * 0x100000001b3ull;
    }
    G.schema = (uint32_t)(sh ^ sh >> 32);
    if (!G.schema) G.schema = 1;   /* (0 is the engine's "not known yet") */
    return G.nflags > 0;
}

static int32_t raw_of(Ctx *c, const GFlag *f) {
    return f->is_int ? (int32_t)recomp_load32(c, f->addr + I_CURRENT) : (int32_t)recomp_load8(c, f->addr + B_CURRENT);
}
static uint32_t reg_of(const GFlag *f, int32_t raw) {
    if (!f->is_int) return (raw != 0) != f->inv;   /* 1: differs from its default */
    return raw > 0 ? (uint32_t)raw : 0;
}

static GFlag *flag_by_hash(uint32_t hash) {
    int lo = 0, hi = G.nflags - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        GFlag *f = &G.flags[G.by_hash[mid]];
        if (f->hash == hash) return f;
        if (f->hash < hash) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* A set of this hash waits on the Int ring (the game changed it this frame): a pool write now would overwrite it. */
static int int_pending(Ctx *c, uint64_t mgr, uint32_t hash) {
    const uint64_t k = mgr + C_INT, ring = recomp_load64(c, k + CN_RING);
    const uint32_t n = (uint32_t)recomp_load32(c, k + CN_RING_CTR) & 0xFFFFF, cap = (uint32_t)recomp_load32(c, k + CN_RING_CAP);
    if (n > cap || (n && !rd_ok(ring, 12ull * n))) return 1;
    for (uint32_t i = 0; i < n; i++)
        if ((uint32_t)recomp_load32(c, ring + 12ull * i + 4) == hash) return 1;
    return 0;
}

/* What the game is given for apply a of flag f (the engine checked it against the limits already; this is the check of
   last resort): 1 and the value to set, 0 when it is refused. A flag of another category here, a Bool set to anything
   but "changed", a count above the flag's limit or below 0: refused. Values are only raised: a flag is set to the value
   it does not have by default, an Int written only above what the game shows, except a pool counter, whose value is
   the pool's (it may go down when another player spends, within 0 and the limit). */
static int apply_value(const GFlag *f, const CoopApply *a, int32_t now, int32_t *out) {
    if (!f || f->cat != a->cat || coop_cat_is_pool(f->cat) != (a->pool != 0)) return 0;
    const int32_t v = a->value;
    if (!f->is_int) {
        if (a->pool || v != 1) return 0;
        *out = !f->inv;
        return now != *out;
    }
    if (v < 0 || v > (int32_t)f->max) return 0;
    if (!a->pool && v <= now) return 0;
    *out = v;
    return v != now;
}

/* ---- calling the game, on the presenting thread: the guest's registers as they were at the present afterwards */

static int game_get_bool(Ctx *c, uint64_t mgr, uint32_t hash, int *value) {
    A64Fn get = a64_lookup(GET_BOOL_FN);
    if (!get) return 0;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = hash, c->x[30] = 0;
    get(c);
    const int found = (int)(c->x[0] & 1);
    *value = (int)(recomp_load8(c, out) & 1);
    *c = saved;
    return found;
}

static int set_flag(Ctx *c, uint64_t mgr, int is_int, uint32_t hash, int32_t value) {
    A64Fn set = a64_lookup(is_int ? SET_INT_FN : SET_BOOL_FN);
    if (!set) return 0;
    const Ctx saved = *c;
    c->x[0] = mgr, c->x[1] = (uint64_t)(uint32_t)value, c->x[2] = hash, c->x[30] = 0;
    set(c);
    *c = saved;
    return 1;
}

/* The table read both ways before a save is shared: CHECK_FLAGS Bool flags spread over the table and the scene-change
   flags, each by the table and by the game's getter. The mismatches (0: the table is as this file reads it). */
static int check_table(Ctx *c, uint64_t mgr, int *checked) {
    const uint64_t k = mgr + C_BOOL, table = recomp_load64(c, k + CN_TABLE), entries = recomp_load64(c, k + CN_ENTRIES);
    const uint32_t cap = (uint32_t)recomp_load32(c, k + CN_CAP), count = (uint32_t)recomp_load32(c, k + CN_COUNT);
    int bad = 0, n = 0;
    *checked = 0;
    if (!cap || !count || !rd_ok(table, 8ull * cap) || !rd_ok(entries, (uint64_t)count * BOOL_SIZE)) return 1;
    uint32_t hashes[CHECK_FLAGS + 4];
    for (uint32_t i = 0, step = cap / CHECK_FLAGS ? cap / CHECK_FLAGS : 1; i < cap && n < CHECK_FLAGS; i += step) {
        uint32_t j = i;
        while (j < cap && !recomp_load32(c, table + 8ull * j)) j++;
        if (j < cap) hashes[n++] = (uint32_t)recomp_load32(c, table + 8ull * j);
    }
    for (int i = 0; i < 4; i++) hashes[n++] = transition_flags[i];
    for (int i = 0; i < n; i++) {
        const uint64_t e = entry_of(c, mgr, C_BOOL, BOOL_SIZE, hashes[i]);
        int got = 0;
        const int found = game_get_bool(c, mgr, hashes[i], &got);
        if (!found && !e) continue;
        ++*checked;
        if (!found || !e || (int)(recomp_load8(c, e + B_CURRENT) & 1) != got) {
            bad++;
            hos_log("coop: sync: flag %08x reads %d in the table (entry %#llx) and %d (found %d) by the game's getter", hashes[i],
                    e ? (int)recomp_load8(c, e + B_CURRENT) : -1, (unsigned long long)e, got, found);
        }
    }
    return bad;
}

/* ---- settings */

/* A category's mode: HOS_COOP_<CAT> (shared, own, pool) over the options file's coop_<cat> (the Co-op tab's rows: 0 own,
   1 shared, 2 pool); -1 when neither is set, -2 when the one set names a mode the category does not have. */
static int mode_of(const char *env, const char *key, int pool) {
    const char *v = getenv(env);
    int m = hos_ini_option(key, -1);
    if (v && *v) m = !strcmp(v, "own") ? COOP_MODE_OWN : !strcmp(v, "shared") ? COOP_MODE_SHARED : !strcmp(v, "pool") ? COOP_MODE_POOL : -2;
    return m == -1 || m == COOP_MODE_OWN || m == (pool ? COOP_MODE_POOL : COOP_MODE_SHARED) ? m : -2;
}

static void read_rules(CoopRules *r, int say) {
    static const char *const envs[COOP_CAT_N] = {"HOS_COOP_PROGRESS", "HOS_COOP_ITEMS", "HOS_COOP_SONGS", "HOS_COOP_QUEST",
                                                 "HOS_COOP_KEYS", "HOS_COOP_RUPEES", "HOS_COOP_AMMO"};
    static const char *const keys[COOP_CAT_N] = {"coop_progress", "coop_items", "coop_songs", "coop_quest", "coop_keys",
                                                 "coop_rupees", "coop_ammo"};
    coop_rules_default(r);
    for (int k = 0; k < COOP_CAT_N; k++) {
        const int m = mode_of(envs[k], keys[k], coop_cat_is_pool(k));
        if (m >= 0) r->mode[k] = (uint8_t)m;
        else if (m == -2 && say)
            hos_log("coop: sync: %s (or %s in the options file) is not %s; the default, %s, is used", envs[k], keys[k],
                    coop_cat_is_pool(k) ? "pool or own" : "shared or own", coop_mode_name(r->mode[k]));
    }
    CoopRules as_set = *r;
    if (coop_rules_normalize(r) && say) {
        char a[200], b[200];
        coop_rules_text(&as_set, a, sizeof a);
        coop_rules_text(r, b, sizeof b);
        hos_log("coop: sync: the settings ask for %s; a room made with them uses %s", a, b);
    }
}

/* "hash=value@frame" items separated by ',' or ';' (hash in hex): set that flag at that frame of play, for tests. */
static void read_test_aid(void) {
    G.ntest = 0;
    const char *t = getenv("HOS_COOP_TEST_SET");
    while (t && *t && G.ntest < (int)(sizeof G.test / sizeof G.test[0])) {
        char *end;
        const unsigned long h = strtoul(t, &end, 16);
        if (*end != '=') break;
        const long v = strtol(end + 1, &end, 0);
        if (*end != '@') break;
        const unsigned long fr = strtoul(end + 1, &end, 0);
        G.test[G.ntest++] = (TestSet){(uint32_t)h, (int32_t)v, (uint32_t)fr, 0};
        while (*end == ',' || *end == ';' || *end == ' ') end++;
        t = end;
    }
    if (t && *t) hos_log("coop: test: HOS_COOP_TEST_SET is \"hash=value@frame,...\" (hex hash); stopped at \"%s\"", t);
}

/* ---- the engine's side of the co-op thread */

static void trace_line(void *user, const char *line) { (void)user, hos_log("coop: sync: %s", line); }

/* The engine's limits (coop_sync_limits), with G.lock held: a flag or counter of the table in that category. */
static uint32_t limit_fn(int cat, uint32_t hash, void *user) {
    (void)user;
    const GFlag *f = flag_by_hash(hash);
    return f && f->cat == cat ? f->max : 0;
}

static int ensure_engine(void) {
    if (G.eng) return 1;
    G.self_id = coop_client_id();
    CoopSync *e = coop_sync_new(G.self_id, 0);   /* the schema is the table's, once a save is played (bind) */
    if (!e) return 0;
    uint8_t key[16 + 32];   /* the index's key, and the seed of this start's identities and offer numbers */
    if (!G.self_id || !coop_random(key, sizeof key)) {   /* a seed others could guess would let them sign as this player */
        coop_sync_free(e);
        if (!G.said_random) hos_log("coop: sync: the system's random source failed; progress sync stays off");
        G.said_random = 1;
        return 0;
    }
    coop_sync_index_key(e, key);
    coop_sync_seed(e, key + 16);
    coop_sync_limits(e, limit_fn, NULL);
    if (TRACE_ON) coop_sync_trace(e, trace_line, NULL);
    AcquireSRWLockExclusive(&G.lock);
    G.eng = e;
    ReleaseSRWLockExclusive(&G.lock);
    return 1;
}

static int send_fn(int member, uint8_t kind, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    return member ? coop_send_to(member, kind, bytes, n) : coop_send(kind, bytes, n);
}

static void on_message(int from, const uint8_t *bytes, size_t n, void *user) {
    const uint8_t kind = (uint8_t)(uintptr_t)user;
    AcquireSRWLockExclusive(&G.lock);
    if (G.eng) coop_sync_receive(G.eng, from, kind, bytes, n, coop_now_ms());
    ReleaseSRWLockExclusive(&G.lock);
}

/* A file of n bytes only this account reads: written beside path and moved over it. 1 when done. */
static int write_private(const char *path, const void *bytes, size_t n) {
    char tmp[1100];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return 0;
#ifdef _WIN32
    FILE *f = fopen(tmp, "wb");
    const int ok = f && fwrite(bytes, 1, n, f) == n;
    if (f && fclose(f)) return 0;
#else
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int ok = fd >= 0 && write(fd, bytes, n) == (ssize_t)n;
    if (fd >= 0) ok &= fsync(fd) == 0, ok &= close(fd) == 0;
#endif
    return ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

static void make_private_dir(const char *dir) {
#ifdef _WIN32
    CreateDirectoryA(dir, NULL);
#else
    mkdir(dir, 0700);
    chmod(dir, 0700);   /* (a folder an earlier run made readable to others) */
#endif
}

/* The maker's key for the room of code (32 random bytes) in <user>/coop-keys, so that a maker whose game started again
   signs the room's rules and removals with the same key. The file is named by a hash of the room's secret, and the key
   is mixed with the secret before use (coop_state.c), so the file alone signs nothing; with the secret, whoever can read
   it acts as the room's maker, so the folder is this account's alone (0700, the file 0600). make: a room this session
   made gets one when it has none. 1 and the key in key when there is one. */
static int room_key(const char *code, int make, uint8_t key[32]) {
    if (!hos_user_dir) return 0;
    uint8_t h[8];
    coop_blake2b(h, sizeof h, NULL, 0, code, strlen(code));
    char dir[1024], path[1100];
    if ((size_t)snprintf(dir, sizeof dir, "%s/coop-keys", hos_user_dir) >= sizeof dir) return 0;
    snprintf(path, sizeof path, "%s/%02x%02x%02x%02x%02x%02x%02x%02x.key", dir, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    FILE *f = fopen(path, "rb");
    uint8_t b[33];
    const size_t n = f ? fread(b, 1, sizeof b, f) : 0;
    if (f) fclose(f);
    if (n == 32) {
        memcpy(key, b, 32);
        make_private_dir(dir);
#ifndef _WIN32
        chmod(path, 0600);
#endif
        return 1;
    }
    if (f)
        hos_log("coop: sync: the room's key file in %s is damaged (not 32 bytes); %s", dir,
                make ? "a new key replaces it, so this game no longer signs the rules the room accepted from it"
                     : "this game does not sign the room's rules");
    if (!make || !coop_random(key, 32)) return 0;
    make_private_dir(dir);
    if (!write_private(path, key, 32)) {
        hos_log("coop: sync: the room's key could not be kept in %s; a restart of this game ends its say over the room's rules", dir);
        return 1;
    }
    hos_log("coop: sync: the key this game signs the room's rules with is kept in %s (with the room's secret it acts as the "
            "room's maker: never hand out a copy of this folder or of the user folder)", path);
    return 1;
}

static void view_changed(const CoopView *v) {
    int ids[COOP_MAX_MEMBERS], n = 0;
    uint64_t sessions[COOP_MAX_MEMBERS];
    const int in_room = v->state == COOP_JOINED || v->state == COOP_RECONNECTING;
    for (int i = 0; in_room && i < v->n; i++) {
        /* this player's own session is the one it joined with (the engine signs for it), whatever a roster lists */
        sessions[n] = v->member[i].id == v->self && v->session ? v->session : v->member[i].session;
        ids[n++] = v->member[i].id;
    }
    if (in_room && (strcmp(G.key_code, v->code) || (v->made && !G.has_room_key))) {   /* the file once per room, outside the lock */
        snprintf(G.key_code, sizeof G.key_code, "%s", v->code);
        G.has_room_key = room_key(v->code, v->made, G.room_key);
    }
    AcquireSRWLockExclusive(&G.lock);
    G.view = *v;
    if (G.eng) {
        coop_sync_name(G.eng, v->name);   /* the player's own name, signed: the relay's roster may say another */
        if (in_room && G.has_room_key) coop_sync_room_seed(G.eng, v->code, G.room_key);
        coop_sync_room(G.eng, in_room ? v->code : "", v->self, ids, sessions, n, in_room ? v->maker : 0, coop_now_ms());
        CoopRules mine;
        read_rules(&mine, 0);
        coop_sync_settings(G.eng, &mine, in_room && v->made);
    }
    ReleaseSRWLockExclusive(&G.lock);
}

/* Progress sync is on from here, once: at the game's start when HOS_COOP or the options file's coop= asks for co-op
   (start), or when the session first calls back, which it does only while its thread runs: co-op switched on in the
   Co-op tab after the start (coop_configure). Says what the settings ask for, then lets the present read the game. */
static void sync_on(void) {
    if (__atomic_exchange_n(&G.on, 1, __ATOMIC_ACQ_REL)) return;
    CoopRules r;
    read_rules(&r, 1);
    __atomic_store_n(&G.state, ST_ON, __ATOMIC_RELEASE);
}

static void on_view(const CoopView *v, void *user) {
    (void)user;
    sync_on();
    G.view_seen = v->version, G.view_any = 1;
    view_changed(v);
}

/* HOS_COOP_ACCEPT=always: every offer accepted, from the environment only (hookshot_env.txt, a file players hand each
   other, cannot set it: host/hos_main.c env_line_allowed), so a file nobody checked cannot make a game take a room. */
static int accept_always(void) {
    const char *v = getenv("HOS_COOP_ACCEPT");
    return v && !strcmp(v, "always");
}

/* The engine's offer, with the relay's names for members not identified yet (G.lock held). */
static int offer_of(CoopOffer *o, uint64_t now) {
    const int has = coop_sync_offer(G.eng, o, now);
    for (int i = 0; i < o->n; i++)
        for (int k = 0; !o->member[i].name[0] && k < G.view.n; k++)
            if (G.view.member[k].id == o->member[i].id) snprintf(o->member[i].name, sizeof o->member[i].name, "%s", G.view.member[k].name);
    return has;
}

/* What the menu reads (G.lock held): the offer offer_update settled on, whether this game is the maker, the
   contributors at most every 500 ms. */
static void publish(const CoopOffer *o, int has, uint64_t now) {
    const int maker = coop_sync_is_maker(G.eng), fresh = !G.pub_contrib_at || now - G.pub_contrib_at >= 500;
    CoopContributor k[PUB_CONTRIBUTORS];
    const int n = fresh ? coop_sync_contributors(G.eng, k, PUB_CONTRIBUTORS) : 0;
    AcquireSRWLockExclusive(&G.pub_lock);
    G.pub = *o, G.pub_has = has, G.pub_maker = maker;
    if (fresh) memcpy(G.pub_contrib, k, sizeof *k * (size_t)n), G.pub_ncontrib = n, G.pub_contrib_at = now;
    ReleaseSRWLockExclusive(&G.pub_lock);
}

/* No engine (co-op off, or no save played yet): nothing to read. */
static void publish_none(void) {
    AcquireSRWLockExclusive(&G.pub_lock);
    if (G.pub_has || G.pub.n || G.pub_ncontrib || G.pub_maker)
        memset(&G.pub, 0, sizeof G.pub), G.pub_has = G.pub_maker = G.pub_ncontrib = 0, G.pub_contrib_at = 0;
    ReleaseSRWLockExclusive(&G.pub_lock);
}

/* The offer of the room (the co-op thread, G.lock held): accepted by the menu or the switch, logged when it changed and
   is not accepted, and published for the menu. */
static void offer_update(uint64_t now) {
    CoopOffer o;
    const uint32_t menu = __atomic_exchange_n(&G.want_accept, 0, __ATOMIC_ACQ_REL);   /* of the offer the menu showed */
    int has = offer_of(&o, now);
    const char *how = !has || o.accepted ? NULL : menu == o.token ? "in the menu" : accept_always() ? "HOS_COOP_ACCEPT=always" : NULL;
    if (how && coop_sync_accept(G.eng, o.token)) {
        hos_log("coop: sync: room accepted (%s); %s", how, coop_sync_copy_wanted(G.eng) ? "the save is copied first" : "values from it are applied");
        has = offer_of(&o, now);
    }
    if (o.token && !o.accepted && (o.token != G.logged || o.unidentified != G.logged_unid)) {
        const int again = G.logged != 0;
        G.logged = o.token, G.logged_unid = o.unidentified;
        char who[COOP_MAX_MEMBERS * 160] = "", rules[200];
        size_t at = 0;
        for (int i = 0; i < o.n && at < sizeof who; i++) {
            const CoopOfferMember *m = &o.member[i];
            at += (size_t)snprintf(who + at, sizeof who - at, "%s%s (member %d, %s%s%s%s%s%s)", i ? ", " : "", m->name, m->id,
                                   m->heard ? "key " : "not identified", m->key, m->accepted_as[0] ? ", a key you accepted as " : "",
                                   m->accepted_as, m->maker ? ", signs the rules" : "", m->removed ? ", removed by the maker: ignored" : "");
        }
        coop_rules_text(&o.rules, rules, sizeof rules);
        hos_log("coop: sync: %s; in the room: %s; rules: %s, signed by key %s",
                o.new_save ? "another save is played in the room: it gets the room's progress, which holds what the last save brought"
                : again    ? "the room changed (a member, the rules or the key that signs them)"
                           : "a room to accept",
                o.n ? who : "nobody else", rules, o.maker_key);
        if (o.maker_moved) hos_log("coop: sync: the relay names another member as the maker than the one whose key signs the rules; the signed rules stay");
        if (o.disagree)
            hos_log("coop: sync: members carry rules signed by different keys: before accepting, compare the key that signs these rules with the "
                    "maker's, by voice or chat");
        if (o.unidentified)
            hos_log("coop: sync: %d member(s) in the room are not identified (never said who they are, or two keys present themselves for "
                    "one): the room cannot be accepted while they are in it (the maker can remove them, or everyone moves to a new room)",
                    o.unidentified);
        else
            hos_log("coop: sync: nothing from the room is applied to this game until you accept it on the Co-op tab (offer %08x); the "
                    "save is copied to coop-backup in the user folder first. Your key pools count only the players you accept, among "
                    "those the maker lets use the pools at all; your story flags take what the players you accept took from the "
                    "players they accepted", o.token);
    }
    if (!o.token && o.disagree && !G.said_disagree)
        hos_log("coop: sync: the members carry rules signed by different keys, so this game takes no rules and applies nothing; ask the "
                "room's maker which key is theirs");
    G.said_disagree = !o.token && o.disagree;
    __atomic_store_n(&G.open, coop_sync_open(G.eng), __ATOMIC_RELEASE);
    publish(&o, has, now);
}

int hos_oot_coop_offer(CoopOffer *out) {
    AcquireSRWLockShared(&G.pub_lock);
    *out = G.pub;
    const int has = G.pub_has;
    ReleaseSRWLockShared(&G.pub_lock);
    return has;
}

int hos_oot_coop_accept(uint32_t token) {
    AcquireSRWLockShared(&G.pub_lock);
    const int same = token && G.pub_has && token == G.pub.token, done = same && G.pub.accepted;
    const int ok = same && !G.pub.accepted && !G.pub.unidentified;
    ReleaseSRWLockShared(&G.pub_lock);
    if (ok) __atomic_store_n(&G.want_accept, token, __ATOMIC_RELEASE);
    return ok || done;
}

int hos_oot_coop_is_maker(void) {
    AcquireSRWLockShared(&G.pub_lock);
    const int maker = G.pub_maker;
    ReleaseSRWLockShared(&G.pub_lock);
    return maker;
}

/* The maker's pool reset (G.lock held). A player still in the room when the pools are reset joins the new ones with
   what it took already taken, so a drainer is removed first. */
static void reset_pools(void) {
    const int ok = G.eng && coop_sync_reset_pools(G.eng);
    G.pub_contrib_at = 0;
    if (ok)
        hos_log("coop: sync: the pools are reset: every game joins them again with what they held for the players in the room; a player "
                "who drained a pool and is still in the room keeps what it took, so remove it first, then reset");
    else hos_log("coop: sync: pool reset refused: only the room's maker resets the pools");
}

int hos_oot_coop_reset_pools(void) {
    const int maker = hos_oot_coop_is_maker();
    if (maker) __atomic_store_n(&G.want_reset, coop_now_ms(), __ATOMIC_RELEASE);
    else hos_log("coop: sync: pool reset refused: only the room's maker resets the pools");
    return maker;
}

/* The maker removes a key (G.lock held): a member that left, or one in the room. */
static void remove_key(const char *fingerprint) {
    uint64_t kid;
    const int ok = G.eng && coop_sync_key_parse(fingerprint, &kid) && coop_sync_remove_key(G.eng, kid);
    if (ok) hos_log("coop: sync: key %s removed: every game drops it and its pool records, and ignores it from now on", fingerprint);
    else hos_log("coop: sync: key %s was not removed: only the room's maker removes, and only a key the room knows", fingerprint);
    G.pub_contrib_at = 0;
}

int hos_oot_coop_remove_key(const char *fingerprint) {
    uint64_t kid;
    if (!fingerprint || !coop_sync_key_parse(fingerprint, &kid)) return 0;
    AcquireSRWLockExclusive(&G.pub_lock);
    const int maker = G.pub_maker;
    if (maker) snprintf(G.want_remove, sizeof G.want_remove, "%s", fingerprint), G.want_remove_at = coop_now_ms();
    ReleaseSRWLockExclusive(&G.pub_lock);
    if (!maker) hos_log("coop: sync: key %s was not removed: only the room's maker removes", fingerprint);
    return maker;
}

int hos_oot_coop_contributors(CoopContributor *out, int max) {
    AcquireSRWLockShared(&G.pub_lock);
    const int n = G.pub_ncontrib < max ? G.pub_ncontrib : max;
    if (n > 0) memcpy(out, G.pub_contrib, sizeof *out * (size_t)n);
    ReleaseSRWLockShared(&G.pub_lock);
    return n > 0 ? n : 0;
}

/* A removal by fingerprint from the menu (the co-op thread, without G.lock): a member in the room that holds the key is
   removed at the relay too (coop_remove_member signs it through on_remove). */
static void remove_by_fingerprint(const char *fp) {
    uint64_t kid = 0, k;
    int member = 0;
    CoopOffer o;
    o.n = 0;
    AcquireSRWLockExclusive(&G.lock);
    if (G.eng && coop_sync_key_parse(fp, &kid)) coop_sync_offer(G.eng, &o, coop_now_ms());
    for (int i = 0; i < o.n; i++)
        if (o.member[i].heard && coop_sync_key_parse(o.member[i].key, &k) && k == kid) member = o.member[i].id;
    ReleaseSRWLockExclusive(&G.lock);
    if (member && coop_remove_member(member)) return;
    AcquireSRWLockExclusive(&G.lock);
    remove_key(fp);
    ReleaseSRWLockExclusive(&G.lock);
}

/* coop_remove_member's hook (coop.h): the maker's removal, signed, so that every game drops that identity. */
static int on_remove(int member, void *user) {
    (void)user;
    AcquireSRWLockExclusive(&G.lock);
    const int ok = G.eng && coop_sync_remove(G.eng, member);
    ReleaseSRWLockExclusive(&G.lock);
    if (ok) hos_log("coop: sync: member %d removed: every game drops its identity and its pool records", member);
    return ok;
}

/* For the maker: the pool contributors that took more from the pools than they brought, when that changed (at most every
   10 s), G.lock held. A player who used keys others found is among them too; a drainer is the one nobody expected. */
static void say_spenders(uint64_t now) {
    if (now - G.spent_at < 10000 || !coop_sync_is_maker(G.eng)) return;
    CoopContributor k[24];
    const int n = coop_sync_contributors(G.eng, k, 24);
    uint64_t h = 0;
    for (int i = 0; i < n; i++)
        if (!k[i].self && !k[i].removed && (k[i].spent[0] > 0 || k[i].spent[1] > 0 || k[i].spent[2] > 0))
            for (int j = 0; j < 3; j++) h = (h ^ (uint64_t)k[i].spent[j] ^ (uint64_t)(uint8_t)k[i].key[j * 5] ^ (uint64_t)k[i].member << 40) * 0x100000001b3ull;
    if (h == G.spent_seen) return;
    G.spent_seen = h, G.spent_at = now;
    for (int i = 0; h && i < n; i++) {
        if (k[i].self || k[i].removed || !(k[i].spent[0] > 0 || k[i].spent[1] > 0 || k[i].spent[2] > 0)) continue;
        char who[COOP_NAME_MAX + 40];
        if (k[i].member) snprintf(who, sizeof who, "member %d, %s", k[i].member, k[i].name);
        else snprintf(who, sizeof who, "not in the room");
        hos_log("coop: sync: key %s (%s) took %lld keys, %lld rupees and %lld ammunition from the pools beyond what it brought; "
                "removing it gives back what it took (remove it before a pool reset: a reset while it is in the room keeps what it took)",
                k[i].key, who, (long long)k[i].spent[0], (long long)k[i].spent[1], (long long)k[i].spent[2]);
    }
}

/* The copy of the save an accepted room waits for, on the co-op thread without G.lock (it takes a while). The first copy
   for a room in this start of the game is kept out of the rotation ("first-<time>"), so the save from before the room
   outlasts the copies of later acceptances. A save being written meanwhile: the copy waits for the next step. */
static void make_snapshot(void) {
    AcquireSRWLockExclusive(&G.lock);
    const int want = G.eng && coop_sync_copy_wanted(G.eng);
    char room[sizeof G.view.code];
    snprintf(room, sizeof room, "%s", G.view.code);
    int first = 1;
    for (unsigned i = 0; i < G.nfirst && i < 8; i++) first &= strcmp(G.first_rooms[i], room) != 0;
    ReleaseSRWLockExclusive(&G.lock);
    if (!want) return;
    char path[1200];
    const int ok = hos_save_copy("coop-backup", first ? "first-" : NULL, 8, path, sizeof path);
    if (ok < 0) {
        if (!G.copy_busy_said) hos_log("coop: sync: the game is writing its save; the copy waits for it to finish");
        G.copy_busy_said = 1;
        return;
    }
    G.copy_busy_said = 0;
    if (ok && path[0])
        hos_log("coop: sync: the save as it was before this room%s is in %s (docs/coop.md: going back)%s", first ? "" : " was accepted again",
                path, first ? "; it is kept until you delete it" : "");
    else if (ok) hos_log("coop: sync: nothing saved yet, so nothing to copy before this room");
    else hos_log("coop: sync: the save could not be copied; nothing from the room is applied (accept again to try once more)");
    AcquireSRWLockExclusive(&G.lock);
    if (G.eng) coop_sync_copied(G.eng, ok);
    if (ok && first) snprintf(G.first_rooms[G.nfirst++ % 8], sizeof G.first_rooms[0], "%s", room);
    __atomic_store_n(&G.open, G.eng && coop_sync_open(G.eng), __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&G.lock);
}

/* What the engine refused and the identities it questioned, once a second (G.lock held). */
static void say_stats(uint64_t now) {
    if (now - G.checked_at < 1000) return;
    G.checked_at = now;
    CoopSyncStats st;
    coop_sync_stats(G.eng, &st);
    const unsigned ign = coop_sync_ignored(G.eng);
    if (st.rejected != G.rejected_seen)
        hos_log("coop: sync: %u values from the room were refused (flags this game does not share or values it does not take); %u in "
                "this session", st.rejected - G.rejected_seen, st.rejected);
    for (int id = 1; id <= COOP_MAX_MEMBERS; id++)
        if (ign >> id & 1 && !(G.ignored_seen >> id & 1))
            hos_log("coop: sync: member %d sent values this game does not take; nothing more is taken from it while it stays in the room", id);
    G.rejected_seen = st.rejected, G.ignored_seen = ign;
    if (st.own_key != G.own_key_seen && (!G.own_key_said || now - G.own_key_said >= 60000)) {
        G.own_key_said = now, G.own_key_seen = st.own_key;
        hos_log("coop: sync: member %d presents this game's own key for the room: another game holds a copy of this user folder's "
                "coop-keys folder (or of the whole user folder) and can act as the room's maker; it stays not identified, so nobody can "
                "accept the room while it is in it", st.own_key_member);
    }
    if (st.key_changes != G.key_changes_seen) {
        G.key_changes_seen = st.key_changes;
        hos_log("coop: sync: member %d presents another key than the one it was identified by in its session (%u times in this room): "
                "it counts as nobody while both are presented, and the other takes its place 10 s after the first stops; compare the "
                "fingerprints the room's offer shows", st.key_change_member, st.key_changes);
    }
    if (st.key_moves != G.key_moves_seen) {
        G.key_moves_seen = st.key_moves;
        CoopOffer o;
        const CoopOfferMember *m = NULL;
        coop_sync_offer(G.eng, &o, now);
        for (int i = 0; i < o.n; i++)
            if (o.member[i].id == st.key_move_member) m = &o.member[i];
        char was[48] = "";
        if (st.key_move_from && st.key_move_from != st.key_move_member) snprintf(was, sizeof was, " as member %d", st.key_move_from);
        hos_log("coop: sync: member %d (%s, key %s) presents a key you accepted%s%s%s%s", st.key_move_member, m ? m->name : "?",
                m ? m->key : "?", was, m && m->accepted_as[0] ? " under the name " : "", m ? m->accepted_as : "",
                m && m->accepted_as[0] ? ": under this name it is a new offer" : "");
    }
    say_spenders(now);
}

static void on_tick(uint64_t now, void *user) {
    (void)user;
    sync_on();
    {   /* a room joined before these hooks were registered, or a view hook that came and went: the view as it is */
        CoopView v;
        coop_view_get(&v);
        if (!G.view_any || v.version != G.view_seen) G.view_seen = v.version, G.view_any = 1, view_changed(&v);
    }
    make_snapshot();
    char asked[sizeof G.want_remove];
    AcquireSRWLockExclusive(&G.pub_lock);
    snprintf(asked, sizeof asked, "%s", now - G.want_remove_at < 2000 ? G.want_remove : "");
    G.want_remove[0] = 0;
    ReleaseSRWLockExclusive(&G.pub_lock);
    if (asked[0]) remove_by_fingerprint(asked);
    const uint64_t reset_at = __atomic_exchange_n(&G.want_reset, 0, __ATOMIC_ACQ_REL);
    AcquireSRWLockExclusive(&G.lock);
    if (!G.eng) {
        ReleaseSRWLockExclusive(&G.lock);
        publish_none();
        return;
    }
    if (reset_at && now - reset_at < 2000) reset_pools();
    coop_sync_tick(G.eng, now, send_fn, NULL);
    CoopRules r;
    const unsigned ver = coop_sync_rules(G.eng, &r);
    if (ver != G.rules_seen) {
        G.rules_seen = ver;
        if (r.epoch) {
            char text[200];
            coop_rules_text(&r, text, sizeof text);
            hos_log("coop: sync: the room's rules: %s (from member %d)", text, r.maker);
        }
    }
    offer_update(now);
    say_stats(now);
    ReleaseSRWLockExclusive(&G.lock);
}

/* ---- the game thread */

static void unbind(const char *why) {
    if (!G.bound) return;
    G.bound = 0;
    hos_log("coop: sync: %s; nothing is read or written until a save is played", why);
}

static void say_counts(const char *what) {
    hos_log("coop: sync: %s: %u progress flags, %u items, %u songs, %u quest items, %u key counters, %u rupee counter, %u "
            "ammunition counters; the room's rules decide which are shared",
            what, G.count[COOP_CAT_PROGRESS], G.count[COOP_CAT_ITEMS], G.count[COOP_CAT_SONGS], G.count[COOP_CAT_QUEST],
            G.count[COOP_CAT_KEYS], G.count[COOP_CAT_RUPEES], G.count[COOP_CAT_AMMO]);
}

static void bind_tables(Ctx *c, uint64_t mgr) {
    const uint64_t be = recomp_load64(c, mgr + C_BOOL + CN_ENTRIES), ie = recomp_load64(c, mgr + C_INT + CN_ENTRIES);
    if (!TryAcquireSRWLockExclusive(&G.lock)) return;   /* the co-op thread has the engine: the next frame */
    if (mgr != G.mgr || be != G.bool_entries || ie != G.int_entries || !G.nflags) {
        G.checked = 0;
        if (!build_table(c, mgr)) {   /* under the lock: the engine's limits read the table on the co-op thread */
            ReleaseSRWLockExclusive(&G.lock);
            return;
        }
        G.mgr = mgr, G.bool_entries = be, G.int_entries = ie;
    }
    if (!G.checked) {   /* the table against the game's getter, once per table */
        int n = 0;
        const int bad = check_table(c, mgr, &n);
        G.checked = 1, G.refused = bad || n < CHECK_FLAGS / 2;
        if (G.refused)
            hos_log("coop: sync: the game's flags do not read as this build expects (%d of %d checked differ from the game's getter): "
                    "progress sync shares nothing", bad, n);
        else hos_log("coop: sync: the flag table reads as the game's getter does (%d flags checked)", n);
    }
    if (G.refused) {
        ReleaseSRWLockExclusive(&G.lock);
        return;
    }
    const uint64_t now = coop_now_ms();
    if (G.eng) coop_sync_schema(G.eng, G.schema), coop_sync_rebind(G.eng);
    for (int i = 0; i < G.nflags; i++) {
        GFlag *f = &G.flags[i];
        f->last = raw_of(c, f);
        if (G.eng && !coop_cat_is_pool(f->cat) && reg_of(f, f->last)) coop_sync_local(G.eng, f->cat, f->hash, reg_of(f, f->last), now);
    }
    ReleaseSRWLockExclusive(&G.lock);
    G.bound = 1, G.frames = 0, G.slice = 0;
    say_counts("playing a save");
}

/* Link stands in the world: the Player actor, [[PLAYER_PTR] + 0x480], exists (its name, [+0x20], reads "Player") away from
   the origin (its position in metres at +0x31c). There is none on the title screen at the start, so this holds sync back
   while the first load builds the world; after a return to the title screen the actor stays (title_starts marks that). */
static int player_in_world(Ctx *c) {
    const uint64_t m = recomp_load64(c, PLAYER_PTR), a = rd_ok(m + 0x480, 8) ? recomp_load64(c, m + 0x480) : 0;
    const uint64_t name = rd_ok(a + 0x20, 8) ? recomp_load64(c, a + 0x20) : 0;
    if (!rd_ok(name, 7) || memcmp(GP(name), "Player", 7) || !rd_ok(a + 0x31c, 12)) return 0;
    float p[3];
    memcpy(p, GP(a + 0x31c), sizeof p);
    return isfinite(p[0]) && isfinite(p[2]) && fabsf(p[0]) + fabsf(p[2]) >= 1.0f;
}

/* The title screen's event flow is opened each time the title screen starts: at boot and after the pause menu's Title
   Screen. From then until a save is loaded the flags are the title's, and the play clock runs on from the save's value.
   Counted whether co-op is on or not: sync switched on later must know the title screen came back before it. */
static volatile int title_starts;
void hos_oot_coop_rom_open(const char *guest_path) {
    if (strstr(guest_path, "/Event/EventFlow/TitleEvent.bfevfl")) __atomic_add_fetch(&title_starts, 1, __ATOMIC_RELAXED);
}

/* The game is being played: a save was loaded since the title screen, Link stands in the world, the clock runs, no save
   is loading, no scene is changing. Notes a save change (unbind). */
static int gate(Ctx *c, uint64_t mgr) {
    if (mgr != G.mgr || !G.clock_entry) {   /* the addresses of the few flags read every frame */
        G.clock_entry = entry_of(c, mgr, C_UINT, INT_SIZE, F_PLAY_CLOCK);
        G.loading_entry = entry_of(c, mgr, C_BOOL, BOOL_SIZE, F_LOADING);
        for (int i = 0; i < 4; i++) G.transition_entry[i] = entry_of(c, mgr, C_BOOL, BOOL_SIZE, transition_flags[i]);
        if (!G.clock_entry) return 0;
    }
    const uint64_t now = coop_now_ms();
    const int titles = __atomic_load_n(&title_starts, __ATOMIC_RELAXED);
    if (titles != G.titles) {
        G.titles = titles;
        unbind("the title screen came back");
        G.loaded = 0;
    }
    const uint32_t clock = (uint32_t)recomp_load32(c, G.clock_entry + I_CURRENT);
    const int loading = G.loading_entry && recomp_load8(c, G.loading_entry + B_CURRENT);
    unsigned moving = 0;
    for (int i = 0; i < 4; i++) moving |= (unsigned)(G.transition_entry[i] && recomp_load8(c, G.transition_entry[i] + B_CURRENT)) << i;
    if (clock != G.clock) {
        /* the clock counts seconds one by one, on the title screen too; any other change is a save being loaded (a load, a
           new game, the same save again from where it was saved), which also sets the flags: play starts at the next tick
           after one */
        const int tick = clock > G.clock && clock - G.clock <= 5;
        if (!tick) unbind("another save was loaded"), G.loaded = 1;
        G.clock = clock;
        G.clock_at = tick ? now : 0;
    }
    if (loading) unbind("a save is loading"), G.loaded = 1;
    if (!G.looked) {
        /* the first look: a clock that appears reads as a load. That is right when sync is on from the game's start. Sync
           switched on later, after the title screen came back, may be looking at the title screen, where Link's actor
           stays and the clock runs: whether a save is played is not known until one is loaded */
        G.looked = 1;
        if (titles > 1 && !loading) {
            G.loaded = 0;
            hos_log("coop: sync: co-op was switched on after the title screen came back: progress is shared once a save is loaded");
        }
    }
    const int player = player_in_world(c);
    if (player != G.player && (TRACE_ON || G.probe)) hos_log("coop: sync: Link %s the world", player ? "stands in" : "is not in");
    G.player = player;
    const int open = G.loaded && player && !loading && !moving && G.clock_at && now - G.clock_at < CLOCK_MS;
    if (open != G.gate && (TRACE_ON || G.probe))
        hos_log("coop: sync: game %s (play clock %u, loading %d, scene change %#x)", open ? "running" : "paused", clock, loading, moving);
    G.gate = open;
    return open;
}

static void run_test_aid(Ctx *c, uint64_t mgr) {
    for (int i = 0; i < G.ntest; i++) {
        TestSet *t = &G.test[i];
        if (t->done == 1 && G.frames >= t->frame + 30) {   /* half a second later: what the game shows now */
            t->done = 2;
            const uint64_t e = entry_of(c, mgr, C_BOOL, BOOL_SIZE, t->hash);
            int got = -1;
            const int found = e ? game_get_bool(c, mgr, t->hash, &got) : 0;
            const uint64_t ie = e ? 0 : entry_of(c, mgr, C_INT, INT_SIZE, t->hash);
            if (e) hos_log("coop: test: Bool %08x reads %d in the table and %d (found %d) by the game's getter", t->hash,
                           (int)recomp_load8(c, e + B_CURRENT), got, found);
            else if (ie) hos_log("coop: test: Int %08x reads %d in the table", t->hash, (int)recomp_load32(c, ie + I_CURRENT));
        }
        if (t->done || G.frames < t->frame) continue;
        t->done = 1;
        const int is_int = entry_of(c, mgr, C_BOOL, BOOL_SIZE, t->hash) ? 0 : entry_of(c, mgr, C_INT, INT_SIZE, t->hash) ? 1 : -1;
        if (is_int < 0) {
            hos_log("coop: test: no Bool or Int flag %08x", t->hash);
            t->done = 2;
            continue;
        }
        set_flag(c, mgr, is_int, t->hash, t->value);
        hos_log("coop: test: set %s %08x = %d at frame %u", is_int ? "Int" : "Bool", t->hash, t->value, G.frames);
    }
}

static void apply(Ctx *c, uint64_t mgr) {
    CoopApply a[APPLY_PER_FRAME];
    if (!G.eng || !__atomic_load_n(&G.open, __ATOMIC_ACQUIRE)) return;   /* not accepted, or no copy of the save yet: values wait */
    if (!TryAcquireSRWLockExclusive(&G.lock)) return;   /* the co-op thread has the engine: nothing is applied this frame */
    const int n = coop_sync_take(G.eng, a, APPLY_PER_FRAME);
    ReleaseSRWLockExclusive(&G.lock);
    for (int i = 0; i < n; i++) {
        const GFlag *f = flag_by_hash(a[i].hash);
        int32_t v = 0;
        if (f && a[i].pool && int_pending(c, mgr, f->hash)) {   /* the game changed it this frame: the next one */
            AcquireSRWLockExclusive(&G.lock);
            coop_sync_untake(G.eng, &a[i]);
            ReleaseSRWLockExclusive(&G.lock);
            continue;
        }
        if (!apply_value(f, &a[i], f ? raw_of(c, f) : 0, &v)) {
            if (TRACE_ON) hos_log("coop: sync: not applied %s %08x = %d", coop_cat_name(a[i].cat), a[i].hash, a[i].value);
            continue;
        }
        set_flag(c, mgr, f->is_int, f->hash, v);
        if (a[i].pool) {
            AcquireSRWLockExclusive(&G.lock);
            coop_sync_pool_wrote(G.eng, f->hash, v);
            ReleaseSRWLockExclusive(&G.lock);
        }
        G.applied[f->cat]++, G.applied_total++;
        if (TRACE_ON) hos_log("coop: sync: applied %s %08x = %d", coop_cat_name(f->cat), f->hash, v);
    }
    const uint64_t now = coop_now_ms();
    if (G.applied_total && now - G.applied_log_at >= 5000) {
        char text[200];
        size_t at = 0;
        for (int k = 0; k < COOP_CAT_N; k++)
            if (G.applied[k] && at < sizeof text)
                at += (size_t)snprintf(text + at, sizeof text - at, "%s%s %u", at ? ", " : "", coop_cat_name(k), G.applied[k]);
        hos_log("coop: sync: took %u values from the room (%s)", G.applied_total, text);
        memset(G.applied, 0, sizeof G.applied);
        G.applied_total = 0;
        G.applied_log_at = now;
    }
}

static void scan(Ctx *c, uint64_t mgr, uint64_t now) {
    const int nflat = G.nflags - G.npools;
    const int from = nflat * G.slice / SLICES, to = nflat * (G.slice + 1) / SLICES;
    if (!G.eng || !TryAcquireSRWLockExclusive(&G.lock)) return;   /* the co-op thread has the engine: this frame reads nothing */
    G.slice = (G.slice + 1) % SLICES;
    for (int i = from; i < to; i++) {
        GFlag *f = &G.flags[i];
        const int32_t raw = raw_of(c, f);
        if (raw == f->last) continue;
        f->last = raw;
        coop_sync_local(G.eng, f->cat, f->hash, reg_of(f, raw), now);
        if (TRACE_ON) {   /* (what the game shows now, a value of its own or one the room gave it, and its getter's reading) */
            int got = -1;
            const int found = f->is_int ? 0 : game_get_bool(c, mgr, f->hash, &got);
            hos_log("coop: sync: the game shows %s %08x = %d%s", coop_cat_name(f->cat), f->hash, (int)raw,
                    f->is_int ? "" : found && got == raw ? " (its getter agrees)" : " (its getter DIFFERS)");
        }
    }
    for (int i = nflat; i < G.nflags; i++) {
        GFlag *f = &G.flags[i];
        f->last = raw_of(c, f);
        const int32_t cap = f->cap_addr ? (int32_t)recomp_load32(c, f->cap_addr + I_CURRENT) : -1;
        coop_sync_pool_local(G.eng, f->cat, f->hash, f->last, cap > 0 ? cap : -1, now);
    }
    ReleaseSRWLockExclusive(&G.lock);
}

/* HOS_COOP_PROBE: what this file reads of the game, once a save is played (the containers, their tables, the entries'
   own hashes, the structs it uses, the classification, the scene-change flags and the songs against the game's getter,
   and the converted address of the manager's holder from the other port's layout, 0x84ea42a8, against GameDataMgr's).
   Nothing is written. */
static void probe(Ctx *c, uint64_t mgr) {
    const uint64_t holder = recomp_load64(c, GAMEDATA_SLOT), other = recomp_load64(c, 0x84ea42a8ULL);
    hos_log("coop: probe: GameDataMgr: [%#llx] = %#llx, [%#llx] = %#llx (the manager); [0x84ea42a8] = %#llx (%s)",
            (unsigned long long)GAMEDATA_SLOT, (unsigned long long)holder, (unsigned long long)holder, (unsigned long long)mgr,
            (unsigned long long)other, other == mgr ? "the same manager" : other == holder ? "the holder" : "another value");
    static const struct { unsigned base, stride; const char *name; } kinds[] = {
        {C_BOOL, BOOL_SIZE, "Bool"}, {C_INT, INT_SIZE, "Int"}, {C_UINT, INT_SIZE, "UInt"}};
    for (int t = 0; t < 3; t++) {
        const uint64_t k = mgr + kinds[t].base, table = recomp_load64(c, k + CN_TABLE), entries = recomp_load64(c, k + CN_ENTRIES);
        const uint32_t cap = (uint32_t)recomp_load32(c, k + CN_CAP), count = (uint32_t)recomp_load32(c, k + CN_COUNT);
        uint32_t used = 0, out_of_range = 0, own_hash = 0, own_hash8 = 0, dup = 0;
        if (cap && cap <= 4000000 && rd_ok(table, 8ull * cap) && rd_ok(entries, (uint64_t)count * kinds[t].stride)) {
            for (uint32_t i = 0; i < cap; i++) {
                const uint32_t h = (uint32_t)recomp_load32(c, table + 8ull * i), idx = (uint32_t)recomp_load32(c, table + 8ull * i + 4);
                if (!h) continue;
                used++;
                if (idx >= count) { out_of_range++; continue; }
                const uint64_t e = entries + (uint64_t)idx * kinds[t].stride;
                own_hash += (uint32_t)recomp_load32(c, e) == h, own_hash8 += (uint32_t)recomp_load32(c, e + 4) == h;
                dup += entry_of(c, mgr, kinds[t].base, kinds[t].stride, h) != e;
            }
        }
        hos_log("coop: probe: %s container at mgr+%#x: %u entries of %#x bytes at %#llx, table %#llx of %u slots, %u used, %u past the "
                "entries, %u found elsewhere by a lookup, entry+0 holds its hash in %u, entry+4 in %u; ring %u of %u",
                kinds[t].name, kinds[t].base, count, kinds[t].stride, (unsigned long long)entries, (unsigned long long)table, cap, used,
                out_of_range, dup, own_hash, own_hash8, (uint32_t)recomp_load32(c, k + CN_RING_CTR) & 0xFFFFF,
                (uint32_t)recomp_load32(c, k + CN_RING_CAP));
        if (count && rd_ok(entries, 2ull * kinds[t].stride)) {
            char hex[2 * 0x28 * 2 + 4];
            size_t at = 0;
            for (unsigned b = 0; b < 2 * kinds[t].stride; b++)
                at += (size_t)snprintf(hex + at, sizeof hex - at, "%02x%s", (unsigned)recomp_load8(c, entries + b), b + 1 == kinds[t].stride ? " | " : "");
            hos_log("coop: probe: %s entries 0 and 1: %s", kinds[t].name, hex);
        }
    }
    const uint32_t scount = (uint32_t)recomp_load32(c, mgr + ST_COUNT), scap = (uint32_t)recomp_load32(c, mgr + ST_CAP);
    hos_log("coop: probe: structs: %u in a table of %u slots; pouch %s, first-get %s, dungeon %s, capacity %s; wallet capacity entry %#llx",
            scount, scap, struct_of(c, mgr, S_POUCH) ? "found" : "missing", struct_of(c, mgr, S_FIRST_GET) ? "found" : "missing",
            struct_of(c, mgr, S_DUNGEON) ? "found" : "missing", struct_of(c, mgr, S_CAPACITY) ? "found" : "missing",
            (unsigned long long)member_entry(c, mgr, S_CAPACITY, CAP_RUPEES));
    say_counts("probe: the classification");
    char line[1024];
    size_t at = 0;
    static const uint32_t named[] = {F_LOADING, 0xcea220d3u /* Sequence_IsChildhood */, 0xef2b60c2u /* Sequence_IsLoadingBanc */};
    uint32_t list[sizeof named / sizeof *named + sizeof transition_flags / sizeof *transition_flags + sizeof song_flags / sizeof *song_flags];
    size_t n = 0;
    for (size_t i = 0; i < sizeof named / sizeof *named; i++) list[n++] = named[i];
    for (size_t i = 0; i < sizeof transition_flags / sizeof *transition_flags; i++) list[n++] = transition_flags[i];
    for (size_t i = 0; i < sizeof song_flags / sizeof *song_flags; i++) list[n++] = song_flags[i];
    int same = 0, differ = 0;
    for (size_t i = 0; i < n; i++) {
        const uint64_t e = entry_of(c, mgr, C_BOOL, BOOL_SIZE, list[i]);
        int got = -1;
        const int found = game_get_bool(c, mgr, list[i], &got);
        const int mine = e ? (int)(recomp_load8(c, e + B_CURRENT) & 1) : -1;
        same += found && e && mine == got, differ += !(found && e && mine == got) && (found || e);
        if (at < sizeof line - 40) at += (size_t)snprintf(line + at, sizeof line - at, " %08x=%d/%d%s", list[i], mine, found ? got : -1,
                                                          e && found ? (mine == got ? "" : "!") : "?");
    }
    hos_log("coop: probe: known flags (table/getter): %d agree, %d differ:%s", same, differ, line);
    int checked = 0;
    const int bad = check_table(c, mgr, &checked);
    hos_log("coop: probe: the check before sharing: %d of %d differ; Link %s the world; play clock %u (entry %#llx)", bad, checked,
            player_in_world(c) ? "stands in" : "is not in", G.clock_entry ? (uint32_t)recomp_load32(c, G.clock_entry + I_CURRENT) : 0,
            (unsigned long long)G.clock_entry);
}

/* The switches, once, at the first present, and the session's hooks: registered whether co-op is on or not, since the
   Co-op tab may switch it on later (they are called only while the session's thread runs). */
static int start(void) {
    const char *on = getenv("HOS_COOP"), *pr = getenv("HOS_COOP_PROBE");
    const int want = on ? !env_value_off(on) : hos_ini_option("coop", 0) != 0;   /* (as host/coop.c coop_init reads it) */
    G.probe = pr && !env_value_off(pr);
    G.trace = env_on("HOS_COOP_TRACE");
    read_test_aid();
    if (G.probe) hos_log("coop: probe: HOS_COOP_PROBE: the flag table is logged once a save is played%s", want ? "" : "; co-op is off");
    __atomic_store_n(&G.state, G.probe ? ST_ON : ST_OFF, __ATOMIC_RELEASE);   /* (before the hooks: one may switch sync on at once) */
    coop_on_message(COOP_K_DIGEST, on_message, (void *)(uintptr_t)COOP_K_DIGEST);
    coop_on_message(COOP_K_SECTION, on_message, (void *)(uintptr_t)COOP_K_SECTION);
    coop_on_view(on_view, NULL);
    coop_on_tick(on_tick, NULL);
    coop_on_remove(on_remove, NULL);
    if (want) sync_on();
    return __atomic_load_n(&G.state, __ATOMIC_ACQUIRE);
}

void hos_oot_coop_present(Ctx *c) {
    int s = __atomic_load_n(&G.state, __ATOMIC_ACQUIRE);
    if (s == ST_OFF) return;
    if (s == ST_UNSEEN) {
        int unseen = ST_UNSEEN;
        if (!__atomic_compare_exchange_n(&G.state, &unseen, ST_STARTING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
        s = start();
        if (s == ST_OFF) return;
    }
    if (s != ST_ON) return;   /* (another thread is starting) */
    const int on = __atomic_load_n(&G.on, __ATOMIC_ACQUIRE);
    if (on && !G.eng && !ensure_engine()) return;
    const uint64_t holder = recomp_load64(c, GAMEDATA_SLOT), mgr = rd_ok(holder, 8) ? recomp_load64(c, holder) : 0;
    if (!rd_ok(mgr, MGR_SIZE)) {
        unbind("the game's flags are gone");
        return;
    }
    if (G.bound && (mgr != G.mgr || recomp_load64(c, mgr + C_BOOL + CN_ENTRIES) != G.bool_entries ||
                    recomp_load64(c, mgr + C_INT + CN_ENTRIES) != G.int_entries))
        unbind("the game made its flags anew");
    if (!gate(c, mgr)) return;
    if (!G.bound) bind_tables(c, mgr);
    if (!G.bound) return;
    if (G.probe && !G.probed) G.probed = 1, probe(c, mgr);
    G.frames++;
    run_test_aid(c, mgr);
    if (!on) return;
    scan(c, mgr, coop_now_ms());
    apply(c, mgr);
}
