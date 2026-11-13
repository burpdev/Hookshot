/* Co-op for Ocarina of Time: the presence layer of a room.
 *
 * Two to eight players, each in their own copy of the game, sharing one adventure: what one finds, all hold; what one
 * has done, is done for all. The F1 menu's Co-op tab turns it on with two rows: Host, or Join at an address.
 * docs/coop.md is the players' page.
 *
 * ONE ROOM, ONE WAY INTO IT (host/coop.c). The room is the host's listening socket: the game that hosts holds it, the
 * others reach it at the host's address and its port (43400 unless the tab says otherwise), and it ends when the host's
 * game closes. That session is Hookshot's own — its wire, its members and its round trip are in host/coop.h,
 * host/coop_net.c and host/coop_proto.h; what a room shares and what stays each player's own is its rules engine
 * (host/coop_state.c, host/coop_oot.c) under the tab's Room rules page. This file adds no link of its own, on Windows or
 * on Linux: what it says goes out as the room's messages (coop_send, COOP_K_LINE) and what the others say comes to it on
 * the room's thread.
 *
 * WHAT THIS LAYER SHARES
 *   where      Thirty times a second each player says which map they are on, where they stand, which way they face and
 *              what Link plays; as his game puts equipment in his hands or hangs it on him, he says that too. The menu
 *              and the corner of the screen show it, and each other player is drawn there as a Link of their age,
 *              holding and wearing what their Link does (co_bodies_step). A Go to player row travels to one of them.
 *              A game that has stopped updating Link — the pause menu, the map, the save — keeps saying once a
 *              second that it is where it was and busy, so a pause is not read as a player leaving.
 *   enemies    An enemy's life is one for all, and it stands in one place for all: the game of the player nearest to it
 *              leads it, the others' copies follow ("one enemy for everyone", further down).
 *   PvP        A hit that reaches another player takes hearts from them, between players who both have the row on.
 *   hearts     Each heart container, each Piece of Heart and each Gold Skulltula token is one for one: what one finds,
 *              all hold, once. Those three are what the room's rules leave to this layer.
 * Everything else of what a room shares — the game's save flags, items, songs, quest items, small keys, rupees and
 * ammunition — is the room's rules engine's, with its pools, its acceptance and its signed rules. A room does not have two
 * of them, and this file is not one: what it used to keep in step itself (nx2/coop_flags.py's shared flags, the placed
 * objects' flags, each dungeon's small keys) went to the rules, where the Room rules page says whether it is shared at
 * all, shared per player, or drawn from one pool.
 * Nothing is changed in a game while an event plays in it or a map loads (co_game_busy): it waits.
 *
 * THE LINES, carried as the room's messages of kind COOP_K_LINE. The room puts every message under the number of the
 * member that sent it (host/coop.c stamps it), so a line here never names its sender; the members are numbered 1..8 and
 * the host is always 1, which is the number a hit names (D, U, O). A player who joins is caught up by the rules engine;
 * this layer says only its hearts (M).
 *   P <map> <x> <y> <z> <yaw> <child> <busy> <pvp>   where I am, and what my game is doing
 *   I <name> <count> [q]     I found a heart container, a Piece of Heart or a token (q: said while a player catches up)
 *   M <quarters>             (catching up) my hearts, in quarters: containers and pieces
 *   D <player> <quarters>    (PvP) my swing reached that player: so many quarter hearts
 *   O <player>               (PvP) that player's hearts ran out
 *   U <enemy> <x y z> <life> my hit took this much of that enemy's life (its name's hash, where it was made)
 *   Q <enemy> <x y z> ...    I lead that enemy: where it stands and what it plays (co_enemy_lead_step)
 *   T <slot> <command>       what my Link plays in an animation slot ("-": nothing)
 *   Y <words> <base64>       my Link's animation inputs: the words that changed
 *   E <slot> <name> <number> my Link's game put that equipment in that slot: what he holds (said from the game's own
 *                            equipment user call, hooked in nx2/oot_patches.py)
 *   S <kind> <state>         my Link's equipment state changed (kind 1 and kind 2, the game's own two)
 *   G, A, Z                  test runs only (HOS_COOP_TEST=1): travel, an animation, an actor
 *
 * THREADS: the room's own thread hears (co_on_line, co_on_room); the game's side speaks (oot_coop_frame every frame
 * shown, co_tick from Link's own update while a save is played). Between them two queues under co.lock, and co.game keeps
 * the two callers of the game's own data apart. The name, the host's address and the port are the Co-op tab's rows and
 * hookshot_options.ini's (coop_name=, coop_server=, coop_port=), which host/coop.c reads at the start; Host and Join are
 * the tab's buttons. A changed game — a randomizer's seed — is not this layer's business: the room's rules are signed by
 * the host's key, and everyone in a room plays the same Hookshot build.
 */
#include "hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   /* (the queues' locks and the counters; on Linux, host/posix/windows.h: the same calls) */
#include "coop.h"      /* (the room: host/coop.c. Its transport, host/coop_net.c, is Windows' and Linux's) */
#include "coop_proto.h"
#include "hos_paths.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../gpu/vk/oot_cheat_items.h"
#include "../gpu/vk/oot_coop_opts.h"   /* (HOS_OPT_COOP_*: the Co-op tab's rows) */

#define CO_VERSION 1
#define CO_PEERS 8
#define CO_LINE 1024

int rx_option(int id, int otherwise);
void rx_option_set(int id, int value);

/* ── the pouch, by an item's name ────────────────────────────────────────────
   How many of an item the pouch holds: the game's own count (f_805b63e0 on the manager at 0x84e98e28). -1: not ready. */
static int co_pouch_count(Ctx *c, const char *name) {
    if (strlen(name) >= 0x60) return -1;
    const uint64_t manager = recomp_load64(c, 0x84e98e28ULL);
    A64Fn count = a64_lookup(0x805b63e0ULL);
    if (!manager || !count) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memcpy(A64_PTR(frame + 0x10), name, strlen(name) + 1);
    recomp_store64_sp(c, frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = manager, c->x[1] = frame, c->x[2] = 0, c->x[3] = 0;
    count(c);
    const int n = (int)(int32_t)c->x[0];
    *c = saved;
    return n;
}

/* An item added to the pouch or taken from it, the way the Inventory page does it (hos_main.c oot_pouch_change: the
   game's own pouch edit, f_828d4a70, with an event's arguments; above 0 adds, below takes away). The edit's status,
   -1 when the game is not ready for it. */
static int co_pouch_change(Ctx *c, const char *name, int delta) {
    const uint64_t pouch_singleton = recomp_load64(c, 0x84c9e0d0ULL);
    const uint64_t state_singleton = recomp_load64(c, 0x84c9e2a8ULL);
    const uint64_t actor_db = recomp_load64(c, 0x84eb6888ULL);
    const uint64_t actor_info = recomp_load64(c, 0x84e8c060ULL);
    A64Fn edit = a64_lookup(0x828d4a70ULL);
    if (!edit || !pouch_singleton || !recomp_load64(c, pouch_singleton) ||
        !state_singleton || !recomp_load64(c, state_singleton) || !actor_db || !actor_info || strlen(name) >= 0x60) return -1;
    const Ctx saved = *c;
    const uint64_t event = (c->x[31] - 0xe0) & ~0xfULL;
    memset(A64_PTR(event), 0, 0xe0);
    memcpy(A64_PTR(event + 0x80), name, strlen(name) + 1);
    recomp_store64_sp(c, event + 0x20, event + 0x60);
    recomp_store64_sp(c, event + 0x30, event + 0x70);
    recomp_store64_sp(c, event + 0x38, event + 0x68);
    recomp_store64_sp(c, event + 0x48, event + 0x74);
    recomp_store64_sp(c, event + 0x50, event + 0x74);
    recomp_store64_sp(c, event + 0x60, event + 0x80);
    recomp_store64_sp(c, event + 0x68, 0x83f0a6d8ULL);
    recomp_store32_sp(c, event + 0x70, (uint32_t)delta);
    c->x[31] = event;
    c->x[0] = event;
    edit(c);
    const int result = (int)(recomp_load32(c, event + 0x18) >> 30);
    *c = saved;
    return result;
}
static int co_pouch_give_many(Ctx *c, const char *name, int many) { return co_pouch_change(c, name, many < 1 ? 1 : many); }
static int co_pouch_give(Ctx *c, const char *name) { return co_pouch_give_many(c, name, 1); }
static int co_pouch_take(Ctx *c, const char *name, int many) { return many > 0 ? co_pouch_change(c, name, -many) : -1; }

/* ── the lasting items ───────────────────────────────────────────────────── */
#define CO_ITEM_LABEL(label, name, kind) label,
#define CO_ITEM_NAME(label, name, kind) name,
#define CO_ITEM_KIND(label, name, kind) kind,
#define CO_NO_SECTION(name)
static const char *const co_item_label[] = {OOT_CHEAT_ITEMS(CO_NO_SECTION, CO_ITEM_LABEL)};
static const char *const co_item_name[] = {OOT_CHEAT_ITEMS(CO_NO_SECTION, CO_ITEM_NAME)};
static const unsigned char co_item_kind[] = {OOT_CHEAT_ITEMS(CO_NO_SECTION, CO_ITEM_KIND)};
#define CO_ITEMS ((int)(sizeof co_item_name / sizeof co_item_name[0]))

/* A pickup's name to its place in the list. A chest hands over a stand-in (Item_Sword_Kokiri)
   for what the inventory keeps under another name (Weapon_Sword_Kokiri, Armor_Adult_HoverBoot,
   Armor_Child_Mask_Bunny for Item_Mask_Bunny): those count as the kept item. -1: not a lasting item. */
static int co_item_index(const char *name) {
    for (int i = 0; i < CO_ITEMS; i++)
        if (!strcmp(name, co_item_name[i])) return i;
    if (strncmp(name, "Item_", 5)) return -1;
    static const char *const kept[] = {"Weapon_", "Armor_Adult_", "Armor_Child_"};
    char twin[0x70];
    for (unsigned k = 0; k < 3; k++) {
        snprintf(twin, sizeof twin, "%s%s", kept[k], name + 5);
        for (int i = 0; i < CO_ITEMS; i++)
            if (!strcmp(twin, co_item_name[i])) return i;
    }
    return -1;
}

/* What a pickup is to the others, and the name it goes by between games:
     0  each player's own (a heart picked up, a fairy, what a bottle holds, a dungeon's own items: those are save values)
     1  lasting: held or not, given once
     2  counted and shared: given one for one (heart containers and pieces, bottles, tokens, rupees and ammunition:
        what one picks up, all get) */
static int co_item_class(const char *name, char canon[0x60]) {
    canon[0] = 0;
    if (strlen(name) >= 0x60) return 0;
    if (strstr(name, "Heart_Piece")) return snprintf(canon, 0x60, "Obj_Heart_Piece"), 2;   /* (the digging hole's, the mini-game's: one and the same) */
    if (!strcmp(name, "Obj_Heart_Container")) return snprintf(canon, 0x60, "%s", name), 2;
    /* (What lies on the ground, by the Actor Ledger's names: rupees but not a puzzle's silver ones, and ammunition.
       A heart, a fairy, a magic jar fill up the one who took them: those stay one's own.) */
    if (!strncmp(name, "Obj_Rupee_", 10) && !strstr(name, "Silver")) return snprintf(canon, 0x60, "%s", name), 2;
    if (!strcmp(name, "Obj_DekuStick") || !strcmp(name, "Obj_DekuNut") || !strcmp(name, "Obj_DekuSeed") || !strcmp(name, "Obj_Bomb") ||
        !strcmp(name, "Obj_Bombchu") || !strncmp(name, "Obj_ArrowBundle_", 16)) return snprintf(canon, 0x60, "%s", name), 2;
    const int i = co_item_index(name);
    if (i >= 0) {
        snprintf(canon, 0x60, "%s", co_item_name[i]);
        return co_item_kind[i] == 0 ? 1 : 2;                /* (the counted ones: seeds, nuts, sticks, bombs, arrows, beans, bottles, tokens) */
    }
    if (!strncmp(name, "Item_Dungeon_", 13) || !strncmp(name, "Item_Bottle_", 12) || !strncmp(name, "Item_GetDemo", 12)) return 0;
    if (strncmp(name, "Item_", 5) && strncmp(name, "Weapon_", 7) && strncmp(name, "Armor_", 6)) return 0;
    snprintf(canon, 0x60, "%s", name);   /* (an item the Inventory page does not list: a trade item, a nut or stick bag) */
    return 1;
}

/* The name the game takes an item by, and the other spellings it may hold it under. */
static void co_item_spellings(int i, char give[0x70], char other[2][0x70]) {
    const char *name = co_item_name[i];
    other[0][0] = other[1][0] = 0;
    snprintf(give, 0x70, "%s", name);
    if (!strncmp(name, "Armor_Child_Mask_", 17)) {            /* (hos_main.c oot_mask_alias: added as Item_Mask_X) */
        snprintf(give, 0x70, "Item_Mask_%s", name + 17);
        snprintf(other[0], 0x70, "%s", name);
    } else if (!strncmp(name, "Weapon_", 7)) snprintf(other[0], 0x70, "Item_%s", name + 7);
    else if (!strncmp(name, "Armor_Adult_", 12)) snprintf(other[0], 0x70, "Item_%s", name + 12);
    else if (!strncmp(name, "Item_Gauntlet_Goron", 19)) snprintf(other[0], 0x70, "Armor_Child_Gauntlet_Goron");
}

/* ── shared state ────────────────────────────────────────────────────────── */
typedef struct {
    int used, id;
    char name[24], seed[24], map[64];
    float pos[3], yaw;
    float was[3], was_yaw;              /* the place before this one, and when each came: the body moves between them */
    int64_t seen_before;
    int child;
    int busy;                           /* in a cutscene, a conversation or a loading: not to be hit (PvP) */
    int pvp;                            /* they have PvP on */
    int64_t seen;                       /* when a position last came */
} CoPeer;

typedef struct { int from; char text[CO_LINE]; } CoEvent;

static struct {
    CRITICAL_SECTION lock;
    CRITICAL_SECTION game;              /* the game's side: oot_coop_frame and co_tick, one at a time */
    int ready;
    volatile LONG state;                /* the room's (host/coop.h): 0 off, 1 out of it, 2 reaching it, 3 in it,
                                           4 the host went silent: reaching it again */
    volatile LONG hosting;              /* this game holds the room */
    volatile LONG my_id, links;         /* my number in it, the host's being 1; how many members there are */
    char name[24], addr[128];           /* mine, and where the room is reached (for a note: not typed here) */
    CoPeer peer[CO_PEERS];
    CoEvent *in; size_t in_len, in_cap; /* what came (the room's thread -> the game's) */
    /* me, from oot_coop_player */
    volatile int64_t player_seen;
    volatile uint64_t my_user;              /* my Link's equipment user: the equipment hooks compare against it */
    float pos[3], yaw;
} co;

static CoPeer *co_peer(int id, int make);
static void co_event(int from, const char *text);
static void co_take(int from, const char *text);
static void co_note(const char *fmt, ...);
static void co_send(const char *fmt, ...);
static int co_level;                    /* say everything again: someone joined or left */

/* In a room, or on the way into one or back to it */
static int co_room(void) {
    const long s = (long)co.state;
    return s == COOP_CONNECTING || s == COOP_JOINED || s == COOP_RECONNECTING;
}

static char co_map[64];                 /* the map I am on (co_tick) */
static int co_playing;
#define CO_AS_SLOTS 24               /* (the animation: slots read of an actor, and a command name's room; adult Link's
 *              controller carries more than child's, and the upper-body slots — the arms, a sword — sit past the twelve a
 *              child uses: an animation said in a slot this does not read never reaches the others' body) */
#define CO_AS_NAME 48
static volatile LONG co_test_play;      /* (test runs) play co_test_play_name in slot co_test_play_slot on the bodies */
static char co_test_play_name[CO_AS_NAME];
static int co_test_play_slot;
static int64_t co_flinch_until[CO_PEERS];   /* (PvP) a body I just hit plays the hit until then, before its player's own game says so */
static int co_flinch_hard[CO_PEERS];
static char co_test_go[64];          /* (test runs: a map to travel to) */
static void co_enemy_hit(uint32_t hash, const float at[3], uint32_t taken);   /* (one enemy for everyone, below) */
static void co_enemy_led(int from, const char *text);
static int co_actor_move(Ctx *c, uint64_t actor, const float at[12]);
static volatile LONG co_busy_now;      /* (co_tick) an event is playing or a map is loading in my game */
static volatile LONG co_my_child = 1;  /* (co_tick) my Link's age, said with the place; held across a shut flag table */
static volatile int64_t co_place_sent; /* (co_tick and the frame) the last time I said where I am */
static volatile LONG co_hurt;           /* (PvP) quarter hearts to come off mine (the game side takes it) */
static volatile LONG co_hurt_from;      /* (PvP) the number of the player whose swing it was */
static char co_hurt_by[24];
static volatile LONG co_test_spawn;     /* (test runs) make co_test_name beside Link */
static char co_test_name[0x60];
static int co_test_option;
static _Thread_local int co_applying;   /* an item this side is handing over: not a pickup of mine */

static int co_ok(uint64_t addr, uint64_t len) {
    if (!len || addr + len < addr) return 0;
    if (addr >= HOS_SYSTEM_BASE && addr + len <= HOS_SYSTEM_END) return 1;
    if (addr >= hos_image_base && addr + len <= hos_image_end) return 1;
    if (addr >= HOS_HEAP_BASE && addr + len <= HOS_HEAP_BASE + hos_heap_used) return 1;
    /* (The game has memory above the heap too, hos_region_alloc's: an animation command's name is there. The system region,
       the image and the heap are not all: with only those no command name is read at all, and every player
       stands in one pose. So anywhere else in the guest window: is it memory, as Windows has it?) */
    if (addr < hos_image_base || !hos_guest_range(addr, len)) return 0;
    static _Thread_local uint64_t known_from, known_to;   /* (the stretch last asked about: the next address is most often in it) */
    static _Thread_local int64_t known_at;
    const int64_t now = hos_now_ns();
    if (addr >= known_from && addr + len <= known_to && now - known_at < 50000000LL) return 1;
    MEMORY_BASIC_INFORMATION info;
    for (uint64_t at = addr; at < addr + len;) {
        if (!VirtualQuery(GP(at), &info, sizeof info) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(info.Protect & 0xff)) return 0;
        const uint64_t done = (uint64_t)((const uint8_t *)info.BaseAddress + info.RegionSize - (const uint8_t *)GP(at));
        if (at == addr) known_from = addr & ~0xfffULL, known_to = addr + done, known_at = now;
        at += done ? done : 0x1000;
    }
    return 1;
}



/* Letters, digits and a few marks only: a name or an address travels in a line of words. */
static void co_clean(char *out, size_t cap, const char *in, const char *also) {
    size_t n = 0;
    for (; *in && n + 1 < cap; in++) {
        const unsigned char ch = (unsigned char)*in;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch && strchr(also, ch))) out[n++] = (char)ch;
    }
    out[n] = 0;
}

/* ── settings: <game>\coop.txt (name, the host's address); the port is the menu's own row ── */
/* (the word for a changed game that all players must share: none here, the game as it is) */

/* ── to the menu: a text, a letter at a time (the host's calls into it carry numbers only) ── */
static void co_menu_text(int which, const char *text) {
    rx_option_set(HOS_OPT_COOP_CHAR, 0);
    for (; *text; text++) rx_option_set(HOS_OPT_COOP_CHAR, (unsigned char)*text);
    rx_option_set(HOS_OPT_COOP_TEXT, which);   /* (1 a note, 4 the others, a line each) */
}

static void co_note(const char *fmt, ...) {
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    co_menu_text(1, text);
    hos_log("coop: %s", text);
    hos_log_flush();                                   /* (rare, and the first thing asked for when it goes wrong) */
}
/* ── the queues ──────────────────────────────────────────────────────────── */
static void co_send(const char *fmt, ...) {            /* a line for the others (any thread) */
    char line[CO_LINE];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n <= 0 || n >= (int)sizeof line - 1 || !co.ready) return;
    coop_send(COOP_K_LINE, line, (size_t)n);           /* (not in a room: the session says so and sends nothing) */
}

/* ── the room (host/coop.c) ──────────────────────────────────────────────────
   The room is the host's listening socket, and this file never touches a socket: a line goes out as one of the
   room's messages (COOP_K_LINE), what came arrives on the room's thread (co_on_line -> co_take, under co.lock, for
   co_events on the game's thread), and who is in the room, under what name, which number I am and whether I hold it,
   come from the room's view (co_on_room). The room's members are numbered 1..8, the host always 1; a peer here is a
   member by that number, so the numbers a player's hit names (D, U, O) mean the same in every game. */
static void co_on_line(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)user;
    if (!n || n >= CO_LINE) return;
    char text[CO_LINE];
    memcpy(text, bytes, n);
    text[n] = 0;
    co_take(from, text);                               /* (the room's thread: positions, animations, an enemy being led,
                                                        and, for the game's side, what is worth saying: co_event) */
}

static void co_on_room(const CoopView *v, void *user) {
    (void)user;
    EnterCriticalSection(&co.lock);
    InterlockedExchange(&co.state, (long)v->state);
    InterlockedExchange(&co.hosting, v->hosting ? 1 : 0);
    InterlockedExchange(&co.my_id, (long)v->self);
    InterlockedExchange(&co.links, (long)v->n);
    snprintf(co.name, sizeof co.name, "%s", v->name[0] ? v->name : "Player");
    snprintf(co.addr, sizeof co.addr, "%s", v->addr);
    uint8_t here[COOP_MAX_MEMBERS + 1] = {0};
    for (int i = 0; i < v->n && i < COOP_MAX_MEMBERS; i++) {
        const CoopPeer *m = &v->member[i];
        if (m->id == v->self) continue;                /* (the others only: my own place and name are the game's side's,
                                                            and my own id is never marked here, so a peer that was somehow
                                                            made about myself is swept away with the departed) */
        here[m->id] = 1;
        CoPeer *p = co_peer((int)m->id, 1);
        if (!p) continue;
        if (strcmp(p->name, m->name)) {
            co_clean(p->name, sizeof p->name, m->name, "_-");
            co_level = 1;                              /* (a player joined, or took another name: say what I hold) */
        }
    }
    for (int i = 0; i < CO_PEERS; i++) {               /* (a member the room no longer lists: its number is not given to
                                                        someone else while the room lasts, so its body and its animation go) */
        CoPeer *p = &co.peer[i];
        if (!p->used || here[p->id]) continue;
        if (p->name[0]) co_note("%s left", p->name);
        p->used = 0;
    }
    LeaveCriticalSection(&co.lock);
}

static void co_event(int from, const char *text) {     /* (the room's thread) for the game's side */
    EnterCriticalSection(&co.lock);
    if (co.in_len == co.in_cap) {
        const size_t cap = co.in_cap ? co.in_cap * 2 : 64;
        CoEvent *grown = cap <= 65536 ? realloc(co.in, cap * sizeof *grown) : NULL;
        if (grown) co.in = grown, co.in_cap = cap;
    }
    if (co.in_len < co.in_cap) {
        co.in[co.in_len].from = from;
        snprintf(co.in[co.in_len].text, CO_LINE, "%s", text);
        co.in_len++;
    }
    LeaveCriticalSection(&co.lock);
}

#define CO_INPUTS 1024                  /* (how a player moves: see "how another player moves" below) */
static uint32_t co_in_value[CO_PEERS][CO_INPUTS];        /* (under co.lock, by the peer's slot) their Link's animation inputs, word by word */
static uint8_t co_in_have[CO_PEERS][CO_INPUTS];
static uint32_t co_in_words[CO_PEERS];                   /* how many words their graph's inputs are: ours must be as many */
static char co_cmd_name[CO_PEERS][CO_AS_SLOTS][CO_AS_NAME];   /* what their Link plays in each slot ("" nothing) */
static uint8_t co_cmd_have[CO_PEERS][CO_AS_SLOTS];

#define CO_EQ_SLOTS 18          /* (the equipment user's slots: the game's own request numbers them) */
#define CO_EQ_EVENTS 16         /* (hangs kept for a body that is not awake yet) */
/* What another player's Link holds, as their game last said it (under co.lock); a body takes it on its next step,
   and a new body takes it all again. */
static char co_eq_name[CO_PEERS][CO_EQ_SLOTS][CO_AS_NAME];
static int co_eq_num[CO_PEERS][CO_EQ_SLOTS];
/* And the hangs: these are not a state but calls the game made — which equipment call, on which slot — because
   a Link reaching for or putting away a thing is something that happens, not something that stays. They queue
   under co.lock and a body works through them in order; a body not awake yet keeps them, and if the queue fills
   its oldest is forgotten. */
typedef struct { uint8_t kind, slot; } CoEquipEvent;
static CoEquipEvent co_eq_evt[CO_PEERS][CO_EQ_EVENTS];
static int co_eq_evt_head[CO_PEERS], co_eq_evt_tail[CO_PEERS];   /* (under co.lock) */
/* Mine, already said: a hold goes out once, as it changes; a hang goes out every time the game asks for it, the
   same one at most a couple of times a second. Someone joining gets the holds whole again. */
static char co_said_name[CO_EQ_SLOTS][CO_AS_NAME];
static int co_said_num[CO_EQ_SLOTS];
static int64_t co_said_hang_ns[3][CO_EQ_SLOTS];   /* (under co.lock, where the hook writes it) */
/* (how much of this holds-and-hangs chain went to the log: the lines are changes, not frames, and a game left
   swinging still has to stop talking eventually) */
static int co_eq_put_said;

static CoPeer *co_peer(int id, int make) {             /* (under co.lock) */
    CoPeer *free_slot = NULL;
    for (int i = 0; i < CO_PEERS; i++) {
        if (co.peer[i].used && co.peer[i].id == id) return &co.peer[i];
        if (!co.peer[i].used && !free_slot) free_slot = &co.peer[i];
    }
    if (!make || !free_slot) return NULL;
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->used = 1, free_slot->id = id;
    {   /* (another player's movement was here) */
        const size_t at = (size_t)(free_slot - co.peer);
        memset(co_in_have[at], 0, CO_INPUTS), co_in_words[at] = 0, memset(co_cmd_have[at], 0, CO_AS_SLOTS),
        memset(co_eq_name[at], 0, sizeof co_eq_name[at]),
        memset(co_eq_evt[at], 0, sizeof co_eq_evt[at]), co_eq_evt_head[at] = co_eq_evt_tail[at] = 0;
    }
    return free_slot;
}

static void co_peer_name(int id, char *out, size_t cap) {
    EnterCriticalSection(&co.lock);
    const CoPeer *p = co_peer(id, 0);
    snprintf(out, cap, "%s", p && p->name[0] ? p->name : "A player");
    LeaveCriticalSection(&co.lock);
}

/* ---- how another player moves ----
   What was tried first and does not work: the animation controller keeps the pose it has worked out (at +0x30: a
   count of bones at +0x48, 40 bytes a bone at +0x50), and writing another player's pose over a body's looked like
   the whole answer. It is not drawn from: with the body's own animation playing, that is worked out and drawn in one
   go and the write is lost; with it stopped, nothing is drawn afresh and the body stands as it last stood: it
   moves but floats.
   What works is to animate the body the way the game animates Link:
     * the same command in the same slot. Link's controller has nine slots (0 the body: Wait, Move, Jump, CutS1...;
       1 the upper body: CommonUpper...; 4 the face; 8 the mask), co_as_names reads what each plays, co_as_play and
       co_as_remove are the game's own calls.
     * the same inputs. Standing and walking are not clips but blends driven by values the game sets every frame
       (speed, lean, what is in hand...): the object at controller+0x40 (vtable 0x84c49818) holds them, in arrays
       it points at from +0x10..+0xc8, all inside itself (for the child's graph +0x108..+0xca0). Without them Wait
       and Move leave the body in its build pose; with Link's copied over, its legs ran with his.
   So a game says, with its place: "T <slot> <command or ->" when a slot changes (and all of them again every two
   seconds), and "Y <words> <base64>": its inputs as 32-bit words, those that changed, each a u16 number and the
   value (all of them again within four seconds). Words that hold addresses are never said or written.
   This needs a body that has Link's own animation graph: nx2/coop_bodies.py makes those actors from the
   player's game files. The pause menu's Link, which co-op used before, has a graph of menu poses only: there the
   inputs' size does not match and nothing here is applied; it stands as it did. */
static size_t co_base64(const uint8_t *in, size_t n, char *out) {
    static const char set[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = set[v >> 18], out[o++] = set[v >> 12 & 63];
        if (i + 1 < n) out[o++] = set[v >> 6 & 63];
        if (i + 2 < n) out[o++] = set[v & 63];
    }
    out[o] = 0;
    return o;
}

static size_t co_unbase64(const char *in, uint8_t *out, size_t cap) {
    size_t o = 0;
    uint32_t v = 0;
    int bits = 0;
    for (; *in; in++) {
        const char ch = *in;
        const int d = ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52
                    : ch == '+' ? 62 : ch == '/' ? 63 : -1;
        if (d < 0) break;
        v = v << 6 | (uint32_t)d, bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return 0;
            out[o++] = (uint8_t)(v >> bits);
        }
    }
    return o;
}

static void co_inputs_take(int from, const char *text) {    /* (the net thread) "Y <words> <base64>" */
    unsigned words = 0;
    int skip = 0;
    uint8_t raw[CO_LINE];
    if (sscanf(text, "%u %n", &words, &skip) < 1 || !words || words > CO_INPUTS) return;
    const size_t len = co_unbase64(text + skip, raw, sizeof raw);
    EnterCriticalSection(&co.lock);
    CoPeer *p = co_peer(from, 0);
    if (p) {
        const size_t at = (size_t)(p - co.peer);
        if (co_in_words[at] != words) memset(co_in_have[at], 0, CO_INPUTS), co_in_words[at] = words;   /* (another age: another graph) */
        for (size_t o = 0; o + 6 <= len; o += 6) {
            uint16_t word;
            memcpy(&word, raw + o, 2);
            if (word >= words) break;
            memcpy(&co_in_value[at][word], raw + o + 2, 4);
            co_in_have[at][word] = 1;
        }
    }
    LeaveCriticalSection(&co.lock);
}

static void co_command_take(int from, const char *text) {   /* (the net thread) "T <slot> <command or ->" */
    int slot = -1;
    char name[CO_AS_NAME] = "";
    if (sscanf(text, "%d %47s", &slot, name) != 2 || slot < 0 || slot >= CO_AS_SLOTS) return;
    EnterCriticalSection(&co.lock);
    CoPeer *p = co_peer(from, 0);
    if (p) {
        const size_t at = (size_t)(p - co.peer);
        snprintf(co_cmd_name[at][slot], CO_AS_NAME, "%s", strcmp(name, "-") ? name : "");
        co_cmd_have[at][slot] = 1;
    }
    LeaveCriticalSection(&co.lock);
}

/* What every side does with a line from player `from` (the net thread): positions are kept
   here, the comings and goings set the table, the rest is the game side's. */
static void co_take(int from, const char *text) {
    if ((long)from == co.my_id) return;                /* (my own line: I wrote it and my game has done it already.
                                                            Should the room ever throw it back, my place, hearts and
                                                            what I hold would come at me as another player's) */
    if (text[0] == 'Y' && text[1] == ' ') { co_inputs_take(from, text + 2); return; }
    if (text[0] == 'T' && text[1] == ' ') { co_command_take(from, text + 2); return; }
    if (text[0] == 'Q' && text[1] == ' ') { co_enemy_led(from, text + 2); return; }
    if (text[0] == 'P' && text[1] == ' ') {
        char map[64] = "";
        float x = 0, y = 0, z = 0, yaw = 0;
        int child = 1, busy = 0, pvp = 0;
        if (sscanf(text + 2, "%63s %f %f %f %f %d %d %d", map, &x, &y, &z, &yaw, &child, &busy, &pvp) < 5) return;
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 0);
        if (p) {
            const int moved_map = strcmp(p->map, map) != 0;
            snprintf(p->map, sizeof p->map, "%s", map);
            memcpy(p->was, p->pos, sizeof p->was), p->was_yaw = p->yaw, p->seen_before = p->seen;
            p->pos[0] = x, p->pos[1] = y, p->pos[2] = z, p->yaw = yaw, p->child = child, p->busy = busy, p->pvp = pvp;
            p->seen = hos_now_ns();
            if (moved_map || !p->seen_before) memcpy(p->was, p->pos, sizeof p->was), p->was_yaw = yaw, p->seen_before = p->seen;
        }
        LeaveCriticalSection(&co.lock);
        return;
    }
    if (text[0] == 'R' && text[1] == ' ') {              /* a player took another name */
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 0);
        if (p) co_clean(p->name, 17, text + 2, "_-");
        LeaveCriticalSection(&co.lock);
        return;
    }
    if (text[0] == 'J' && text[1] == ' ') {
        char name[64] = "", seed[64] = "-";
        sscanf(text + 2, "%63s %63s", name, seed);
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 1);
        if (p) co_clean(p->name, 17, name, "_-"), co_clean(p->seed, 17, seed, "_-");
        LeaveCriticalSection(&co.lock);
    }
    if (text[0] == 'L' && !text[1]) {                  /* (their name goes with it: the table forgets them here) */
        char left[48] = "L A player";
        EnterCriticalSection(&co.lock);
        CoPeer *p = co_peer(from, 0);
        if (p) { if (p->name[0]) snprintf(left, sizeof left, "L %s", p->name); p->used = 0; }
        LeaveCriticalSection(&co.lock);
        co_event(from, left);
        return;
    }
    co_event(from, text);
}

/* ── the room's view, asked for once ──────────────────────────────────────────
   The name and the host's address are the Co-op tab's fields and the room's settings (gpu/vk/vk_menu.cpp,
   host/coop_menu.c, hookshot_options.ini); the room itself is started there, with Host and Join. What this file needs is
   its view, which it is given. */
static void co_ready(void) {
    if (co.ready) return;
    InitializeCriticalSection(&co.lock);
    InitializeCriticalSection(&co.game);
    snprintf(co.name, sizeof co.name, "Player");
    co.ready = 1;
    coop_on_message(COOP_K_LINE, co_on_line, NULL);
    coop_on_view(co_on_room, NULL);
}
/* ── the game's side ─────────────────────────────────────────────────────── */

/* The save flags: GameDataMgr (hos_main.c OOT_GAMEDATA_SLOT), its bools as f_8022d9c0 reads them:
   +32 the (hash, index) table (+44 its size), +24 the 24-byte entries (+16 their count). An entry, as the
   game's own code writes it (80bc27c0 loads a save into it, 80bcad18 keeps the changed ones at a save):
       +0x10 which save file keeps it (below 0: none)    +0x14 changed since the last save
       +0x15 the value a new file starts with            +0x16 the value now
       +0x17 the value as last saved
   Read here for two things only: which map Link is on, and whether he is a child — what the others are
   told of him, so his body is drawn as the Link of their age. What a room shares of the save itself is
   its rules engine's (host/coop_state.c, host/coop_oot.c, the Co-op tab's Room rules page): progress,
   items, songs, quest items, small keys, rupees and ammunition. Not this file's business. */
#define CO_GAMEDATA_SLOT 0x84c9e180ULL
typedef struct { uint64_t table, entries; uint32_t size, count; } CoFlags;

static int co_flags_open(CoFlags *f) {
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    if (!co_ok(holder, 8)) return 0;
    const uint64_t mgr = G64(holder);
    if (!co_ok(mgr, 0x30)) return 0;
    f->table = G64(mgr + 32), f->size = G32(mgr + 44), f->entries = G64(mgr + 24), f->count = G32(mgr + 16);
    return f->size && f->count && f->size < (1u << 20) && f->count < (1u << 20) && co_ok(f->table, f->size * 8ULL) &&
           co_ok(f->entries, f->count * 24ULL);
}

static uint8_t *co_flag_value(const CoFlags *f, uint32_t hash, uint32_t *index) {
    for (uint32_t probe = 0, slot = hash % f->size; probe < f->size; probe++, slot = (slot + 1) % f->size) {
        const uint32_t key = G32(f->table + slot * 8ULL);
        if (!key) return NULL;
        if (key != hash) continue;
        const uint32_t i = G32(f->table + slot * 8ULL + 4);
        if (i >= f->count) return NULL;
        if (index) *index = i;
        return (uint8_t *)GP(f->entries + i * 24ULL + 22);
    }
    return NULL;
}

static uint32_t co_mm3(const char *text) {             /* MurMur3, seed 0: the game's name hash */
    const size_t len = strlen(text), blocks = len / 4;
    uint32_t h = 0, k = 0;
    for (size_t i = 0; i < blocks; i++) {
        memcpy(&k, text + i * 4, 4);
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    for (size_t i = len & 3; i > 0; i--) k = (k << 8) | (unsigned char)text[blocks * 4 + i - 1];
    if (len & 3) k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u, h ^= k;
    h ^= (uint32_t)len, h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

/* A save value of the Int kind, by its name's hash. Read here for my hearts (PlayerStatus.MaxLife, co_my_quarters);
   what a room shares of the counted values — small keys, rupees, ammunition — is its rules engine's own (host/coop_state.c). */
static int32_t *co_int_value(uint32_t hash) {
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    const uint64_t mgr = co_ok(holder, 8) ? G64(holder) : 0;
    if (!co_ok(mgr, 0x100)) return NULL;
    const uint64_t entries = G64(mgr + 0xd8), table = G64(mgr + 0xe0);
    const uint32_t count = G32(mgr + 0xd0), size = G32(mgr + 0xec);
    if (!size || !count || size > (1u << 20) || count > (1u << 20) || !co_ok(table, size * 8ULL) || !co_ok(entries, count * 40ULL)) return NULL;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = (slot + 1) % size) {
        const uint32_t key = G32(table + slot * 8ULL);
        if (!key) return NULL;
        if (key != hash) continue;
        const uint32_t i = G32(table + slot * 8ULL + 4);
        return i < count ? (int32_t *)GP(entries + i * 40ULL + 0x1c) : NULL;
    }
    return NULL;
}

/* Sequence_CurrentBanc (hos_main.c oot_gamedata_string: f_80cd0ea0, String64 by name hash) */
static void co_current_map(Ctx *c, char *buf, size_t size) {
    buf[0] = 0;
    const uint64_t holder = G64(CO_GAMEDATA_SLOT);
    const uint64_t mgr = co_ok(holder, 8) ? G64(holder) : 0;
    A64Fn get = a64_lookup(0x80cd0ea0ULL);
    if (!co_ok(mgr, 0x30) || !get) return;
    const Ctx saved = *c;
    const uint64_t out = (c->x[31] - 0x40) & ~0xfULL;
    recomp_store64_sp(c, out, 0);
    c->x[31] = out;
    c->x[0] = mgr, c->x[1] = out, c->x[2] = 0x1d6189dau;
    get(c);
    const int found = (int)(c->x[0] & 1);
    const uint64_t text = recomp_load64(c, out);
    *c = saved;
    if (!found || !co_ok(text, 64)) return;
    const char *s = (const char *)GP(text);
    size_t n = 0;
    for (; n + 1 < size && n < 63 && s[n] > ' ' && s[n] < 0x7f; n++) buf[n] = s[n];
    buf[n] = 0;
}

/* "KokiriForest" as "Kokiri Forest" */
static void co_map_words(const char *map, char *out, size_t cap) {
    size_t n = 0, len = strlen(map);
    if (len > 6 && (!strcmp(map + len - 6, "_Child") || !strcmp(map + len - 6, "_Adult"))) len -= 6;   /* (each age has its own copy of a map) */
    for (size_t i = 0; i < len && n + 2 < cap; i++) {
        const char ch = map[i] == '_' ? ' ' : map[i];
        if (i && ch >= 'A' && ch <= 'Z' && map[i - 1] >= 'a' && map[i - 1] <= 'z') out[n++] = ' ';
        out[n++] = ch;
    }
    out[n] = 0;
}

/* What the others found that this layer gives one for one — a heart container, a Piece of Heart, a Gold Skulltula
   token — and how many are still to hand over. Those the Inventory page lists go by their place in it; any other by its
   name (co_more). */
static int co_to_give[256];
typedef struct { char name[0x60]; int to_give; } CoMore;
static CoMore co_more[96];
static int co_more_n;
static int co_their_quarters = -1;                    /* the most hearts another player said they have, in quarters */

static CoMore *co_more_get(const char *name) {
    for (int i = 0; i < co_more_n; i++)
        if (!strcmp(co_more[i].name, name)) return &co_more[i];
    if (co_more_n >= (int)(sizeof co_more / sizeof co_more[0]) || strlen(name) >= 0x60) return NULL;
    CoMore *m = &co_more[co_more_n++];
    memset(m, 0, sizeof *m);
    strcpy(m->name, name);
    return m;
}

static void co_give_named(Ctx *c, const char *name) {
    co_applying = 1;
    const int status = co_pouch_give(c, name);
    co_applying = 0;
    hos_log("coop: gave %s (status %d)", name, status);
}
/* (counted things: the lot in one call, as the game itself adds thirty seeds at once) */
static void co_give_many(Ctx *c, const char *name, int many) {
    co_applying = 1;
    const int status = co_pouch_give_many(c, name, many);
    co_applying = 0;
    hos_log("coop: gave %s x%d (status %d)", name, many, status);
}

/* An item that takes another's place. The game itself swaps them (an egg hatches: the egg goes, the cucco comes), but the
   others are only told what was found, so they kept both, and the game went on showing them the old one: a Weird Egg
   stayed an egg after another player's hatched, with Item_AlarmCucco in the pouch all along. Whoever holds a later one
   of a row has the earlier ones taken away. Looked at every two seconds of play, so a save that already holds both is put
   right too. */
static const char *const co_trade_rows[][11] = {
    {"Item_WeirdEgg", "Item_AlarmCucco", "Item_ZeldaLetter", NULL},
    {"Item_PocketEgg", "Item_PocketCucco", "Item_CojiroCucco", "Item_OddMushroom", "Item_PoacherSaw", "Item_Sword_BrokenGiant",
     "Item_Prescription", "Item_EyeballFrog", "Item_ClaimCheck", NULL},
};
static void co_trade_step(Ctx *c, int64_t now) {
    static int64_t next;
    if (now < next) return;
    next = now + 2000000000LL;
    for (unsigned r = 0; r < sizeof co_trade_rows / sizeof co_trade_rows[0]; r++) {
        int last = -1;
        for (int k = 0; co_trade_rows[r][k]; k++)
            if (co_pouch_count(c, co_trade_rows[r][k]) > 0) last = k;
        for (int k = 0; k < last; k++) {
            const int held = co_pouch_count(c, co_trade_rows[r][k]);
            if (held <= 0) continue;
            co_applying = 1;
            const int status = co_pouch_take(c, co_trade_rows[r][k], held);
            co_applying = 0;
            hos_log("coop: %s took the place of %s: taken away (status %d)", co_trade_rows[r][last], co_trade_rows[r][k], status);
        }
    }
}

/* My hearts in quarters: containers (PlayerStatus.MaxLife counts them in quarters already) and pieces. */
static int co_my_quarters(Ctx *c) {
    static uint32_t hash;
    if (!hash) hash = co_mm3("PlayerStatus.MaxLife");
    const int32_t *max = co_int_value(hash);
    const int pieces = co_pouch_count(c, "Obj_Heart_Piece");
    if (!max || pieces < 0 || *max < 4 || *max > 400) return -1;
    return *max + (pieces > 3 ? 3 : pieces);
}

/* One item a tick at most, a little apart: the game shows each one it is handed. */
static void co_items_step(Ctx *c, int64_t now) {
    static int64_t next;
    static int at;
    if (now < next) return;
    if (co_pouch_count(c, "Item_DekuSeed") < 0) return;   /* the pouch is not ready to be asked */
    if (co_their_quarters > 0) {                       /* hearts another player has and I do not: a container, or a piece */
        const int mine = co_my_quarters(c);
        if (mine > 0 && co_their_quarters > mine) {
            co_give_named(c, co_their_quarters - mine >= 4 ? "Obj_Heart_Container" : "Obj_Heart_Piece");
            next = now + 600000000LL;
            return;
        }
        if (mine > 0) co_their_quarters = -1;
    }
    const int total = CO_ITEMS + co_more_n;
    for (int tried = 0; tried < total; tried++, at = (at + 1) % total) {
        const int i = at % total;
        if (i >= CO_ITEMS) {
            CoMore *m = &co_more[i - CO_ITEMS];
            if (m->to_give <= 0) continue;
            const int many = m->to_give > 99 ? 99 : m->to_give;
            m->to_give -= many;
            co_give_named(c, m->name);
        } else if (co_item_kind[i] != 0 && co_to_give[i] > 0) {
            char give[0x70], other[2][0x70];
            const int many = co_to_give[i] > 99 ? 99 : co_to_give[i];
            co_to_give[i] -= many;
            co_item_spellings(i, give, other);
            co_give_many(c, give, many);
        } else continue;
        at = (at + 1) % total;
        next = now + 400000000LL;
        return;
    }
}

/* My hearts, said quietly: for a player who just joined, so that the hearts one for one catch up. What else a room
   shares of what everyone holds is its rules engine's, and it levels that itself. */
static void co_say_hearts(Ctx *c) {
    const int quarters = co_my_quarters(c);
    if (quarters > 0) co_send("M %d", quarters);
}

/* A heart container, a Piece of Heart or a Gold Skulltula token: the three the room's rules leave to this layer,
   each given one for one (canon: the name it goes by between games). */
static int co_heart_or_token(const char *name, char canon[0x60]) {
    if (strstr(name, "Heart_Piece")) return snprintf(canon, 0x60, "Obj_Heart_Piece"), 1;
    if (!strcmp(name, "Obj_Heart_Container")) return snprintf(canon, 0x60, "%s", name), 1;
    if (strstr(name, "Skulltula") && strstr(name, "Token")) return snprintf(canon, 0x60, "%s", name), 1;
    canon[0] = 0;
    return 0;
}

/* (oot_coop_pouch_add, any of the game's threads) an item entered the pouch by this name */
static void co_pickup(const char *name, int many) {
    if (!co.ready || !co_room() || co_applying) return;
    char canon[0x60];
    char canon2[0x60];
    if (!co_heart_or_token(name, canon2)) return;        /* (the room's rules share the rest: host/coop_state.c) */
    const int kind = co_item_class(name, canon);
    if (kind == 2) co_send("I %s %d", canon, many < 1 ? 1 : many > 99 ? 99 : many);
}

/* (hos_main.c hos_oot_lift_player, the player's own actor each frame) where Link stands */
static void co_tick(Ctx *c, uint64_t link);
void oot_coop_player(Ctx *c, uint64_t actor) {
    (void)c;
    if (!co.ready || !hos_guest_range(actor, 848)) return;   /* (not co_ok: Link is not in the heap's first stretch) */
    {
        static int said;
        if (!said) said = 1, hos_log("coop: Link's actor seen at %llx", (unsigned long long)actor);
    }
    float v[3], sx, sz;
    memcpy(v, GP(actor + 796), sizeof v);
    memcpy(&sx, GP(actor + 816), 4);
    memcpy(&sz, GP(actor + 840), 4);
    EnterCriticalSection(&co.lock);
    memcpy(co.pos, v, sizeof v);
    co.yaw = atan2f(sx, sz);
    LeaveCriticalSection(&co.lock);
    co.player_seen = hos_now_ns();
    co_tick(c, actor);
}


static void co_events(void) {
    CoEvent *list = NULL; size_t n = 0;
    EnterCriticalSection(&co.lock);
    if (co.in_len && (list = malloc(co.in_len * sizeof *list)) != NULL) memcpy(list, co.in, co.in_len * sizeof *list), n = co.in_len;
    if (list || !co.in_len) co.in_len = 0;
    LeaveCriticalSection(&co.lock);
    for (size_t e = 0; e < n; e++) {
        const char *text = list[e].text;
        char who[24];
        co_peer_name(list[e].from, who, sizeof who);
        if (text[0] == 'L' && text[1] == ' ') {
        } else if (text[0] == 'D' && text[1] == ' ') {     /* a player's swing reached someone: me? */
            int target = -1, quarters = 0;
            if (sscanf(text + 2, "%d %d", &target, &quarters) == 2 && target == (int)co.my_id && quarters > 0 && quarters <= 8) {
                snprintf(co_hurt_by, sizeof co_hurt_by, "%s", who);
                InterlockedExchange(&co_hurt_from, list[e].from);
                InterlockedExchange(&co_hurt, quarters);
            }
        } else if (text[0] == 'U' && text[1] == ' ') {     /* a player's hit took life off an enemy: the same off mine */
            unsigned hash = 0, taken = 0;
            float at[3] = {0, 0, 0};
            if (sscanf(text + 2, "%x %f %f %f %u", &hash, &at[0], &at[1], &at[2], &taken) == 5 && taken > 0 && taken < 100000) co_enemy_hit(hash, at, taken);
        } else if (text[0] == 'O' && text[1] == ' ') {     /* a player lost their last heart to another's hit */
            const int by = atoi(text + 2);
            char winner[24];
            co_peer_name(by, winner, sizeof winner);
            if (by == (int)co.my_id) co_note("You defeated %s", who);
            else co_note("%s defeated %s", winner, who);
        } else if (text[0] == 'I' && text[1] == ' ') {
            char name[0x70] = "", canon[0x60], quiet[8] = "";
            int many = 1;
            if (sscanf(text + 2, "%100s %d %7s", name, &many, quiet) < 1) continue;
            if (!co_heart_or_token(name, canon)) continue;   /* (hearts and Gold Skulltula tokens only: the room's rules
                                                                share the other finds) */
            many = many < 1 ? 1 : many > 99 ? 99 : many;
            const int i = co_item_index(canon);
            const char *label = canon;
            if (i >= 0 && i < 256) {
                label = co_item_label[i];
                co_to_give[i] += many;
            } else {
                CoMore *m = co_more_get(canon);
                if (!m) continue;
                m->to_give += many;
                label = !strcmp(canon, "Obj_Heart_Piece") ? "a Piece of Heart" : !strcmp(canon, "Obj_Heart_Container") ? "a Heart Container"
                      : !strncmp(canon, "Item_", 5) ? canon + 5 : canon;
            }
            if (!quiet[0]) co_note("%s found %s", who, label);
        } else if (text[0] == 'E' && text[1] == ' ') {   /* that player's Link holds this: kept for their body */
            static int heard;                             /* (a few lines each run: did the room really bring them) */
            if (heard < 40) heard++, hos_log("coop: heard from the room: %s", text), hos_log_flush();
            int slot = 0, num = 0;
            char name[CO_AS_NAME] = "";
            if (sscanf(text + 2, "%d %47s %d", &slot, name, &num) >= 2 && slot >= 0 && slot < CO_EQ_SLOTS) {
                EnterCriticalSection(&co.lock);
                const CoPeer *p = co_peer(list[e].from, 1);
                if (p) {
                    const size_t at = (size_t)(p - co.peer);
                    snprintf(co_eq_name[at][slot], CO_AS_NAME, "%s", name), co_eq_num[at][slot] = num;
                }
                LeaveCriticalSection(&co.lock);
            }
        } else if (text[0] == 'S' && text[1] == ' ') {   /* that player's game asked their Link to reach for or put away this */
            int kind = 0, slot = 0;
            if (sscanf(text + 2, "%d %d", &kind, &slot) == 2 && kind >= 1 && kind <= 2 && slot >= 0 && slot < CO_EQ_SLOTS) {
                EnterCriticalSection(&co.lock);
                const CoPeer *p = co_peer(list[e].from, 1);
                if (p) {
                    const size_t at = (size_t)(p - co.peer);
                    const int t = co_eq_evt_tail[at];
                    co_eq_evt[at][t].kind = (uint8_t)kind, co_eq_evt[at][t].slot = (uint8_t)slot;
                    co_eq_evt_tail[at] = (t + 1) % CO_EQ_EVENTS;
                    if (co_eq_evt_tail[at] == co_eq_evt_head[at])   /* (their body is not awake: the oldest reach is forgotten) */
                        co_eq_evt_head[at] = (co_eq_evt_head[at] + 1) % CO_EQ_EVENTS;
                }
                LeaveCriticalSection(&co.lock);
            }
        } else if (text[0] == 'G' && text[1] == ' ') {     /* (test runs only: travel to this map) */
            const char *e = getenv("HOS_COOP_TEST");
            if (e && !strcmp(e, "1")) snprintf(co_test_go, sizeof co_test_go, "%s", text + 2);
        } else if (text[0] == 'A' && text[1] == ' ') {     /* (test runs only: play this animation command on the bodies) */
            const char *e = getenv("HOS_COOP_TEST");
            int slot = 0;
            char name[CO_AS_NAME] = "";
            if (e && !strcmp(e, "1") && sscanf(text + 2, "%d %47s", &slot, name) == 2) {
                co_test_play_slot = slot;
                snprintf(co_test_play_name, sizeof co_test_play_name, "%s", name);
                InterlockedExchange(&co_test_play, 1);
            }
        } else if (text[0] == 'Z' && text[1] == ' ') {     /* (test runs only: make this actor beside Link) */
            static int allowed = -1;
            if (allowed < 0) { const char *e = getenv("HOS_COOP_TEST"); allowed = e && !strcmp(e, "1"); }
            int option = 0;
            if (allowed && sscanf(text + 2, "%95s %d", co_test_name, &option) >= 1) co_test_option = option, InterlockedExchange(&co_test_spawn, 1);
        } else if (text[0] == 'M' && text[1] == ' ') {
            const int quarters = atoi(text + 2);
            if (quarters > co_their_quarters && quarters <= 400) co_their_quarters = quarters;
        }
    }
    free(list);
}

static void co_players_list(int64_t now, const char *map, int playing) {
    static char last_list[700];
    char list[700] = "";                               /* ("Name: where", a line each, in the room's order: co_goto_ask counts the same way) */
    size_t listed = 0;
    float mine[3];
    EnterCriticalSection(&co.lock);
    memcpy(mine, co.pos, sizeof mine);
    for (int i = 0; i < CO_PEERS; i++) {
        const CoPeer *p = &co.peer[i];
        if (!p->used) continue;
        char where[112];
        if (!p->map[0] || now - p->seen > 3000000000LL) snprintf(where, sizeof where, "not in the game yet");
        else if (playing && !strcmp(p->map, map)) {
            const float dx = p->pos[0] - mine[0], dy = p->pos[1] - mine[1], dz = p->pos[2] - mine[2];
            snprintf(where, sizeof where, "here, %.0f m away", sqrtf(dx * dx + dy * dy + dz * dz));
        } else {
            char words[96];
            co_map_words(p->map, words, sizeof words);
            snprintf(where, sizeof where, "%s", words);
        }
        if (listed < sizeof list - 150)
            listed += (size_t)snprintf(list + listed, sizeof list - listed, "%s: %s\n", p->name, where);
    }
    LeaveCriticalSection(&co.lock);
    if (strcmp(list, last_list)) snprintf(last_list, sizeof last_list, "%s", list), co_menu_text(4, list);
}

/* ---- go to a player ----
   The Co-op tab has a "Go to player" row for each of the others. In the same map it is a step: Link is stood
   beside them, by the call the debug tools' "return to the mark" makes (the controller's change-position, hos_main.c
   oot_update_player_cheats). In another map it is the debug tools' teleport to that map first (hos_oot_travel),
   then the step once the map is in. Not during an event or a load, as everything else here. */
int hos_oot_travel(Ctx *c, uint64_t actor, const char *banc);   /* hos_main.c */
static int co_game_busy(void);
static struct { int id, stage; int64_t since, settle; char name[24]; } co_go;   /* stage 1 asked, 2 the map is changing */

static void co_goto_ask(int which) {
    int n = 0, found = 0;
    EnterCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS && !found; i++) {
        if (!co.peer[i].used || n++ != which) continue;
        co_go.id = co.peer[i].id;
        snprintf(co_go.name, sizeof co_go.name, "%s", co.peer[i].name);
        found = 1;
    }
    LeaveCriticalSection(&co.lock);
    if (!found) return;
    co_go.stage = 1, co_go.since = hos_now_ns(), co_go.settle = 0;
    co_note("Going to %s...", co_go.name);
}

static int co_place(Ctx *c, uint64_t link, const float at[3]) {
    static const unsigned step[] = {80, 32, 16, 8};    /* (Link's components to his controller) */
    uint64_t controller = hos_guest_range(link + 624, 8) ? G64(link + 624) : 0;
    for (unsigned i = 0; i < 4 && controller; i++) controller = hos_guest_range(controller + step[i], 8) ? G64(controller + step[i]) : 0;
    A64Fn change = a64_lookup(0x806d2530ULL);
    if (!change || !hos_guest_range(controller, 8) || G64(controller) != 0x84c54f10ULL || G64(0x84c54f10ULL + 0xe8) != 0x806d2530ULL) return 0;
    const Ctx saved = *c;
    const uint64_t arg = c->x[31] - 16;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t bits;
        memcpy(&bits, &at[i], 4);
        recomp_store32_sp(c, arg + i * 4, bits);
    }
    recomp_store32_sp(c, arg + 12, 0);
    c->x[31] = arg;
    c->x[0] = controller, c->x[1] = arg;
    change(c);
    *c = saved;
    return 1;
}

static void co_goto_step(Ctx *c, uint64_t link, int64_t now) {
    if (!co_go.stage) return;
    char map[64] = "", words[96];
    float at[3] = {0, 0, 0}, yaw = 0;
    int there = 0;
    EnterCriticalSection(&co.lock);
    const CoPeer *p = co_peer(co_go.id, 0);
    if (p && p->map[0] && now - p->seen < 3000000000LL) {
        there = 1, yaw = p->yaw;
        snprintf(map, sizeof map, "%s", p->map);
        memcpy(at, p->pos, sizeof at);
    }
    LeaveCriticalSection(&co.lock);
    if (!there) { co_go.stage = 0; co_note("%s is not in the game right now", co_go.name); return; }
    if (now - co_go.since > 45000000000LL) { co_go.stage = 0; co_note("Could not get to %s", co_go.name); return; }
    if (!co_map[0] || co_game_busy()) { co_go.settle = now + 1500000000LL; return; }
    if (now < co_go.settle) return;
    co_map_words(map, words, sizeof words);
    if (!strcmp(map, co_map)) {
        at[0] -= sinf(yaw) * 0.8f, at[2] -= cosf(yaw) * 0.8f, at[1] += 0.05f;   /* (a step behind them, not inside them) */
        const int ok = co_place(c, link, at);
        co_go.stage = 0;
        co_note(ok ? "You are with %s" : "Could not move to %s", co_go.name);
        hos_log("coop: go to %s: placed at %.1f %.1f %.1f in %s (%d)", co_go.name, at[0], at[1], at[2], map, ok);
        return;
    }
    if (co_go.stage == 2) {                             /* the map came in, and it is not theirs (they moved on, or the other age's) */
        if (now - co_go.since < 20000000000LL) return;
        co_go.stage = 0;
        co_note("%s is in %s now", co_go.name, words);
        return;
    }
    const int asked = hos_oot_travel(c, link, map);
    hos_log("coop: go to %s: travel to %s (%d)", co_go.name, map, asked);
    if (asked == 1) co_go.stage = 2, co_go.settle = now + 3000000000LL;
    else {
        co_go.stage = 0;
        if (asked < 0) co_note("%s is in %s: there is no way to travel into there, only to walk", co_go.name, words);
        else co_note("Cannot travel just now: try again in a moment");
    }
}

/* ── the other players, drawn ────────────────────────────────────────────────
 *
 * The game makes an actor by name through one function, f_82c4b220 (the one that names itself
 * "createActorAndConnectSync" to the engine; its only caller, at 82ebbf18, makes a horse's rider):
 *     f_82c4b220(ActorMgr, make, 0, &name, &arg, 0, 1, 0; on the stack: 0, 0, set-up, 0, 0, 0,
 *                "createActorAndConnectSync", a link for the new actor or 0, 8)
 *   ActorMgr  *(*0x84c9f3e0)
 *   make      f_82ec15e0: the manager's own "make it and start it" (f_8080b990, then f_83acfe30)
 *   set-up    f_82ec16c0: does nothing
 *   arg       as the caller fills it (82eb9fd0 on) and f_82c4be30 reads it: +0 place, +0xc a 3x3 turn,
 *             +0x30 size, +0x40 a text (must point at one, an empty one will do: it is read without a
 *             check, and 0 there was this code's first crash), +0x58..0x98 zero (+0x60 a list of values to
 *             give the actor: none), +0x98 which of place (1), turn (2) and size (4) are given
 * It answers with the actor. An actor keeps its place at +0x31c, its turn at +0x328 and its size at +0x34c
 * (the same caller reads them from the rider's horse; Link's are there too: hos_main.c); co_body_put moves one.
 *
 * The other players are PouchPlayer_Child and PouchPlayer_Adult: the Link the inventory screen shows, a
 * Link with nothing driving him. One is made for each player on this map and moved to where they say they
 * are; one whose player left the map is put away under the ground and used again. A map change takes all of
 * them with it (the game's own clean-up), which is seen as Link himself being another actor. */
#define CO_CREATE_ACTOR 0x82c4b220ULL
#define CO_CREATE_MAKE  0x82ec15e0ULL
#define CO_CREATE_SETUP 0x82ec16c0ULL
#define CO_CREATE_NAME  0x83dfa34aULL
#define CO_ACTORMGR_SLOT 0x84c9f3e0ULL
#define CO_ACTOR_PLACE 0x31c
#define CO_ACTOR_TURN  0x328

static void co_turn(float yaw, float m[9]) {           /* (hos_main.c oot_write_flight_rotation: about the up axis) */
    const float sn = sinf(yaw), cs = cosf(yaw);
    const float t[9] = {cs, 0, sn, 0, 1, 0, -sn, 0, cs};
    memcpy(m, t, sizeof t);
}

static uint64_t co_create(Ctx *c, const char *name, const float place[3], float yaw, int option) {
    A64Fn create = a64_lookup(CO_CREATE_ACTOR);
    const uint64_t holder = G64(CO_ACTORMGR_SLOT);
    const uint64_t mgr = hos_guest_range(holder, 8) ? G64(holder) : 0;
    if (!create || !hos_guest_range(mgr, 0x110) || strlen(name) >= 0x60) return 0;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x500) & ~0xfULL;   /* the stack arguments, then our own blocks above them */
    const uint64_t arg = frame + 0x80, name_ref = frame + 0x2c0, name_at = frame + 0x2d0, list = frame + 0x340;
    memset(A64_PTR(frame), 0, 0x400);
    float turn[9];
    const float size[3] = {1, 1, 1};
    co_turn(yaw, turn);
    memcpy(A64_PTR(arg), place, 12);
    memcpy(A64_PTR(arg + 0xc), turn, sizeof turn);
    memcpy(A64_PTR(arg + 0x30), size, sizeof size);
    *(uint8_t *)A64_PTR(arg + 0x48) = (uint8_t)(option & 1);
    recomp_store64_sp(c, arg + 0x40, list);              /* (a text the actor may be given: an empty one. Not 0: it is read) */
    *(uint8_t *)A64_PTR(arg + 0x98) = 7;
    strcpy((char *)A64_PTR(name_at), name);
    recomp_store64_sp(c, name_ref, name_at);
    recomp_store64_sp(c, frame + 0x10, CO_CREATE_SETUP);
    recomp_store64_sp(c, frame + 0x30, CO_CREATE_NAME);
    recomp_store64_sp(c, frame + 0x40, 8);
    c->x[31] = frame;
    c->x[0] = mgr, c->x[1] = CO_CREATE_MAKE, c->x[2] = 0, c->x[3] = name_ref, c->x[4] = arg, c->x[5] = 0, c->x[6] = 1, c->x[7] = 0;
    create(c);
    const uint64_t actor = c->x[0];
    *c = saved;
    return hos_guest_range(actor, 0x360) ? actor : 0;
}

typedef struct { uint64_t actor, made_of; int child, posed, foe; uint32_t life_full; int64_t tried, struck; float push[2], push_far;
                 char eq_name[CO_EQ_SLOTS][CO_AS_NAME]; int eq_num[CO_EQ_SLOTS];
                 uint64_t retired, retired_of;   /* (the body this one replaced: put under and kept under, see co_bodies_step) */
                 int64_t child_at; int child_want; } CoBody;
static CoBody co_body[CO_PEERS];
static uint64_t co_link_actor;                        /* Link's own actor when the bodies were made */

/* Moving one. Writing its place (+0x31c) moves nothing: the picture stays where it was made. The game moves an actor
   by asking (f_80c8fd60, what its OneShotActorRequestSetMtx script node calls): the place wanted goes to +0x358 and
   the turn wanted to +0x364, and the actor takes them at its next update and marks them taken (the first value of
   each becomes "not a number"). So the first value of each is written last: the rest is in place when it is seen. */
#define CO_ACTOR_ASK_PLACE 0x358
#define CO_ACTOR_ASK_TURN  0x364
static void co_body_put(uint64_t actor, const float place[3], float yaw) {
    float turn[9];
    co_turn(yaw, turn);
    uint8_t *a = (uint8_t *)GP(actor);
    memcpy(a + CO_ACTOR_ASK_TURN + 4, turn + 1, 32);
    memcpy(a + CO_ACTOR_ASK_PLACE + 4, place + 1, 8);
    memcpy(a + CO_ACTOR_ASK_TURN, turn, 4);
    memcpy(a + CO_ACTOR_ASK_PLACE, place, 4);
    HOS_WRITTEN(a + CO_ACTOR_ASK_PLACE, 48);
}

/* ---- how a player moves: the animation ----
   An actor's animation controller is at actor+0x270 -> +0x40 -> +0x18 -> +0x18 (as the game's own "play this" request,
   f_802222a0, walks to it). It has slots: their count at +0x18, an array of 0x48-byte entries at +0x20 whose first
   word is the slot; a slot's +0x10 is the name of the command it plays now (a text, "" for none).
   The game plays a command by name with f_802228f0(controller, &name, slot, 0, 0, 0, 0, 0; s0 a blend the caller
   reads from a global, s1 -1): what f_802222a0 ends in, after a trace call this side has no use for. */
static uint64_t co_as_ctl(uint64_t actor) {
    static const unsigned step[] = {0x270, 0x40, 0x18, 0x18};
    uint64_t at = actor;
    for (unsigned i = 0; i < 4 && at; i++) at = co_ok(at + step[i], 8) ? G64(at + step[i]) : 0;
    return co_ok(at, 0x138) ? at : 0;
}

static int co_as_names(uint64_t actor, char names[CO_AS_SLOTS][CO_AS_NAME]) {
    const uint64_t ctl = co_as_ctl(actor);
    if (!ctl) return 0;
    const uint32_t count = G32(ctl + 0x18);
    const uint64_t slots = G64(ctl + 0x20);
    if (!count || count > 64 || !co_ok(slots, count * 0x48ULL)) return 0;
    int n = 0;
    for (uint32_t i = 0; i < count && n < CO_AS_SLOTS; i++, n++) {
        memset(names[n], 0, CO_AS_NAME);
        const uint64_t slot = G64(slots + i * 0x48ULL);
        const uint64_t name = co_ok(slot, 0x18) ? G64(slot + 0x10) : 0;
        if (!co_ok(name, CO_AS_NAME)) continue;
        const char *text = (const char *)GP(name);
        size_t len = 0;
        while (len < CO_AS_NAME - 1 && text[len] > 0x20 && text[len] < 0x7f) len++;
        if (text[len]) continue;                        /* (not a short plain word: not a name) */
        memcpy(names[n], text, len + 1);
    }
    return n;
}

static int co_as_play(Ctx *c, uint64_t actor, int slot, const char *name) {
    const uint64_t ctl = co_as_ctl(actor);
    A64Fn play = a64_lookup(0x802228f0ULL);
    if (!ctl || !play || slot < 0 || (uint32_t)slot >= G32(ctl + 0x18) || strlen(name) >= CO_AS_NAME) return -1;
    const uint64_t global = G64(0x84eb4b20ULL);
    float blend = 1.0f;
    if (hos_guest_range(global, 4)) memcpy(&blend, GP(global), 4);
    const float from = -1.0f;
    uint32_t bits[2];
    memcpy(&bits[0], &blend, 4), memcpy(&bits[1], &from, 4);
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memset(A64_PTR(frame), 0, 0x80);
    memcpy(A64_PTR(frame + 0x10), name, strlen(name) + 1);
    recomp_store64_sp(c, frame, frame + 0x10);
    c->x[31] = frame;
    c->x[0] = ctl, c->x[1] = frame, c->x[2] = (uint64_t)slot, c->x[3] = 0, c->x[4] = 0, c->x[5] = 0, c->x[6] = 0, c->x[7] = 0;
    c->vreg[0][0] = bits[0], c->vreg[0][1] = 0, c->vreg[1][0] = bits[1], c->vreg[1][1] = 0;
    play(c);
    const int ok = (int)(c->x[0] & 1);
    *c = saved;
    return ok;
}

/* A slot plays nothing any more: what the script node OneShotASRemoveNoMorph (f_828cf2d0) calls, f_837d5c10(controller,
   slot, 0; d0 0). */
static int co_as_remove(Ctx *c, uint64_t actor, int slot) {
    const uint64_t ctl = co_as_ctl(actor);
    A64Fn remove = a64_lookup(0x837d5c10ULL);
    if (!ctl || !remove || slot < 0 || (uint32_t)slot >= G32(ctl + 0x18)) return -1;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x40) & ~0xfULL;
    c->x[0] = ctl, c->x[1] = (uint64_t)slot, c->x[2] = 0;
    c->vreg[0][0] = 0, c->vreg[0][1] = 0;
    remove(c);
    const int result = (int)(c->x[0] & 0xff);
    *c = saved;
    return result;
}

/* An actor's animation inputs: where they start and how many 32-bit words (see "how another player moves").
   No hardcoded vtable: child and adult Link wear different controllers, and each game was lifted at a different
   build, so the constants were wrong as often as right; what decides is the shape — the holder, its table of pointers
   to one contiguous block of words — that only this layout has. Only Link and a co-op body are ever looked up. */
static uint64_t co_inputs_at(uint64_t actor, uint32_t *words) {
    const uint64_t ctl = co_as_ctl(actor);
    if (!ctl || !G64(ctl)) return 0;
    const uint64_t holder = G64(ctl + 0x40);
    if (!co_ok(holder, 0x110) || !G64(holder)) return 0;
    uint64_t lo = ~0ULL, hi = 0;
    for (unsigned o = 0x10; o <= 0xc8; o += 8) {
        const uint64_t at = G64(holder + o);
        if (!at) continue;
        if (at < holder + 0x100 || at > holder + 0x4000) return 0;   /* (not inside the object: not the layout this knows) */
        if (at < lo) lo = at;
        if (at + 8 > hi) hi = at + 8;
    }
    if (lo >= hi || ((hi - lo) & 3) || (hi - lo) / 4 > CO_INPUTS || !co_ok(lo, hi - lo)) return 0;
    *words = (uint32_t)((hi - lo) / 4);
    return lo;
}

/* Is the word at this place half of an address? (an 8-byte value, on an 8-byte boundary, that points into the game) */
static int co_input_is_address(uint64_t word_at) {
    const uint64_t v = G64(word_at & ~7ULL);
    return (v >= HOS_HEAP_BASE && v < HOS_HEAP_BASE + 0x400000000ULL) || (v >> 32) == 0x7f || (v >= 0x80000000ULL && v < 0x90000000ULL && (v & 3) == 0 && v > 0x80001000ULL);
}

/* (with my place, thirty times a second) what my Link plays and his inputs, for the others */
static void co_anim_say(uint64_t link) {
    static char said_name[CO_AS_SLOTS][CO_AS_NAME];
    static uint8_t said_slot[CO_AS_SLOTS];
    static uint32_t said_value[CO_INPUTS], said_words, turn;
    static uint8_t said[CO_INPUTS];
    char names[CO_AS_SLOTS][CO_AS_NAME];
    const int slots = co_as_names(link, names);
    turn++;
    for (int i = 0; i < slots; i++) {
        if (said_slot[i] && !strcmp(said_name[i], names[i]) && turn % 60 != (uint32_t)i) continue;
        memcpy(said_name[i], names[i], CO_AS_NAME), said_slot[i] = 1;
        co_send("T %d %s", i, names[i][0] ? names[i] : "-");
    }
    uint32_t words = 0;
    const uint64_t at = co_inputs_at(link, &words);
    if (!at) return;
    if (words != said_words) memset(said, 0, sizeof said), said_words = words;
    uint8_t raw[660];
    char text[900];
    size_t used = 0;
    for (uint32_t i = 0; i < words; i++) {
        if (co_input_is_address(at + i * 4ULL)) continue;
        const uint32_t v = G32(at + i * 4ULL);
        if (said[i] && said_value[i] == v && i % 120 != turn % 120) continue;
        if (used + 6 > sizeof raw) {
            co_base64(raw, used, text);
            co_send("Y %u %s", words, text);
            used = 0;
        }
        const uint16_t word = (uint16_t)i;
        memcpy(raw + used, &word, 2), memcpy(raw + used + 2, &v, 4), used += 6;
        said_value[i] = v, said[i] = 1;
    }
    if (used) {
        co_base64(raw, used, text);
        co_send("Y %u %s", words, text);
    }
}

/* (every frame) a body is given that player's inputs and plays what their Link plays. */
static void co_anim_put(Ctx *c, uint64_t actor, int peer) {
    static uint32_t value[CO_INPUTS];                   /* (the game side is one thread at a time: co.game) */
    static uint8_t have[CO_INPUTS], cmd_have[CO_AS_SLOTS];
    static char want[CO_AS_SLOTS][CO_AS_NAME];
    static struct { uint64_t actor; char name[CO_AS_NAME]; int64_t at; } tried[CO_PEERS][CO_AS_SLOTS];   /* (a command the body's graph does not have is not asked for every frame) */
    uint32_t words = 0;
    const uint64_t at = co_inputs_at(actor, &words);
    if (!at) return;
    EnterCriticalSection(&co.lock);
    const int same = co_in_words[peer] == words;
    if (same) {
        memcpy(value, co_in_value[peer], words * 4), memcpy(have, co_in_have[peer], words);
        memcpy(want, co_cmd_name[peer], sizeof want), memcpy(cmd_have, co_cmd_have[peer], sizeof cmd_have);
    }
    LeaveCriticalSection(&co.lock);
    if (!same) memset(have, 0, sizeof have), memset(cmd_have, 0, sizeof cmd_have), memset(want, 0, sizeof want);   /* (their graph is not this body's, or they have said nothing yet: nothing of theirs fits it. A hit still shows) */
    for (uint32_t i = 0; i < words; i++)
        if (have[i] && !co_input_is_address(at + i * 4ULL)) *(uint32_t *)GP(at + i * 4ULL) = value[i];
    HOS_WRITTEN(GP(at), words * 4ULL);
    char names[CO_AS_SLOTS][CO_AS_NAME];
    const int slots = co_as_names(actor, names);
    const int64_t now = hos_now_ns();
    if (now < co_flinch_until[peer]) snprintf(want[0], CO_AS_NAME, "%s", co_flinch_hard[peer] ? "DamageM" : "DamageS"), cmd_have[0] = 1;   /* (PvP: I just hit them) */
    if (slots > 0 && cmd_have[0] && !want[0][0]) snprintf(want[0], CO_AS_NAME, "Wait");   /* (no body animation at all is arms held out: stand instead) */
    for (int i = 0; i < slots; i++) {
        if (!cmd_have[i] || !strcmp(want[i], names[i])) continue;
        if (!want[i][0]) { co_as_remove(c, actor, i); continue; }
        if (tried[peer][i].actor == actor && !strcmp(tried[peer][i].name, want[i]) && now - tried[peer][i].at < 400000000LL) continue;
        tried[peer][i].actor = actor, tried[peer][i].at = now;
        memcpy(tried[peer][i].name, want[i], CO_AS_NAME);
        co_as_play(c, actor, i, want[i]);
    }
    /* (The body plays nothing: its player's game said nothing yet, or said a command this graph does not take, as in a
       cutscene. It stands rather than hold its arms out.) */
    static int64_t stood[CO_PEERS];
    if (slots > 0 && !names[0][0] && now - stood[peer] > 500000000LL &&
        (!cmd_have[0] || !strcmp(want[0], "Wait") || (tried[peer][0].actor == actor && now - tried[peer][0].at > 150000000LL))) {
        stood[peer] = now;
        co_as_play(c, actor, 0, "Wait");
    }
}

/* ---- PvP ----
   On when both players have the Co-op tab's PvP row on. The one who swings decides the hit: when Link starts an
   attack (slot 0 goes to a command named Cut..., but not the ones after a swing, or ChargeAttack) every other player
   in reach in front of him, a moment into the swing, is told "D <their number> <quarter hearts>". The one who is told
   takes it off their own hearts (Link's life: a count of quarter hearts at [[player+7184]+8], the value the heart
   protection in nx2/oot_patches.py guards; player is the object 0x1d50 before Link's movement component), once for a
   swing, and not more often than every 0.6 s.
   A hit is seen to land: the one who swings sees the other's body flinch at once (DamageS, or DamageM for the heavy
   swings: Link's own hit animations, played on the body until its player's game takes over); the one who is hit has
   his own Link play the same, and is pushed back from the one who hit him, a metre over a quarter of a second (the
   controller's change-position, as "go to a player" uses). His game says that animation to everyone like any other. */
static struct { int64_t at; int quarters, swing, shot; int told[CO_PEERS]; } co_swing;
static int co_body_is_foe(int i);
static int co_camera(float m[12], float scale[2]);

static int co_attack_quarters(const char *name) {
    if (!strncmp(name, "ChargeAttack", 12)) return 4;
    if (strncmp(name, "Cut", 3) || strstr(name, "After") || strstr(name, "Land") || strstr(name, "Pattern")) return 0;
    const size_t n = strlen(name);
    if (n >= 2 && !strcmp(name + n - 2, "Ed")) return 0;
    return strstr(name, "Jump") || strstr(name, "Hammer") ? 4 : 2;
}

static uint64_t co_life_at(Ctx *c, uint64_t link) {
    int hos_oot_is_player(Ctx *c, uint64_t actor);       /* hos_main.c */
    const uint64_t parts = hos_guest_range(link + 624, 8) ? G64(link + 624) : 0;
    const uint64_t move = hos_guest_range(parts + 800, 8) ? G64(parts + 800) : 0;
    if (move < HOS_HEAP_BASE + 0x1d50 || !hos_oot_is_player(c, move - 0x1d50)) return 0;
    const uint64_t holder = hos_guest_range(move - 0x1d50 + 7184, 8) ? G64(move - 0x1d50 + 7184) : 0;
    return hos_guest_range(holder, 16) ? holder + 8 : 0;
}

static void co_pvp_step(Ctx *c, uint64_t link, int64_t now) {
    const int on = rx_option(HOS_OPT_COOP_PVP, 0) > 0;
    /* mine: did a swing start, and who is in its way */
    static char was[CO_AS_NAME];
    char names[CO_AS_SLOTS][CO_AS_NAME];
    if (co_as_names(link, names) > 0 && strcmp(was, names[0])) {
        memcpy(was, names[0], CO_AS_NAME);
        /* (a seed or an arrow let go: Link's body goes to BowShoot, the slingshot's too. It reaches the player the camera
           looks at: the shot goes where the picture's middle is) */
        const int shot = !strcmp(names[0], "BowShoot");
        const int quarters = shot ? 2 : co_attack_quarters(names[0]);
        if (quarters && on) co_swing.at = now, co_swing.quarters = quarters, co_swing.shot = shot, co_swing.swing++, memset(co_swing.told, 0, sizeof co_swing.told);
    }
    if (on && co_swing.at && now - co_swing.at > 120000000LL && now - co_swing.at < 450000000LL) {
        float mine[3], yaw, view[12], lens[2], eye[3] = {0, 0, 0}, ahead[3] = {0, 0, 0};
        const int camera = co_swing.shot && co_camera(view, lens);
        if (camera) {                                   /* (the view matrix takes the world into the camera's space, which looks down -z) */
            for (int k = 0; k < 3; k++) eye[k] = -(view[k] * view[3] + view[4 + k] * view[7] + view[8 + k] * view[11]), ahead[k] = -view[8 + k];
        }
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine), yaw = co.yaw;
        for (int i = 0; i < CO_PEERS; i++) {
            const CoPeer *p = &co.peer[i];
            if (!p->used || co_swing.told[i] || !p->map[0] || strcmp(p->map, co_map) || now - p->seen > 1500000000LL) continue;
            if (p->busy) continue;                      /* (in a cutscene or talking: left alone) */
            if (co_body_is_foe(i) && !co_swing.shot) continue;   /* (a real target: the game's own combat says what a swing reached, co_bodies_step; a seed or an arrow is still judged by its line too: the sensor is narrow) */
            const float dx = p->pos[0] - mine[0], dy = p->pos[1] - mine[1], dz = p->pos[2] - mine[2];
            const float apart = sqrtf(dx * dx + dz * dz);
            if (co_swing.shot) {                        /* (the shot's line: from the camera, the way it looks, past Link) */
                if (!camera) continue;
                const float t[3] = {p->pos[0] - eye[0], p->pos[1] + (p->child ? 0.55f : 0.95f) - eye[1], p->pos[2] - eye[2]};
                const float along = t[0] * ahead[0] + t[1] * ahead[1] + t[2] * ahead[2];
                const float off2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2] - along * along;
                const float wide = 0.45f + 0.012f * along;   /* (a body's width, and a little more with the distance) */
                if (along < 1.5f || along > 45.0f || off2 > wide * wide) continue;
            } else {
            if (apart > 1.9f || fabsf(dy) > 1.5f) continue;
            if (apart > 0.3f && (dx * sinf(yaw) + dz * cosf(yaw)) / apart < 0.25f) continue;   /* (not in front: some 75 degrees either side) */
            }
            co_swing.told[i] = 1;
            co_flinch_until[i] = now + 450000000LL, co_flinch_hard[i] = co_swing.quarters >= 4;
            const int id = p->id;
            char name[24];
            snprintf(name, sizeof name, "%s", p->name);
            LeaveCriticalSection(&co.lock);
            co_send("D %d %d", id, co_swing.quarters);
            co_note("You hit %s", name);
            hos_log("coop: pvp: my %s reached %s (%d quarter hearts)", co_swing.shot ? "shot" : "swing", name, co_swing.quarters);
            hos_log_flush();
            EnterCriticalSection(&co.lock);
        }
        LeaveCriticalSection(&co.lock);
    }
    /* theirs: what is to come off my hearts */
    static int64_t last_hit, push_until, calm_until;
    static float push[2];
    /* (Not in a cutscene, a conversation or a loading, nor for a moment after: a swing that reaches a player during
       a scene takes hearts, plays the hit on a Link the scene is moving and pushes him out of his place, and the scene
       never ends.) */
    if (co_game_busy()) calm_until = now + 1500000000LL, push_until = 0;
    if (now < calm_until) InterlockedExchange(&co_hurt, 0);
    if (now < push_until) {                             /* (pushed back: a step each frame) */
        float at[3];
        EnterCriticalSection(&co.lock);
        memcpy(at, co.pos, sizeof at);
        LeaveCriticalSection(&co.lock);
        at[0] += push[0] * 0.07f, at[2] += push[1] * 0.07f;
        co_place(c, link, at);
    }
    const int hurt = (int)InterlockedExchange(&co_hurt, 0);
    if (hurt > 0 && on && now - last_hit > 600000000LL) {
        const uint64_t at = co_life_at(c, link);
        if (at) {
            const uint32_t life = G32(at), next = life > (uint32_t)hurt ? life - (uint32_t)hurt : 0;
            *(uint32_t *)GP(at) = next;
            HOS_WRITTEN(GP(at), 4);
            last_hit = now;
            if (life && !next) {                        /* (that was the last of my hearts: everyone is told who did it) */
                co_send("O %d", (int)co_hurt_from);
                co_note("%s defeated you", co_hurt_by[0] ? co_hurt_by : "A player");
            }
            co_as_play(c, link, 0, hurt >= 4 ? "DamageM" : "DamageS");
            {   /* away from the one who swung */
                float mine[3], dx = 0, dz = 0;
                EnterCriticalSection(&co.lock);
                memcpy(mine, co.pos, sizeof mine);
                const CoPeer *by = co_peer((int)co_hurt_from, 0);
                if (by) dx = mine[0] - by->pos[0], dz = mine[2] - by->pos[2];
                LeaveCriticalSection(&co.lock);
                const float size = sqrtf(dx * dx + dz * dz);
                if (size > 0.05f) push[0] = dx / size, push[1] = dz / size, push_until = now + 250000000LL;
            }
            co_note("%s hit you", co_hurt_by[0] ? co_hurt_by : "A player");
            hos_log("coop: pvp: hit by %s: hearts %u -> %u (quarters)", co_hurt_by, life, next);
            hos_log_flush();
        }
    }
}

static int co_body_is_foe(int i) { return i >= 0 && i < CO_PEERS && co_body[i].actor && co_body[i].foe; }

/* ---- what a body holds ----
   The game's own "give this character this equipment" (the script node OneShotEquipmentUserRequestCreateDynamicEquipment,
   run function f_828f1880) is f_82e17b20(equipment user, slot, &name, a number, -1, 1, 0): the equipment user is the
   actor's component at [actor+0x270]+0x200, the slot a number under 18 (the node's slots 1..6 are 1, 0, 5, 6, 11, 12
   by its table at 0x83f0a928). Its "change state" (OneShotEquipmentUserRequestChangeState, f_828f1670) is f_806d6820(equipment user, state)
   for the node's kind 1 and f_80e54c70(equipment user, state) for kind 2. */
static uint64_t co_equipment_user(uint64_t actor) {
    const uint64_t parts = hos_guest_range(actor + 0x270, 8) ? G64(actor + 0x270) : 0;
    const uint64_t user = hos_guest_range(parts + 0x200, 8) ? G64(parts + 0x200) : 0;
    return hos_guest_range(user, 0x20) ? user : 0;
}

static int co_equip(Ctx *c, uint64_t actor, int slot, const char *name, int number) {
    const uint64_t user = co_equipment_user(actor);
    A64Fn make = a64_lookup(0x82e17b20ULL);
    if (!user || !make || slot < 0 || slot >= 18 || strlen(name) >= 0x40) return -1;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x100) & ~0xfULL;
    memset(A64_PTR(frame), 0, 0x100);
    memcpy(A64_PTR(frame + 0x40), name, strlen(name) + 1);
    recomp_store64_sp(c, frame + 0x20, frame + 0x40);
    c->x[31] = frame;
    c->x[0] = user, c->x[1] = (uint64_t)slot, c->x[2] = frame + 0x20, c->x[3] = (uint64_t)(uint32_t)number, c->x[4] = 0xffffffffULL, c->x[5] = 1, c->x[6] = 0;
    make(c);
    const int result = (int)c->x[0];
    *c = saved;
    return result;
}

static int co_equip_state(Ctx *c, uint64_t actor, int kind, int slot) {
    const uint64_t user = co_equipment_user(actor);
    A64Fn change = a64_lookup(kind == 2 ? 0x80e54c70ULL : 0x806d6820ULL);
    if (!user || !change) return -1;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x40) & ~0xfULL;
    c->x[0] = user, c->x[1] = (uint64_t)(uint32_t)slot;
    change(c);
    const int result = (int)c->x[0];
    *c = saved;
    return result;
}

/* (every frame) a body takes what that player's Link holds and works through the reaches it has missed, as the body
   takes what it plays: the game's own equipment calls, on the body's user. A hold a body already has is not asked
   for again; a reach is an event, so the body is asked for it once, in the order it happened. */
static void co_equip_put(Ctx *c, uint64_t actor, CoBody *b, int peer) {
    char want[CO_EQ_SLOTS][CO_AS_NAME];
    int num[CO_EQ_SLOTS];
    CoEquipEvent evt[CO_EQ_EVENTS];
    int n = 0;
    EnterCriticalSection(&co.lock);
    memcpy(want, co_eq_name[peer], sizeof want), memcpy(num, co_eq_num[peer], sizeof num);
    while (co_eq_evt_head[peer] != co_eq_evt_tail[peer]) {
        if (n < CO_EQ_EVENTS) evt[n++] = co_eq_evt[peer][co_eq_evt_head[peer]];
        co_eq_evt_head[peer] = (co_eq_evt_head[peer] + 1) % CO_EQ_EVENTS;
    }
    LeaveCriticalSection(&co.lock);
    for (int i = 0; i < n; i++) {                      /* (the reaches first: they say where the sword and the shield sit) */
        const int got = co_equip_state(c, actor, evt[i].kind, evt[i].slot);
        if (co_eq_put_said < 400) {                    /* (a reach asked for and what the game's own call made of it:
                                                            -1 means this body had no equipment user to ask) */
            co_eq_put_said++;
            hos_log("coop: body %llx reach %d at %d: %d", (unsigned long long)actor, evt[i].kind, evt[i].slot, got);
        }
    }
    for (int s = 0; s < CO_EQ_SLOTS; s++) {
        if (!want[s][0] || !strcmp(b->eq_name[s], want[s])) continue;
        snprintf(b->eq_name[s], CO_AS_NAME, "%s", want[s]), b->eq_num[s] = num[s];
        const int got = co_equip(c, actor, s, want[s], num[s]);
        if (co_eq_put_said < 400) {                  /* (ditto: a hold asked for and what the game's own make said;
                                                        -1: no equipment user, or no room for it) */
            co_eq_put_said++;
            hos_log("coop: body %llx slot %d holds '%s' %d: %d", (unsigned long long)actor, s, want[s], num[s], got);
        }
    }
}

/* (nx2/oot_patches.py: at the head of each of the three equipment user's calls, on whatever thread the game made them)
   What reaches my Link's user is what the others must draw; said once, as it changes. Their bodies hear it. */
void oot_coop_equip_seen(Ctx *c, int kind) {
    if (!co.ready || co.links <= 0 || (unsigned)kind > 2u) return;
    const uint64_t user = co.my_user;
    static int first, first_mine;                  /* (once each per run: did my game's own equipment calls reach here
                                                        at all (no line: the build has no equipment hooks), and is the
                                                        user in one my Link's? where this says no is where a swing
                                                        shows up empty on the other side) */
    if (!first) {
        first = 1;
        hos_log("coop: equip: the game's equipment calls are seen (%s, kind %d): the call's user %llx, my Link's %llx",
                kind ? "a hang" : "a thing", kind, (unsigned long long)c->x[0], (unsigned long long)user);
        hos_log_flush();
    }
    if (!first_mine && user && c->x[0] == user) {
        first_mine = 1;
        hos_log("coop: equip: that is my Link's equipment user");
        hos_log_flush();
    }
    if (!user || c->x[0] != user) return;          /* (not my Link's equipment user: an enemy's, a body's, nobody's) */
    char line[96] = "";
    EnterCriticalSection(&co.lock);
    if (kind == 0) {
        const int slot = (int)(int32_t)c->x[1];
        const uint64_t at = co_ok(c->x[2], 8) ? G64(c->x[2]) : 0;   /* (the game passes a pointer to the name) */
        char name[CO_AS_NAME] = "";
        if (at && co_ok(at, CO_AS_NAME)) {
            const unsigned char *p = (const unsigned char *)GP(at);
            int i = 0;
            while (i < CO_AS_NAME - 1 && p[i] >= 0x21 && p[i] <= 0x7e) name[i] = p[i], i++;
            if (i > 0 && !p[i] && slot >= 0 && slot < CO_EQ_SLOTS) {
                const int num = (int)(int32_t)c->x[3];
                if (strcmp(co_said_name[slot], name) || co_said_num[slot] != num) {
                    snprintf(co_said_name[slot], CO_AS_NAME, "%s", name), co_said_num[slot] = num;
                    snprintf(line, sizeof line, "E %d %s %d", slot, name, num);
                }
            }
        }
    } else {
        const int slot = (int)(int32_t)c->x[1];       /* (the game's own call takes the slot, and calls it again every
                                                            time its Link reaches for or puts away the thing) */
        const int64_t now = hos_now_ns();
        if (slot >= 0 && slot < CO_EQ_SLOTS && now - co_said_hang_ns[kind][slot] >= 400000000LL)
            co_said_hang_ns[kind][slot] = now, snprintf(line, sizeof line, "S %d %d", kind, slot);
    }
    LeaveCriticalSection(&co.lock);
    if (line[0]) {
        co_send("%s", line);
        if (co_eq_put_said < 400) {                  /* (the line as the room will see it; the receiver logs what its
                                                        bodies do with it) */
            co_eq_put_said++;
            hos_log("coop: said to the room: %s", line), hos_log_flush();
        }
    }
}

/* Someone joined: what my Link holds goes to them whole. The reaches are not said again — they are things that
   happened, and a new body catches up with the next one my Link makes. */
static void co_equip_say_again(void) {
    char names[CO_EQ_SLOTS][CO_AS_NAME];
    int num[CO_EQ_SLOTS];
    EnterCriticalSection(&co.lock);
    memcpy(names, co_said_name, sizeof names), memcpy(num, co_said_num, sizeof num);
    LeaveCriticalSection(&co.lock);
    for (int s = 0; s < CO_EQ_SLOTS; s++) if (names[s][0]) co_send("E %d %s %d", s, names[s], num[s]);
}

/* (test runs) what Link plays, said to the log each time it changes; and a "A <slot> <command>" line plays one on the bodies */
static void co_as_test(Ctx *c, uint64_t link, int allowed) {
    if (!allowed) return;
    static char was[CO_AS_SLOTS][CO_AS_NAME];
    static int said;
    char now[CO_AS_SLOTS][CO_AS_NAME];
    memset(now, 0, sizeof now);
    const int n = co_as_names(link, now);
    if (n && memcmp(was, now, sizeof was) && said < 400) {
        char line[CO_AS_SLOTS * (CO_AS_NAME + 6)];
        size_t used = 0;
        for (int i = 0; i < n; i++) if (now[i][0]) used += (size_t)snprintf(line + used, sizeof line - used, " %d:%s", i, now[i]);
        said++;
        hos_log("coop: test: Link plays (%d slots)%s", n, used ? line : " nothing");
        hos_log_flush();
        memcpy(was, now, sizeof was);
    }
}

/* ---- the name over another player ----
   The menu draws a small bubble with the name (gpu/vk/vk_menu.cpp, the list's n-th player); this says where on the
   screen, from the game's own camera. The camera the game plays through (hos_main.c oot_gameplay_view: the first of
   the camera manager's) has its view at +120: a 3x4 matrix at +8, the world into the camera's space, looking down
   -z. Its projection is at +128: a 4x4 at +0xc whose [0][0] and [1][1] are 1/(aspect tan(fov/2)) and 1/tan(fov/2)
   (read in play: 1.2063 and 2.1445, for 50 degrees at 16:9). So a point v in the camera's space is on the screen at
   x = [0][0] v.x / -v.z, y = [1][1] v.y / -v.z, each from -1 to 1.
   One number a label: the list's place << 28, shown << 26, x << 13, y, x and y in 8191ths of the picture. Said again
   at least ten times a second: the menu takes a label away that has not been said for a moment (the pause menu, a
   cutscene, a map load: Link's update, where this runs, stops). */
static int co_camera(float m[12], float scale[2]) {
    const uint64_t holder = G64(0x84ca6bc0ULL);
    const uint64_t mgr = hos_guest_range(holder, 8) ? G64(holder) : 0;
    if (!hos_guest_range(mgr, 48) || !G32(mgr + 32)) return 0;
    const uint64_t list = G64(mgr + 40);
    const uint64_t camera = hos_guest_range(list, 8) ? G64(list) : 0;
    if (!hos_guest_range(camera, 136)) return 0;
    const uint64_t view = G64(camera + 120), lens = G64(camera + 128);
    if (!hos_guest_range(view, 0x38) || G64(view) != 0x84c18a88ULL || !hos_guest_range(lens, 0x24) || G64(lens) != 0x84c18c70ULL) return 0;
    memcpy(m, GP(view + 8), 48);
    memcpy(&scale[0], GP(lens + 0x0c), 4);
    memcpy(&scale[1], GP(lens + 0x20), 4);
    return scale[0] > 0.05f && scale[0] < 50.0f && scale[1] > 0.05f && scale[1] < 50.0f;
}

static void co_label(int order, int shown, const float m[12], const float scale[2], const float place[3], int child, int64_t now) {
    static int was[CO_PEERS];
    static int64_t said[CO_PEERS];
    if (order < 0 || order >= CO_PEERS) return;
    int packed = order << 28;
    if (shown) {
        const float x = place[0], y = place[1] + (child ? 1.32f : 1.98f), z = place[2];   /* (over the head) */
        const float vx = m[0] * x + m[1] * y + m[2] * z + m[3], vy = m[4] * x + m[5] * y + m[6] * z + m[7];
        const float vz = m[8] * x + m[9] * y + m[10] * z + m[11];
        if (vz < -0.4f && vz > -60.0f) {                /* (in front of the camera, and near enough to be read) */
            const float sx = scale[0] * vx / -vz, sy = scale[1] * vy / -vz;
            if (fabsf(sx) < 1.0f && fabsf(sy) < 1.0f)
                packed |= 1 << 26 | (int)((sx * 0.5f + 0.5f) * 8191.0f) << 13 | (int)((0.5f - sy * 0.5f) * 8191.0f);
        }
    }
    if (packed == was[order] && now - said[order] < 100000000LL) return;
    was[order] = packed, said[order] = now;
    rx_option_set(HOS_OPT_COOP_LABEL, packed);
}

static void co_bodies_step(Ctx *c, uint64_t link, int64_t now) {
    if (link != co_link_actor) {                       /* another map: the game took the bodies with the old one */
        co_link_actor = link;
        memset(co_body, 0, sizeof co_body);
    }
    if (co_test_go[0]) {                               /* (test runs: "G <map>") */
        float to[3];
        float away = 0, face[2];
        if (co_test_go[0] == '>' && sscanf(co_test_go + 1, "%f %f %f %f %f", &to[0], &to[1], &to[2], &face[0], &face[1]) == 5) {   /* ("G >x y z tx tz": stood at a place, turned to face a point) */
            float at[12];
            memcpy(at, to, sizeof to);
            co_turn(atan2f(face[0] - to[0], face[1] - to[2]), at + 3);
            const int placed = co_place(c, link, to);
            hos_log("coop: test: stood at %.1f %.1f %.1f (%d) and turned to face %.1f %.1f (%d)", to[0], to[1], to[2], placed, face[0], face[1], co_actor_move(c, link, at));
        } else
        if (co_test_go[0] == '^' && sscanf(co_test_go + 1, "%f %f %f %f", &to[0], &to[1], &to[2], &away) == 4) {
            EnterCriticalSection(&co.lock);
            const float yaw = co.yaw;
            LeaveCriticalSection(&co.lock);
            to[0] -= sinf(yaw) * away, to[2] -= cosf(yaw) * away;
            hos_log("coop: test: stood at %.1f %.1f %.1f, %.1f from the point and facing it (%d)", to[0], to[1], to[2], away, co_place(c, link, to));
        } else
        if (co_test_go[0] == '@' && sscanf(co_test_go + 1, "%f %f %f", &to[0], &to[1], &to[2]) == 3) hos_log("coop: test: stood at %.1f %.1f %.1f (%d)", to[0], to[1], to[2], co_place(c, link, to));
        else hos_log("coop: test: travel to %s (%d)", co_test_go, hos_oot_travel(c, link, co_test_go));
        co_test_go[0] = 0;
    }
    if (InterlockedExchange(&co_test_spawn, 0)) {      /* (test runs: HOS_COOP_TEST=1 and a "Z <actor> <option>" line) */
        float at[3];
        EnterCriticalSection(&co.lock);
        memcpy(at, co.pos, sizeof at);
        const float yaw = co.yaw;
        LeaveCriticalSection(&co.lock);
        at[0] += 2.0f * sinf(yaw), at[2] += 2.0f * cosf(yaw);
        const uint64_t actor = co_create(c, co_test_name, at, yaw, co_test_option);
        hos_log("coop: test: %s made at %.2f %.2f %.2f (option %d): actor %llx, vtable %llx", co_test_name, at[0], at[1], at[2], co_test_option,
                (unsigned long long)actor, (unsigned long long)(actor ? G64(actor) : 0));
        hos_log_flush();
    }
    struct { int want, child, order, foe, id, busy; float place[3], yaw; char name[24]; } who[CO_PEERS];
    /* (CoopFoe_*: the same body with a life and the sensor that takes hits, made by make_bodies.py: a target for all the game
       can hit an enemy with. Used while PvP is on here and there; with it off anywhere the body is CoopPlayer_*, which
       nothing can hit or lock on to.) */
    static int foes = -1, own = -1;
    if (foes < 0 || own < 0) {
        int hos_mods_has(const char *prefix, const char *suffix);   /* mods.c */
        foes = hos_mods_has("pack/actor/coopfoe_child", ".pack.zs") && hos_mods_has("pack/actor/coopfoe_adult", ".pack.zs");
        own = hos_mods_has("pack/actor/coopplayer_child", ".pack.zs") && hos_mods_has("pack/actor/coopplayer_adult", ".pack.zs");
        hos_log("coop: PvP %s", foes ? "by the game's own combat (the other players' bodies are real targets)" : "by reach (no target bodies installed)");
        hos_log("coop: the other players are drawn as %s", own ? "CoopPlayer (they move as they play)" : "PouchPlayer (no co-op bodies installed: they stand and slide)");
    }
    const int pvp_here = rx_option(HOS_OPT_COOP_PVP, 0) > 0;
    int listed = 0;
    static int64_t stepped;
    const float dt = stepped ? (float)(now - stepped) * 1e-9f : 0.016f;
    stepped = now;
    (void)dt;
    float view[12], lens[2];
    const int camera = co_camera(view, lens);
    EnterCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS; i++) {
        const CoPeer *p = &co.peer[i];
        who[i].order = p->used ? listed++ : -1;        /* (their place in the menu's list: co_status counts the same way) */
        who[i].want = p->used && p->map[0] && !strcmp(p->map, co_map) && now - p->seen < 3000000000LL;
        if (!who[i].want) continue;
        /* between the last two places said, then on along the same line for a moment: what was said is a thirtieth
           of a second old when it comes, and the next has not come yet */
        const float span = (float)(p->seen - p->seen_before) * 1e-9f, since = (float)(now - p->seen) * 1e-9f;
        const float t = span > 0.005f && span < 0.5f ? (since > 0.15f ? 0.15f : since) / span : 0.0f;
        for (int k = 0; k < 3; k++) who[i].place[k] = p->pos[k] + (p->pos[k] - p->was[k]) * t;
        who[i].yaw = p->yaw + remainderf(p->yaw - p->was_yaw, 6.2831853f) * t;
        who[i].child = p->child;
        who[i].foe = foes && pvp_here && p->pvp, who[i].id = p->id, who[i].busy = p->busy;
        snprintf(who[i].name, sizeof who[i].name, "%s", p->name);
    }
    LeaveCriticalSection(&co.lock);
    for (int i = 0; i < CO_PEERS; i++) {
        CoBody *b = &co_body[i];
        if (b->actor && (!hos_guest_range(b->actor, 0x360) || G64(b->actor) != b->made_of)) b->actor = 0;   /* (gone, or its memory is something else now) */
        /* (A puppet's age comes over the wire, and the flag it comes from can flicker; and a body put under the world
           is not destroyed — it stays in the game's memory and, left alone, its own walk-up stands it back on the
           ground beside the new one. So only change age on what has held for half a second, and sink the one going
           under in every step, so two of a player never stand in a room at once.) */
        const int flip_foe = b->actor && who[i].want && b->foe != (own && who[i].foe);   /* (the tab's PvP row: a switch you made, act on it; without the target packs there is no foe body to turn to) */
        if (who[i].child != b->child_want) b->child_at = 0, b->child_want = who[i].child;
        else if (who[i].child != b->child && !b->child_at) b->child_at = now;
        const int flip_child = flip_foe == 0 && b->actor && who[i].want && who[i].child != b->child && b->child_at && now - b->child_at > 500000000LL;
        if (flip_foe || flip_child) {   /* (they changed age, or PvP went on or off: this body goes under, the other is made) */
            const float under[3] = {0, -5000, 0};
            co_body_put(b->actor, under, 0);
            b->retired = b->actor, b->retired_of = b->actor ? G64(b->actor) : 0;   /* (kept under and watched: the game may take that memory, and its actor is not this one any more) */
            b->actor = 0;
            b->child = who[i].child, b->child_at = 0;   /* (the body that comes next shows what we know now) */
            hos_log("coop: %s's body turns %s: the old one (%llx) goes under", who[i].name, flip_foe ? "PvP" : "age", (unsigned long long)b->retired);
            hos_log_flush();
        }
        if (b->retired && hos_guest_range(b->retired, 0x360) && G64(b->retired) == b->retired_of) {
            const float under[3] = {0, -5000, 0};
            co_body_put(b->retired, under, 0);
        }
        if (!b->actor && who[i].want && now - b->tried > 3000000000LL) {
            b->tried = now;
            /* (CoopPlayer_*: Link's model with Link's animation graph, made by nx2/coop_bodies.py. Without that
               mod the pause menu's Link is used, as before: it stands and slides, it cannot be animated.) */
            const char *name = own && who[i].foe ? (who[i].child ? "CoopFoe_Child" : "CoopFoe_Adult")
                             : own ? (who[i].child ? "CoopPlayer_Child" : "CoopPlayer_Adult") : (who[i].child ? "PouchPlayer_Child" : "PouchPlayer_Adult");
            b->actor = co_create(c, name, who[i].place, who[i].yaw, 0);
            b->made_of = b->actor ? G64(b->actor) : 0, b->child = who[i].child, b->posed = 0;
            b->child_want = who[i].child, b->child_at = 0;
            b->foe = own && who[i].foe, b->life_full = 0, b->struck = 0;
            memset(b->eq_name, 0, sizeof b->eq_name);
            hos_log("coop: %s drawn as %s: actor %llx", who[i].name, name, (unsigned long long)b->actor);
            hos_log_flush();
        }
        co_label(who[i].order, camera && b->actor && who[i].want, view, lens, who[i].place, who[i].child, now);
        if (!b->actor) continue;
        if (b->foe) {
            /* What my game's combat took off this body's life is what its player loses. The life is the actor's Life
               component's, where the game's own "get life" reads it (QueryGetLife, f_82a36880): [[actor+0x270]+0x2b8] the
               component, [[component+0x1c10]+8] the number, in the units an enemy's life is counted in (the Kokiri Sword
               takes 1, a jump attack 2). Put back at once, so the body never dies here: its player's game decides that.
               One unit is half a heart for a player. */
            const uint64_t parts = G64(b->actor + 0x270);
            const uint64_t life = co_ok(parts + 0x2b8, 8) ? G64(parts + 0x2b8) : 0;
            const uint64_t holder = co_ok(life + 0x1c10, 8) ? G64(life + 0x1c10) : 0;
            if (co_ok(holder, 16)) {
                const uint32_t left = G32(holder + 8);
                if (!b->life_full && left > 0 && left < 100000) b->life_full = left;
                if (b->life_full && left < b->life_full) {
                    const uint32_t taken = b->life_full - left;
                    *(uint32_t *)GP(holder + 8) = b->life_full;
                    HOS_WRITTEN(GP(holder + 8), 4);
                    if (!who[i].busy && now - b->struck > 250000000LL) {   /* (not in their cutscene; and one hit is one hit) */
                        const int quarters = taken >= 4 ? 8 : (int)taken * 2;
                        b->struck = now;
                        co_flinch_until[i] = now + 600000000LL, co_flinch_hard[i] = 1;
                        {   /* (knocked back from me, at once: their own game's push comes a moment later and takes over) */
                            float mine[3];
                            EnterCriticalSection(&co.lock);
                            memcpy(mine, co.pos, sizeof mine);
                            LeaveCriticalSection(&co.lock);
                            float dx = who[i].place[0] - mine[0], dz = who[i].place[2] - mine[2];
                            const float gap = sqrtf(dx * dx + dz * dz);
                            if (gap > 0.05f) dx /= gap, dz /= gap; else dx = 0, dz = 1;
                            b->push[0] = dx, b->push[1] = dz, b->push_far = quarters >= 4 ? 2.2f : 1.4f;
                        }
                        co_send("D %d %d", who[i].id, quarters);
                        co_note("You hit %s", who[i].name);
                        hos_log("coop: pvp: my hit took %u off %s's body: %d quarter hearts", taken, who[i].name, quarters);
                    }
                }
            }
        }
        if (i == 0 || co_test_play) {                   /* (test runs) */
            static int test = -1;
            if (test < 0) { const char *e = getenv("HOS_COOP_TEST"); test = e && !strcmp(e, "1"); }
            if (test && co_test_play) {
                char names[CO_AS_SLOTS][CO_AS_NAME];
                const int slots = co_as_names(b->actor, names);
                const int ok = co_as_play(c, b->actor, co_test_play_slot, co_test_play_name);
                char after[CO_AS_SLOTS][CO_AS_NAME];
                co_as_names(b->actor, after);
                hos_log("coop: test: body %d (%d slots, slot %d played '%s'): play '%s' -> %d, now '%s'", i, slots, co_test_play_slot,
                        slots > co_test_play_slot ? names[co_test_play_slot] : "?", co_test_play_name, ok,
                        slots > co_test_play_slot ? after[co_test_play_slot] : "?");
                hos_log_flush();
            }
        }
        if (who[i].want) {
            const float since_hit = b->struck ? (float)(now - b->struck) * 1e-9f : 9.0f;
            if (b->push_far > 0 && since_hit < 0.6f) {      /* (out fast, held, then back to where their game says they are) */
                const float k = since_hit < 0.12f ? since_hit / 0.12f : since_hit < 0.3f ? 1.0f : 1.0f - (since_hit - 0.3f) / 0.3f;
                const float out = b->push_far * (since_hit < 0.12f ? 1.0f - (1.0f - k) * (1.0f - k) : k);
                who[i].place[0] += b->push[0] * out, who[i].place[2] += b->push[1] * out;
            }
            co_body_put(b->actor, who[i].place, who[i].yaw);
            co_anim_put(c, b->actor, i);
            co_equip_put(c, b->actor, b, i);
            b->posed = 1;
        } else { const float under[3] = {0, -5000, 0}; co_body_put(b->actor, under, 0); }
    }
}

/* Is the game in the middle of something it must be left alone in: an event playing (a cutscene, a conversation,
   a chest opening: the event manager's own "an event is playing", as hos_main.c's teleport asks it, EventMgr +104),
   or a map loading (Sequence_IsLoadingBanc). What the others found and did is handed over once it is over: a new
   file that was given the host's whole progress in the middle of its opening scene did not get out of that scene. */
#define CO_EVENTMGR_SLOT 0x84c9e090ULL
static int co_game_busy(void) {
    const uint64_t holder = G64(CO_EVENTMGR_SLOT);
    const uint64_t events = hos_guest_range(holder, 8) ? G64(holder) : 0;
    if (hos_guest_range(events, 112) && G64(events + 104)) return 1;
    CoFlags f;
    const uint8_t *loading = co_flags_open(&f) ? co_flag_value(&f, 0xef2b60c2u, NULL) : NULL;
    return loading && (*loading & 1);
}

/* ---- one enemy for everyone ----
   Each game runs its own copy of an enemy. What makes it one enemy is that its life is one: a hit that takes life off it
   in one game takes the same off it in every game, and it dies for all when all the hits together have used it up.
   nx2/oot_patches.py has f_804bd8c0, the Life component's update (every actor with a life, each frame it is updated),
   call oot_coop_life_seen at its head and oot_coop_life_write where it takes life away. The component's actor is at +0x18 and
   its life at [+0x1c10]+8; an actor's name is at [+0x20] and its place at +796.
   The same enemy in two games: the same name, first seen at the same place (an enemy is made where the map puts it, in
   every game alike). Told as "U <name's hash> <x> <y> <z> <life taken>".
   A hit from another player is owed to the enemy until its next update, and given there the game's own way: the Life
   component's message handler (vtable +0x90, f_82e67f60) takes {-, kind 0x46030000, -, -> {amount, -, flags 1}} and adds
   the amount to what the update then applies to the life: a negative amount is damage, a positive one heals, and the
   life stays between 0 and its most (tried: -1000 left a Deku Baba at 0, +1000 at its full 2). A life
   written as 0 by hand does not kill: the enemy stood there with no life. An enemy far from Link is not updated: it
   takes what it is owed when Link comes near. What the update then takes off is not told back to the others (expect).
   Several of the game's threads run the life function at once: co_lives_lock. */
#define CO_LIVES 256
#define CO_LEAD_SLOTS 6            /* (an enemy's animation slots said and followed; its inputs' words) */
#define CO_LEAD_IN 96
#define CO_BLOWS 128
typedef struct {
    uint64_t life, actor; uint32_t hash, owed, expect; float born[3]; int64_t seen, expect_until;
    /* (in one place) */
    int64_t first, said, heard, put, noted;             /* first updated this time; my last word; the leader's last word; my last put; a command last asked */
    int heard_from, mine, placed;
    float lead[12], at[12];                             /* the leader's place and turn; where my copy is on its way there */
    char lead_cmd[CO_LEAD_SLOTS][CO_AS_NAME], tried_cmd[CO_LEAD_SLOTS][CO_AS_NAME], said_cmd[CO_AS_NAME];
    int64_t tried_at[CO_LEAD_SLOTS];
    uint8_t lead_slots, tried_bad[CO_LEAD_SLOTS];
    uint32_t lead_in[CO_LEAD_IN], lead_words, said_n;   /* the leader's animation inputs, and how many words they are */
    float lead_far;                                     /* how far the leader's Link is from it */
    int boss;                                           /* a boss: its place is left to each game */
    int64_t hit_at, hurt_at, claim_until;               /* my own attack last hit it; my hit last took life off it; I say it is mine (a boss) until then */
    int64_t nearer_since;                               /* since when my Link has been clearly nearer than the leader's */
} CoLife;
typedef struct { uint32_t hash, taken; float at[3]; int64_t since; } CoBlow;
static CoLife co_lives[CO_LIVES];
static CoBlow co_blows[CO_BLOWS];
static SRWLOCK co_lives_lock = SRWLOCK_INIT;
static volatile LONG co_lives_said;
#define CO_BLOW_KEPT 1200000000000LL            /* (a hit on an enemy this game has not loaded: kept twenty minutes) */

/* (the net's lines, taken on the game's side) another player's hit */
static void co_enemy_hit(uint32_t hash, const float at[3], uint32_t taken) {
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&co_lives_lock);
    for (int i = 0; i < CO_LIVES; i++) {            /* (an enemy this game knows: owed to it) */
        CoLife *e = &co_lives[i];
        const float dx = at[0] - e->born[0], dy = at[1] - e->born[1], dz = at[2] - e->born[2];
        if (!e->life || e->hash != hash || dx * dx + dy * dy + dz * dz > 6.25f) continue;
        e->owed += taken;
        ReleaseSRWLockExclusive(&co_lives_lock);
        hos_log("coop: enemy %x: another player's hit took %u (given at its next update)", hash, taken);
        return;
    }
    CoBlow *b = &co_blows[0];                           /* (not loaded here: kept for when it is) */
    for (int i = 0; i < CO_BLOWS; i++) {
        if (!co_blows[i].taken || now - co_blows[i].since > CO_BLOW_KEPT) { b = &co_blows[i]; break; }
        if (co_blows[i].since < b->since) b = &co_blows[i];
    }
    b->hash = hash, b->taken = taken, b->since = now;
    memcpy(b->at, at, sizeof b->at);
    ReleaseSRWLockExclusive(&co_lives_lock);
}

/* The game's own life message: so much taken off this life at its update. */
static int co_enemy_damage(Ctx *c, uint64_t life, uint32_t amount) {
    const uint64_t vtable = G64(life);
    A64Fn tell = co_ok(vtable, 0x98) ? a64_lookup(G64(vtable + 0x90)) : NULL;
    if (!tell) return 0;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x80) & ~0xfULL;
    memset(A64_PTR(frame), 0, 0x80);
    recomp_store32_sp(c, frame + 0, 0xffffffffu);
    recomp_store32_sp(c, frame + 4, 0x46030000u);
    recomp_store32_sp(c, frame + 8, 0xffffffffu);
    recomp_store64_sp(c, frame + 0x10, frame + 0x20);
    recomp_store32_sp(c, frame + 0x20, (uint32_t)-(int32_t)(amount > 100000 ? 100000 : amount));
    recomp_store32_sp(c, frame + 0x28, 1);
    c->x[31] = frame;
    c->x[0] = life, c->x[1] = frame;
    tell(c);
    *c = saved;
    return 1;
}

/* ---- one enemy, in one place ----
   Every game still has its own copy of an enemy, and one of them leads it: its game alone decides where the enemy goes
   and what it does, and says so fifteen times a second:
       "Q <name's hash> <x> <y> <z where it was made> <how far the leader's Link is from it> <its place and turn: 12
          floats, base64> <what each animation slot plays: names with commas between, - for none>
          [<words>:<its animation inputs, base64>]"
   (the inputs with every third line: the values its blends run on, as "how another player moves" has them for Link).
   The other games move their copy there by the game's own request (co_actor_move), have it play the same commands
   and give it the same inputs, every frame it is updated, over whatever their own copy's mind would do.
   Who leads: a game that starts updating an enemy listens for 0.4 s first. If somebody is saying it, they lead it and
   this game follows; if nobody is, this game leads. After that the nearest player's game leads, so the enemy fights
   whoever is closest as it does alone: a follower whose Link has been two metres nearer than the leader's for half a
   second takes it, and a leader that hears another say it from a metre nearer lets go (within a metre of each other,
   the lower number keeps it). A leader that stops saying it (walked off, left the map) is followed for 0.7 s more,
   then the next game takes it from where it stands.
   All of it is done inside the enemy's own update (the life function's head), so the actor is there. */
static void co_enemy_led(int from, const char *text) {     /* (the net thread) */
    unsigned hash = 0;
    float born[3] = {0, 0, 0}, lead[12], far_off = 0;
    char packed[80] = "", cmds[CO_LEAD_SLOTS * CO_AS_NAME + 8] = "", inputs[700] = "";
    if (sscanf(text, "%x %f %f %f %f %79s %295s %699s", &hash, &born[0], &born[1], &born[2], &far_off, packed, cmds, inputs) < 7) return;
    if (co_unbase64(packed, (uint8_t *)lead, sizeof lead) != sizeof lead) return;
    for (int i = 0; i < 12; i++) if (!(lead[i] > -1e6f && lead[i] < 1e6f)) return;
    char cmd[CO_LEAD_SLOTS][CO_AS_NAME];
    int slots = 0;
    memset(cmd, 0, sizeof cmd);
    for (char *at = cmds; at && *at && slots < CO_LEAD_SLOTS; slots++) {
        char *end = strchr(at, ',');
        const size_t n = end ? (size_t)(end - at) : strlen(at);
        if (n < CO_AS_NAME && !(n == 1 && at[0] == '-')) memcpy(cmd[slots], at, n);
        at = end ? end + 1 : NULL;
    }
    uint32_t in[CO_LEAD_IN];
    unsigned words = 0;
    const char *colon = strchr(inputs, ':');
    if (colon && sscanf(inputs, "%u", &words) == 1 && words > 0 && words <= CO_LEAD_IN) {
        if (co_unbase64(colon + 1, (uint8_t *)in, sizeof in) != words * 4) words = 0;
    } else words = 0;
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&co_lives_lock);
    CoLife *best = NULL;
    float best_d = 6.25f;
    for (int i = 0; i < CO_LIVES; i++) {
        CoLife *e = &co_lives[i];
        if (!e->life || e->hash != hash) continue;
        const float dx = born[0] - e->born[0], dy = born[1] - e->born[1], dz = born[2] - e->born[2], d = dx * dx + dy * dy + dz * dz;
        if (d <= best_d) best = e, best_d = d;
    }
    if (best) {
        memcpy(best->lead, lead, sizeof lead);
        memcpy(best->lead_cmd, cmd, sizeof cmd), best->lead_slots = (uint8_t)slots;
        if (words) memcpy(best->lead_in, in, words * 4), best->lead_words = words;
        best->heard = now, best->heard_from = from, best->lead_far = far_off;
    }
    ReleaseSRWLockExclusive(&co_lives_lock);
}

static void co_upright(float m[9]) {                    /* (a turn mixed from two turns: made square again) */
    float *a = m, *b = m + 3, *d = m + 6;
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n < 1e-4f) { const float one[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(m, one, sizeof one); return; }
    for (int i = 0; i < 3; i++) a[i] /= n;
    const float along = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    for (int i = 0; i < 3; i++) b[i] -= a[i] * along;
    n = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (n < 1e-4f) { const float one[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(m, one, sizeof one); return; }
    for (int i = 0; i < 3; i++) b[i] /= n;
    d[0] = a[1] * b[2] - a[2] * b[1], d[1] = a[2] * b[0] - a[0] * b[2], d[2] = a[0] * b[1] - a[1] * b[0];
}

/* Moving an actor that has physics (an enemy): the fields co_body_put writes are not looked at, the game's own request
   is: f_80c8fd60(actor, a 3x4 matrix: the turn's rows, each with the place's part after it, 0). With a physics part
   ([actor+0x270]+0x50) it hands the place to that (f_804e6630); without, it writes the fields. All 21 of its callers
   pass 0 last. */
static int co_actor_move(Ctx *c, uint64_t actor, const float at[12]) {
    A64Fn ask = a64_lookup(0x80c8fd60ULL);
    if (!ask) return 0;
    float m[12];
    for (int i = 0; i < 3; i++) memcpy(m + i * 4, at + 3 + i * 3, 12), m[i * 4 + 3] = at[i];
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x40) & ~0xfULL;
    memcpy(A64_PTR(frame), m, sizeof m);
    c->x[31] = frame;
    c->x[0] = actor, c->x[1] = frame, c->x[2] = 0;
    ask(c);
    *c = saved;
    return 1;
}

/* What hit an actor this frame, as the game keeps it for its own questions (the script node QuerySensorHitIsHitTg,
   f_82afe200): [[[actor+0x270]+0x3b8]+0x68] is the list of hits taken: +8 its room, +0x10 the records (0x1d0 bytes
   each), +0x18 how many now. A record's +0x60 is a link to the one who hit: +8 the actor, +0x10 a number (-1: none).
   1 when one of them is this game's own doing (Link, or a thing of his: not an enemy, not another player's body). */
static int co_enemy_hits(uint64_t actor, uint32_t hash) {
    static volatile LONG said;
    const uint64_t parts = G64(actor + 0x270);
    const uint64_t part = co_ok(parts + 0x3b8, 8) ? G64(parts + 0x3b8) : 0;
    const uint64_t list = co_ok(part + 0x68, 8) ? G64(part + 0x68) : 0;
    if (!co_ok(list, 0x20)) return 0;
    const uint32_t count = G32(list + 0x18), room = G32(list + 8);
    const uint64_t records = G64(list + 0x10);
    if (!count || !room || room > 64 || count > room || !co_ok(records, room * 0x1d0ULL)) return 0;
    int mine = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t at = records + i * 0x1d0ULL;
        const uint64_t by = G32(at + 0x70) != 0xffffffffu ? G64(at + 0x68) : 0;
        char name[48] = "";
        if (co_ok(by, 0x30) && G64(by) == 0x84b45e18ULL) {
            const uint64_t name_at = G64(by + 0x20);
            if (co_ok(name_at, 48)) snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
        }
        /* (the one who hit is most often a thing with no name to read here: a seed's link is not to an actor. Whatever is
           not an enemy or another player's body is this game's own doing: nothing else in a game hits an enemy) */
        const int own = strncmp(name, "Enemy_", 6) && strncmp(name, "Coop", 4);
        mine |= own;
        if (InterlockedIncrement(&said) <= 6)             /* (the first few, for the log) */
            hos_log("coop: enemy %x: hit by %s (link %x), record %u of %u", hash, name[0] ? name : "?", G32(at + 0x70), i + 1, count);
    }
    return mine;
}

static void co_enemy_lead_step(Ctx *c, uint64_t life, uint64_t actor, int64_t now, int hit_by_me) {
    static int follow_only = -1;                        /* (test runs: HOS_COOP_FOLLOW=1, this game never leads; 2: only by its own hit on a boss) */
    static volatile LONG said_roles, said_plays;
    if (follow_only < 0) { const char *t = getenv("HOS_COOP_TEST"), *f = getenv("HOS_COOP_FOLLOW"); follow_only = t && !strcmp(t, "1") && f ? atoi(f) : 0; }
    /* (On: in test runs with a stand-in for the other player a copy was seen to follow its leader's place and to play its commands, and the lead to pass to
       the nearer player and back. Two real games have not tried it yet: HOS_COOP_ENEMY_PLACE=0 in hookshot_env.txt
       turns it off; health and death are shared either way. Bosses too (HOS_COOP_BOSS=0 leaves them to each game):
       Gohma's fight was watched through a following copy, see below.) */
    static int on = -1;
    if (on < 0) { const char *e = getenv("HOS_COOP_ENEMY_PLACE"); on = !(e && !strcmp(e, "0")); hos_log("coop: enemies in one place: %s", on ? "on" : "off (HOS_COOP_ENEMY_PLACE=0)"); }
    if (!on || co.links <= 0) return;
    /* (My game is in a cutscene or a conversation: what my copy does there is the scene's, not the fight's. It is neither
       said to the others nor moved by them until the scene is over; another game leads it meanwhile.) */
    if (co_busy_now) return;
    int role = 0, took = 0, note = 0, want_slots = 0, with_inputs = 0, changed = 0;
    uint32_t hash = 0, in[CO_LEAD_IN], in_words = 0;
    float born[3], at[12];
    char want[CO_LEAD_SLOTS][CO_AS_NAME], was[CO_AS_NAME] = "";
    uint8_t may_play[CO_LEAD_SLOTS] = {0};
    float my_far;
    {   /* (how far my Link is from it) */
        float mine[3], its[3];
        EnterCriticalSection(&co.lock);
        memcpy(mine, co.pos, sizeof mine);
        LeaveCriticalSection(&co.lock);
        memcpy(its, GP(actor + CO_ACTOR_PLACE), sizeof its);
        my_far = sqrtf((mine[0] - its[0]) * (mine[0] - its[0]) + (mine[1] - its[1]) * (mine[1] - its[1]) + (mine[2] - its[2]) * (mine[2] - its[2]));
    }
    AcquireSRWLockExclusive(&co_lives_lock);
    CoLife *e = NULL;
    for (int i = 0; i < CO_LIVES && !e; i++) if (co_lives[i].life == life && co_lives[i].actor == actor) e = &co_lives[i];
    static int bosses = -1;                             /* (bosses too, unless HOS_COOP_BOSS=0) */
    if (bosses < 0) { const char *b = getenv("HOS_COOP_BOSS"); bosses = !(b && !strcmp(b, "0")); }
    if (!e || (e->boss && !bosses)) { ReleaseSRWLockExclusive(&co_lives_lock); return; }
    hash = e->hash, memcpy(born, e->born, sizeof born);
    if (hit_by_me && !e->hit_at) e->hit_at = now;
    const int heard = e->heard && now - e->heard < 700000000LL;
    /* (A boss keeps its leader: its fight is a string of states only the leader's copy is in, and a new leader's copy
       would go on from its own. Two that lead it: the lower number keeps it. It changes hands only when its leader
       stops saying it.) */
    /* (But a follower whose own attack hit it and took no life takes it: a seed in Gohma's eye, say. What a boss does
       about such a hit, going down stunned, is decided by the copy that is in the fight's state, and a follower's copy
       is not: it blocks. So the boss becomes that player's, and his next shot stuns it as it does alone: seen in
       a test run (GuardShock on the shot that took it over, Down_Start on the next). A hit that
       takes life, a sword on a boss already down, changes nothing here: the life is shared, and the boss stays with
       the one whose copy has it down. The new leader says so with a distance below zero for six seconds; a leader
       that hears that lets go, and nobody else takes it meanwhile: two that say it at once, the lower number keeps
       it. The hit is judged an update late: the life it takes is taken in the update it is seen in.) */
    const int is_boss = e->boss, claiming = now < e->claim_until, they_claim = e->lead_far < 0;
    if (is_boss && !e->mine && heard && follow_only != 1 && !they_claim && e->hit_at && now - e->hit_at > 60000000LL) {
        if (now - e->hit_at < 500000000LL && e->hurt_at < e->hit_at) e->mine = 1, e->claim_until = now + 6000000000LL, took = 5;
        e->hit_at = 0;
    } else if (e->hit_at && now - e->hit_at > 500000000LL) e->hit_at = 0;
    if (took == 5) ;
    else if (e->mine && heard && (is_boss ? (they_claim && (!claiming || e->heard_from < (int)co.my_id)) || (!they_claim && !claiming && e->heard_from < (int)co.my_id)
                                     : e->lead_far + 1.0f < my_far || (e->lead_far <= my_far + 1.0f && e->heard_from < (int)co.my_id))) e->mine = 0, e->placed = 0, took = 2;
    if (!e->mine && heard && !follow_only && !is_boss && my_far + 2.0f < e->lead_far) {
        if (!e->nearer_since) e->nearer_since = now;
        else if (now - e->nearer_since > 500000000LL) e->mine = 1, e->nearer_since = 0, took = 4;
    } else e->nearer_since = 0;
    if (!e->mine && !heard && now - e->first > 400000000LL && !follow_only) e->mine = 1, took = 1;
    if (e->mine && now < e->claim_until) my_far = -1.0f;
    if (e->mine) {
        if (now - e->said >= 66000000LL) e->said = now, role = 1, with_inputs = e->said_n++ % 3 == 0, memcpy(was, e->said_cmd, CO_AS_NAME);
    } else if (heard) {
        role = 2;
        if (!e->placed) memcpy(e->at, GP(actor + CO_ACTOR_PLACE), sizeof e->at), took = 3;   /* (from where my copy stands) */
        float dt = e->placed ? (float)(now - e->put) * 1e-9f : 0.016f;
        if (dt > 0.1f) dt = 0.1f;
        const float dx = e->lead[0] - e->at[0], dy = e->lead[1] - e->at[1], dz = e->lead[2] - e->at[2];
        const float k = dx * dx + dy * dy + dz * dz > 64.0f ? 1.0f : 1.0f - expf(-dt * 16.0f);   /* (far off: there at once) */
        for (int i = 0; i < 12; i++) e->at[i] += (e->lead[i] - e->at[i]) * k;
        co_upright(e->at + 3);
        e->put = now, e->placed = 1;
        memcpy(at, e->at, sizeof at);
        memcpy(want, e->lead_cmd, sizeof want), want_slots = e->lead_slots;
        for (int i = 0; i < want_slots; i++) may_play[i] = strcmp(want[i], e->tried_cmd[i]) || !e->tried_bad[i] || now - e->tried_at[i] > 1000000000LL;
        in_words = e->lead_words;
        if (in_words) memcpy(in, e->lead_in, in_words * 4);
        if (now - e->noted > 500000000LL) e->noted = now, note = 1;
    }
    ReleaseSRWLockExclusive(&co_lives_lock);
    if (took && InterlockedIncrement(&said_roles) <= 60)
        hos_log("coop: enemy %x: %s (my Link is %.1f from it)", hash, took == 1 ? "this game leads it" : took == 2 ? "another game leads it now (nearer, or as near with the lower number)"
                : took == 4 ? "my Link is the nearer one: this game leads it now" : took == 5 ? "my own attack hit it: this game leads it now" : "another game leads it: my copy follows", my_far);
    if (role == 1) {
        char names[CO_AS_SLOTS][CO_AS_NAME], packed[72];
        const int slots = co_as_names(actor, names);
        static volatile LONG said_parts;
        if (took == 1 && InterlockedIncrement(&said_parts) <= 30) {   /* (what there is to say of how it moves) */
            char all[400] = "";
            size_t used = 0;
            for (int i = 0; i < slots && used + 60 < sizeof all; i++) if (names[i][0]) used += (size_t)snprintf(all + used, sizeof all - used, " %d:%s", i, names[i]);
            const uint64_t parts = G64(actor + 0x270), as0 = co_ok(parts + 0x40, 8) ? G64(parts + 0x40) : 0, ctl = co_as_ctl(actor);
            uint32_t words = 0;
            const uint64_t inputs = co_inputs_at(actor, &words);
            hos_log("coop: enemy %x: animation part %llx, controller %llx (vtable %llx), %d slots:%s; inputs %llx, %u words", hash, (unsigned long long)as0,
                    (unsigned long long)ctl, (unsigned long long)(ctl ? G64(ctl) : 0), slots, all, (unsigned long long)inputs, words);
        }
        co_base64((const uint8_t *)GP(actor + CO_ACTOR_PLACE), 48, packed);
        char cmds[CO_LEAD_SLOTS * CO_AS_NAME + 8] = "", inputs[600] = "";
        size_t used = 0;
        for (int i = 0; i < slots && i < CO_LEAD_SLOTS; i++) used += (size_t)snprintf(cmds + used, sizeof cmds - used, "%s%s", i ? "," : "", names[i][0] ? names[i] : "-");
        if (!used) snprintf(cmds, sizeof cmds, "-");
        if (with_inputs) {
            uint32_t words = 0;
            const uint64_t from = co_inputs_at(actor, &words);
            if (from && words && words <= CO_LEAD_IN) {
                const int n = snprintf(inputs, sizeof inputs, " %u:", words);
                co_base64((const uint8_t *)GP(from), words * 4ULL, inputs + n);
            }
        }
        co_send("Q %x %.1f %.1f %.1f %.1f %s %s%s", hash, born[0], born[1], born[2], my_far, packed, cmds, inputs);
        if (slots > 0 && strcmp(was, names[0])) {       /* (for the log: what it goes through) */
            static volatile LONG said_cmds;
            AcquireSRWLockExclusive(&co_lives_lock);
            if (e->life == life && e->actor == actor) memcpy(e->said_cmd, names[0], CO_AS_NAME);
            ReleaseSRWLockExclusive(&co_lives_lock);
            if (InterlockedIncrement(&said_cmds) <= 150 || is_boss) hos_log("coop: enemy %x: plays %s", hash, cmds);
        }
    } else if (role == 2) {
        uint8_t *a = (uint8_t *)GP(actor);
        static volatile LONG said_places;
        if (note && InterlockedIncrement(&said_places) <= 60) {
            float is[3];
            memcpy(is, a + CO_ACTOR_PLACE, sizeof is);
            hos_log("coop: enemy %x: my copy is at %.2f %.2f %.2f, the leader's wanted at %.2f %.2f %.2f", hash, is[0], is[1], is[2], at[0], at[1], at[2]);
        }
        if (!co_actor_move(c, actor, at)) {
            memcpy(a + CO_ACTOR_ASK_TURN + 4, at + 4, 32);
            memcpy(a + CO_ACTOR_ASK_PLACE + 4, at + 1, 8);
            memcpy(a + CO_ACTOR_ASK_TURN, at + 3, 4);
            memcpy(a + CO_ACTOR_ASK_PLACE, at, 4);
            HOS_WRITTEN(a + CO_ACTOR_ASK_PLACE, 48);
        }
        uint32_t words = 0;
        const uint64_t to = in_words ? co_inputs_at(actor, &words) : 0;
        if (to && words == in_words) {                  /* (the leader's inputs: the same graph has as many) */
            for (uint32_t i = 0; i < words; i++)
                if (!co_input_is_address(to + i * 4ULL)) *(uint32_t *)GP(to + i * 4ULL) = in[i];
            HOS_WRITTEN(GP(to), words * 4ULL);
        }
        char names[CO_AS_SLOTS][CO_AS_NAME];
        const int slots = co_as_names(actor, names);
        uint8_t bad[CO_LEAD_SLOTS] = {0};
        for (int i = 0; i < slots && i < want_slots; i++) {
            if (!strcmp(want[i], names[i])) continue;
            if (!want[i][0]) { co_as_remove(c, actor, i); continue; }
            if (!may_play[i]) continue;
            const int ok = co_as_play(c, actor, i, want[i]);
            char after[CO_AS_SLOTS][CO_AS_NAME];
            bad[i] = (co_as_names(actor, after) <= i || strcmp(after[i], want[i])) ? 2 : 1;
            changed = 1;
            if (InterlockedIncrement(&said_plays) <= 80)
                hos_log("coop: enemy %x: slot %d: my copy played '%s', the leader's plays '%s': asked (%d), now '%s'", hash, i, names[i], want[i], ok, bad[i] == 2 ? after[i] : want[i]);
        }
        if (changed) {
            AcquireSRWLockExclusive(&co_lives_lock);
            if (e->life == life && e->actor == actor)
                for (int i = 0; i < CO_LEAD_SLOTS; i++)
                    if (bad[i]) memcpy(e->tried_cmd[i], want[i], CO_AS_NAME), e->tried_at[i] = now, e->tried_bad[i] = bad[i] == 2;
            ReleaseSRWLockExclusive(&co_lives_lock);
        }
    }
}

void oot_coop_life_seen(Ctx *c, uint64_t life) {
    if (!co.ready || !co_room() || !co_ok(life, 0x1c20)) return;
    const uint64_t actor = G64(life + 0x18);
    if (!co_ok(actor, 0x390)) return;
    const uint64_t name_at = G64(actor + 0x20);
    if (!co_ok(name_at, 48) || memcmp(GP(name_at), "Enemy_", 6)) return;
    const uint64_t holder = G64(life + 0x1c10);
    if (!co_ok(holder, 16)) return;
    const int64_t now = hos_now_ns();
    int fresh = 0;
    char name[48] = "";
    AcquireSRWLockExclusive(&co_lives_lock);
    CoLife *e = NULL, *old = &co_lives[0];
    for (int i = 0; i < CO_LIVES && !e; i++) {
        if (co_lives[i].life == life && co_lives[i].actor == actor) e = &co_lives[i];
        else if (co_lives[i].seen < old->seen) old = &co_lives[i];
    }
    if (!e) e = old, memset(e, 0, sizeof *e), e->life = life, e->actor = actor, fresh = 1;
    else if (now - e->seen > 3000000000LL) {            /* (not updated for a while: is the memory still this enemy's?) */
        snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
        if (co_mm3(name) != e->hash) fresh = 1, e->owed = e->expect = 0, e->heard = 0;
    }
    if (fresh || now - e->seen > 500000000LL) e->mine = 0, e->first = now, e->placed = 0;   /* (updated again: who leads it is asked again) */
    e->seen = now;
    if (fresh) {
        snprintf(name, sizeof name, "%.47s", (const char *)GP(name_at));
        e->hash = co_mm3(name);
        memcpy(e->born, GP(actor + 796), sizeof e->born);
        e->boss = strstr(name, "Boss") != NULL;
    }
    for (int i = 0; i < CO_BLOWS; i++) {                /* (hits that came before this game had it) */
        CoBlow *b = &co_blows[i];
        if (!b->taken || b->hash != e->hash || now - b->since > CO_BLOW_KEPT) continue;
        const float dx = b->at[0] - e->born[0], dy = b->at[1] - e->born[1], dz = b->at[2] - e->born[2];
        if (dx * dx + dy * dy + dz * dz > 6.25f) continue;   /* (another of its kind) */
        e->owed += b->taken, b->taken = 0;
    }
    const uint32_t owed = e->owed, hash = e->hash;
    if (owed) e->owed = 0, e->expect = (now < e->expect_until ? e->expect : 0) + owed, e->expect_until = now + 1500000000LL;
    float born[3];
    memcpy(born, e->born, sizeof born);
    ReleaseSRWLockExclusive(&co_lives_lock);
    if (fresh && InterlockedIncrement(&co_lives_said) <= 80)
        hos_log("coop: enemy: %s, life %u: U %x %.1f %.1f %.1f", name, G32(holder + 8), hash, born[0], born[1], born[2]);
    if (owed) {
        const uint32_t left = G32(holder + 8);
        const int told = co_enemy_damage(c, life, owed);
        hos_log("coop: enemy %x: the others' hits, %u in all, given to it (%d): it had %u", hash, owed, told, left);
    }
    co_enemy_lead_step(c, life, actor, now, co_enemy_hits(actor, hash));
}

void oot_coop_life_write(Ctx *c, uint64_t life, uint32_t was, uint32_t now_left) {
    (void)c;
    if (!co.ready || !co_room() || now_left >= was || !co_ok(life, 0x20)) return;
    const uint64_t actor = G64(life + 0x18);
    const int64_t now = hos_now_ns();
    int found = 0;
    uint32_t hash = 0, mine = was - now_left;
    float born[3] = {0, 0, 0};
    AcquireSRWLockExclusive(&co_lives_lock);
    for (int i = 0; i < CO_LIVES && !found; i++) {
        CoLife *e = &co_lives[i];
        if (e->life != life || e->actor != actor) continue;
        found = 1, hash = e->hash, memcpy(born, e->born, sizeof born);
        if (now >= e->expect_until) e->expect = 0;
        const uint32_t theirs = e->expect < mine ? e->expect : mine;   /* (the others' hits being taken: not mine to tell) */
        e->expect -= theirs, mine -= theirs;
        if (mine) e->hurt_at = now;
        if (!now_left) e->expect = 0;
    }
    ReleaseSRWLockExclusive(&co_lives_lock);
    if (!found) return;                                 /* (not an enemy: Link, a sign, a rock) */
    if (mine && co.links > 0) co_send("U %x %.1f %.1f %.1f %u", hash, born[0], born[1], born[2], mine);
    hos_log("coop: enemy %x: life %u -> %u (%u of it my hit%s)", hash, was, now_left, mine, mine && co.links > 0 ? ", told to the others" : "");
}

/* The game's side is two callers. oot_coop_frame runs every frame shown, on the thread that shows it (host/nn_vi.c
   hos_vi_native_presented): the menu's rows, the link's comings and goings, the notes. It runs at the title screen too, so a
   link is up before a save is loaded. co_tick runs from Link's own update, so only while a save is being
   played: everything that reads or changes the game. co.game keeps the two apart. */

void oot_coop_frame(void) {
    static int64_t next_slow;
    {   /* (co-op's corner and the names are not drawn over the game's own menus: the pause menu, the map. Not at the title,
           where the corner says whether a player is in the room before a save is loaded) */
        int hos_oot_menu_in_front(void);               /* host/oot_menu.c */
        static int hidden = -1;
        const int hide = co_playing && hos_oot_menu_in_front();
        if (hide != hidden) hidden = hide, rx_option_set(HOS_OPT_COOP_HIDE, hide);
    }
    co_ready();   /* (the room's view and its lines, asked for once, from the game's first frame) */
    const int action = rx_option(HOS_OPT_COOP_ACTION, 0);
    if (action >= 10 && action < 10 + CO_PEERS) co_goto_ask(action - 10);   /* (the Co-op tab's Go to player rows) */
    const int64_t now = hos_now_ns();
    EnterCriticalSection(&co.game);
    co_events();
    {   /* Link's update does not run while this game is paused (the pause menu, the map, a shop): the others would
           read the silence as a player leaving, sink their puppet of him, and a moment at the save screen would ask
           everyone to join again. A game that was playing keeps saying where it is — once a second, my last place,
           and busy so the others' PvP leaves me alone. */
        if (co.ready && co_room() && co_playing && co_map[0] && co.links > 0 && now - co_place_sent > 1000000000LL) {
            float p[3], yaw;
            EnterCriticalSection(&co.lock);
            memcpy(p, co.pos, sizeof p), yaw = co.yaw;
            LeaveCriticalSection(&co.lock);
            co_send("P %s %.2f %.2f %.2f %.3f %d %d %d", co_map, p[0], p[1], p[2], yaw, (int)co_my_child, 1, rx_option(HOS_OPT_COOP_PVP, 0) > 0);
            co_place_sent = now;   /* (co.game is held here and taken by co_tick's own sends: one writer at a time) */
        }
    }
    if (now >= next_slow) {
        next_slow = now + 1000000000LL;
        const int playing = co_playing && co.player_seen && now - co.player_seen < 1000000000LL;
        co_players_list(now, co_map, playing);
    }
    LeaveCriticalSection(&co.game);
}

/* (oot_coop_player: Link's own update, every frame of play, on whichever of the game's workers runs it.
   The game's "is an event playing?" question is asked only now and then.) At most 100 times a second, one caller at a time. */
static void co_tick(Ctx *c, uint64_t link) {
    static int64_t next_map, next_place, next_flags, playing_since, last_playing;
    static volatile int64_t last_tick;
    static int64_t quiet_until;                        /* nothing is changed in the game before this: an event or a load, and two seconds after */
    if (!co.ready || !co_room()) return;
    const int64_t now = hos_now_ns();
    if (now - last_tick < 10000000LL || !TryEnterCriticalSection(&co.game)) return;
    last_tick = now;
    /* in the game: Link was updated a moment ago, and for three seconds on end (a save just loaded settles first) */
    const int playing_now = co.player_seen && now - co.player_seen < 1000000000LL;
    if (playing_now && (!last_playing || now - last_playing > 3000000000LL)) playing_since = now;
    if (playing_now) last_playing = now;
    const int playing = playing_now && now - playing_since > 3000000000LL;
    co_playing = playing;
    co.my_user = co_equipment_user(link);   /* (two reads, every tick: what my game's equipment calls may compare to) */
    {   /* (Link is another actor: another map. Its name is read at once, so no body is made for the map just left) */
        static uint64_t last_link;
        if (link != last_link) last_link = link, next_map = 0;
    }
    {   /* (once a minute while co-op is on: what the game's side sees, for the log) */
        static int64_t next_say;
        if (now >= next_say) {
            next_say = now + 60000000000LL;
            hos_log("coop: tick: Link seen %lld ms ago, playing %d, map '%s', links %ld, state %ld", co.player_seen ? (long long)((now - co.player_seen) / 1000000) : -1LL,
                    playing, co_map, (long)co.links, (long)co.state);
        }
    }
    if (playing && now >= next_map) next_map = now + 1000000000LL, co_current_map(c, co_map, sizeof co_map);
    if (playing && co_map[0]) {
        if (now >= next_place && co.links > 0) {
            next_place = now + 33000000LL;
            float p[3], yaw;
            EnterCriticalSection(&co.lock);
            memcpy(p, co.pos, sizeof p), yaw = co.yaw;
            LeaveCriticalSection(&co.lock);
            {   /* (the flag table reads open only while sync is taking; say the last age known, so the others'
                   puppet does not flicker between child and adult while the table is shut) */
                CoFlags f;
                const uint8_t *v = co_flags_open(&f) ? co_flag_value(&f, 0xcea220d3u, NULL) : NULL;
                if (v) InterlockedExchange(&co_my_child, (long)(*v & 1));
            }
            co_send("P %s %.2f %.2f %.2f %.3f %d %d %d", co_map, p[0], p[1], p[2], yaw, (int)co_my_child, co_game_busy(), rx_option(HOS_OPT_COOP_PVP, 0) > 0);
            co_place_sent = now;
            co_anim_say(link);
        }
        { const int busy = co_game_busy(); InterlockedExchange(&co_busy_now, busy); if (busy) quiet_until = now + 2000000000LL; }
        const int quiet = now < quiet_until;
        if (now >= next_flags && !quiet) {
            next_flags = now + 500000000LL;
            if (co_level && co.links > 0) co_level = 0, co_say_hearts(c), co_equip_say_again();   /* (a player joined: my hearts and what my Link holds, so theirs catch up) */
        }
        if (!quiet) co_items_step(c, now);
        if (!quiet) co_trade_step(c, now);
        co_bodies_step(c, link, now);
        co_pvp_step(c, link, now);
        InterlockedExchange(&co_test_play, 0);
        { static int test = -1; if (test < 0) { const char *e = getenv("HOS_COOP_TEST"); test = e && !strcmp(e, "1"); } co_as_test(c, link, test); }
        co_goto_step(c, link, now);
    }
    LeaveCriticalSection(&co.game);
}

/* (nx2/oot_patches.py patch_coop_pickup: the head of f_8112d570, where every pickup enters the pouch. x1 points at the
   item's name, w3 is how many.) An item this side is handing over is not a find of mine. */
void oot_coop_pouch_add(Ctx *c) {
    if (!co.ready || co.links <= 0 || co_applying) return;
    const uint64_t ref = c->x[1];
    const int many = (int)(int32_t)c->x[3];
    if (!ref || many < 1) return;
    const uint64_t at = G64(ref);   /* (the game's own argument, read as the game is about to read it) */
    if (!at) return;
    const char *name = (const char *)GP(at);
    const size_t len = strnlen(name, 0x60);
    if (!len || len >= 0x60) return;
    co_pickup(name, many);
}
