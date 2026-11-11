/* (hookshot) The Insert menu's Co-op tab, the host's side (MkwVkHost.coop_view and coop_command, gpu/vk/vk_api.h): the room
   as the process's co-op session has it (host/coop.h) and as progress sync offers it (host/oot_coop.h: each member's
   identity key, the offer to accept, the maker's remedies), in the tab's terms, and what the tab asks turned into
   coop_configure, coop_command and progress sync's calls. The player's name and the relay are the tab's two fields
   (hookshot_options.ini's coop_name= and coop_server=, which the menu saves and coop_init reads at the next start);
   making or joining a room from the tab switches co-op on with them for this run. Make a room makes a room of a new
   random secret, or of HOS_COOP_ROOM's (the environment only, as host/coop.c takes it: test sessions whose second game
   types a secret known before).
   HOS_COOP_DEMO=1: a made-up room instead, for pictures of the tab and for trying it without a relay; the session is never
   started and nothing is sent. It follows what the tab asks: Make a room connects for a second, then the player is the
   room's maker, alone; Saria comes in, unidentified for three seconds, then with her key, and Darunia with his; the room is
   offered for acceptance, and Darunia is away for a while; the maker may remove a player or reset the pools. Join a room
   (any valid secret) puts the player third in Malon's room, offered at once. Accept takes the offer, Leave leaves. All of
   it from the menu's thread. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "coop.h"
#include "coop_state.h"
#include "../gpu/vk/vk_api.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

int64_t hos_now_ns(void);
void hos_log(const char *fmt, ...);
int hos_ini_text(const char *key, char *out, size_t size);   /* host/nn_applet.c: hookshot_options.ini's line */
/* host/oot_coop.h's calls for the menu (declared here: that header needs the lifted code's a64.h) */
int hos_oot_coop_offer(CoopOffer *out);
int hos_oot_coop_accept(uint32_t token);
int hos_oot_coop_is_maker(void);
int hos_oot_coop_reset_pools(void);
int hos_oot_coop_remove_key(const char *fingerprint);
int hos_oot_coop_contributors(CoopContributor *out, int max);

static char g_name[64], g_server[64];   /* as the tab's fields have them */
_Static_assert(HOS_COOP_STATE_OFF == (int)COOP_OFF && HOS_COOP_STATE_LEFT == (int)COOP_LEFT &&
                   HOS_COOP_STATE_CONNECTING == (int)COOP_CONNECTING && HOS_COOP_STATE_JOINED == (int)COOP_JOINED &&
                   HOS_COOP_STATE_RECONNECTING == (int)COOP_RECONNECTING && (int)HOS_COOP_MEMBERS == (int)COOP_MAX_MEMBERS,
               "vk_api.h's co-op states and members are host/coop.h's");

static void copy_text(char *out, size_t cap, const char *in) { snprintf(out, cap, "%s", in ? in : ""); }

/* A field left empty: the environment's, else the options file's, as coop_init takes them (the file's relay is not the
   field's when it is longer than the field holds) */
static const char *fallback(const char *field, const char *env, const char *key, char *buf, size_t cap) {
    if (field[0]) return field;
    if (getenv(env)) return getenv(env);
    return hos_ini_text(key, buf, cap) ? buf : "";
}
static const char *name_now(void) {
    static char buf[160];
    return fallback(g_name, "HOS_COOP_NAME", "coop_name", buf, sizeof buf);
}
static const char *server_now(void) {
    static char buf[160];
    return fallback(g_server, "HOS_COOP_SERVER", "coop_server", buf, sizeof buf);
}

static int demo_on(void) {
    static int on = -1;
    if (on < 0) on = env_on("HOS_COOP_DEMO");
    return on;
}

/* ---- the made-up room ---- */
static struct {
    int state, made, accepted, asks;   /* asks: the commands taken, for the view's version */
    unsigned removed;                  /* the made-up members the maker removed, a bit by id */
    int64_t since_ms;                  /* the last Make or Join */
    char code[24], status[96];
} g_demo = {HOS_COOP_STATE_OFF, 0, 0, 0, 0, 0, "", "a made-up room (HOS_COOP_DEMO=1): nothing is sent"};
enum { DEMO_CONNECT_MS = 1200, DEMO_SARIA_MS = 3000, DEMO_KEYS_MS = 6000, DEMO_AWAY_MS = 9000, DEMO_BACK_MS = 13000 };
static const uint32_t kDemoOffer = 0x5eed0c0bu;

static int64_t now_ms(void) { return hos_now_ns() / 1000000; }

static void demo_member(MkwVkCoopView *v, int id, const char *name, const char *key, int self, int maker, int away) {
    if (g_demo.removed >> id & 1) return;
    MkwVkCoopMember *m = &v->member[v->n++];
    m->id = id, m->self = (uint8_t)self, m->maker = (uint8_t)maker, m->away = (uint8_t)away;
    m->accepted = (uint8_t)(!self && key[0] && g_demo.accepted);
    copy_text(m->name, sizeof m->name, name), copy_text(m->key, sizeof m->key, key);
}

static void demo_view(MkwVkCoopView *v) {
    const int64_t t = now_ms() - g_demo.since_ms;
    const int connecting = g_demo.state == HOS_COOP_STATE_CONNECTING;
    if (connecting && t >= DEMO_CONNECT_MS)
        g_demo.state = HOS_COOP_STATE_JOINED, copy_text(g_demo.status, sizeof g_demo.status, "a made-up room (HOS_COOP_DEMO=1): nothing is sent");
    v->state = g_demo.state;
    copy_text(v->status, sizeof v->status, g_demo.status);
    copy_text(v->name, sizeof v->name, name_now()[0] ? name_now() : "Player-3c1e");
    if (g_demo.state == HOS_COOP_STATE_CONNECTING || g_demo.state == HOS_COOP_STATE_JOINED) copy_text(v->code, sizeof v->code, g_demo.code);
    int phase = 0;
    if (g_demo.state == HOS_COOP_STATE_JOINED) {
        v->ping_ms = 23;
        if (g_demo.made) {
            demo_member(v, 1, v->name, "4be1-07d2-9a3f-c615", 1, 1, 0);
            if (t >= DEMO_SARIA_MS) demo_member(v, 2, "Saria", t >= DEMO_KEYS_MS ? "7c1d-e842-3b90-a5f6" : "", 0, 0, 0);
            if (t >= DEMO_KEYS_MS) demo_member(v, 3, "Darunia", "d03a-5f17-c2e9-8b44", 0, 0, t >= DEMO_AWAY_MS && t < DEMO_BACK_MS);
            phase = t < DEMO_SARIA_MS ? 1 : t < DEMO_KEYS_MS ? 2 : t < DEMO_AWAY_MS ? 3 : t < DEMO_BACK_MS ? 4 : 5;
            v->unidentified = t >= DEMO_SARIA_MS && t < DEMO_KEYS_MS;
            v->offer = t >= DEMO_KEYS_MS ? kDemoOffer : 0;
        } else {
            demo_member(v, 1, "Malon", "a9e0-31c7-6d58-f2b4", 0, 1, 0);
            demo_member(v, 2, "Saria", "7c1d-e842-3b90-a5f6", 0, 0, 0);
            demo_member(v, 3, v->name, "4be1-07d2-9a3f-c615", 1, 0, 0);
            v->offer = kDemoOffer;
            phase = 6;
        }
        v->accepted = g_demo.accepted;
        v->remedies = g_demo.made;
    }
    v->version = (uint32_t)(g_demo.asks << 8 | g_demo.state << 4 | phase);
}

static int demo_command(int cmd, const char *text, uint32_t number) {
    char secret[COOP_SECRET_LEN + 1];
    const int in_room = g_demo.state == HOS_COOP_STATE_CONNECTING || g_demo.state == HOS_COOP_STATE_JOINED;
    switch (cmd) {
    case HOS_COOP_CMD_MAKE:
    case HOS_COOP_CMD_JOIN:
        if (in_room) return 0;
        if (cmd == HOS_COOP_CMD_JOIN && !coop_secret_normalize(text ? text : "", secret)) return 0;
        if (cmd == HOS_COOP_CMD_MAKE) memcpy(secret, "K7XG2MRTB9DHW4PZ5N", sizeof secret);
        coop_secret_format(secret, g_demo.code);
        g_demo.state = HOS_COOP_STATE_CONNECTING, g_demo.made = cmd == HOS_COOP_CMD_MAKE, g_demo.accepted = 0, g_demo.removed = 0;
        g_demo.since_ms = now_ms();
        copy_text(g_demo.status, sizeof g_demo.status, cmd == HOS_COOP_CMD_MAKE ? "made up: making a room" : "made up: joining the room");
        break;
    case HOS_COOP_CMD_LEAVE:
        if (!in_room) return 0;
        g_demo.state = HOS_COOP_STATE_LEFT, g_demo.code[0] = 0;
        copy_text(g_demo.status, sizeof g_demo.status, "made up: left the room");
        break;
    case HOS_COOP_CMD_ACCEPT: {
        MkwVkCoopView v;
        memset(&v, 0, sizeof v);
        demo_view(&v);
        if (!v.offer || number != v.offer || v.accepted) return 0;
        g_demo.accepted = 1;
        copy_text(g_demo.status, sizeof g_demo.status, "accepted: your save was copied first (made up: nothing copied)");
        break;
    }
    case HOS_COOP_CMD_REMOVE: {   /* (the member of that key, from the maker's room) */
        MkwVkCoopView v;
        memset(&v, 0, sizeof v);
        demo_view(&v);
        int id = 0;
        for (int i = 0; i < v.n; i++)
            if (!v.member[i].self && v.member[i].key[0] && text && !strcmp(text, v.member[i].key)) id = v.member[i].id;
        if (!v.remedies || !id) return 0;
        g_demo.removed |= 1u << id;
        copy_text(g_demo.status, sizeof g_demo.status, "made up: the player was removed");
        break;
    }
    case HOS_COOP_CMD_RESET_POOLS:
        if (!g_demo.made || g_demo.state != HOS_COOP_STATE_JOINED) return 0;
        copy_text(g_demo.status, sizeof g_demo.status, "made up: the pools were reset");
        break;
    default:
        return 1;   /* (the name and the relay: kept above, nothing to do in a made-up room) */
    }
    g_demo.asks++;
    return 1;
}

/* ---- the interface ---- */
void hos_coop_menu_view(MkwVkCoopView *out) {
    memset(out, 0, sizeof *out);
    if (demo_on()) {
        demo_view(out);
        return;
    }
    CoopView v;
    coop_view_get(&v);
    out->version = v.version;
    out->state = (int32_t)v.state;
    copy_text(out->code, sizeof out->code, v.code);
    copy_text(out->name, sizeof out->name, v.name);
    copy_text(out->status, sizeof out->status, v.status);
    out->ping_ms = v.ping_ms;
    for (int i = 0; i < v.n && i < COOP_MAX_MEMBERS && out->n < HOS_COOP_MEMBERS; i++) {
        MkwVkCoopMember *m = &out->member[out->n++];
        m->id = v.member[i].id;
        m->self = v.member[i].id == v.self;
        m->maker = v.member[i].maker || (v.maker && v.member[i].id == v.maker);
        m->away = v.member[i].away != 0;
        copy_text(m->name, sizeof m->name, v.member[i].name);
    }
    if (v.state != COOP_JOINED && v.state != COOP_RECONNECTING) return;
    /* (progress sync: the others' keys from the offer, which it makes while the room is not accepted and keeps, accepted,
       with its acceptance; this player's own key from the pools' list, which marks it) */
    CoopOffer offer;
    if (hos_oot_coop_offer(&offer)) {
        out->offer = offer.token, out->accepted = offer.accepted, out->unidentified = offer.unidentified;
        for (int j = 0; j < offer.n && j < COOP_MAX_MEMBERS; j++)
            for (int k = 0; k < out->n; k++)
                if (out->member[k].id == offer.member[j].id && !out->member[k].self) {
                    copy_text(out->member[k].key, sizeof out->member[k].key, offer.member[j].key);
                    out->member[k].accepted = (uint8_t)(offer.accepted && offer.member[j].key[0] && !offer.member[j].accepted_as[0]);
                }
    }
    CoopContributor who[COOP_MAX_MEMBERS * 2];
    const int n = hos_oot_coop_contributors(who, COOP_MAX_MEMBERS * 2);
    for (int j = 0; j < n; j++)
        for (int k = 0; k < out->n; k++)
            if (who[j].self && out->member[k].self) copy_text(out->member[k].key, sizeof out->member[k].key, who[j].key);
    out->remedies = hos_oot_coop_is_maker();
}

int hos_coop_menu_command(int cmd, const char *text, uint32_t number) {
    if (cmd == HOS_COOP_CMD_NAME || cmd == HOS_COOP_CMD_SERVER) {
        char host[160];
        uint16_t port = 0;
        if (cmd == HOS_COOP_CMD_SERVER && text && *text && !coop_server_parse(text, host, sizeof host, &port)) return 0;
        copy_text(cmd == HOS_COOP_CMD_NAME ? g_name : g_server, sizeof g_name, text);
        if (demo_on()) return 1;
        CoopView v;
        coop_view_get(&v);
        /* (number 1: the field's Enter, applied now to a session that runs: a new name rejoins under it, a new relay leaves the
           room. 0: the saved value at the start, for the next Make or Join) */
        return !number || v.state == COOP_OFF || coop_configure(1, name_now(), server_now());
    }
    if (demo_on()) return demo_command(cmd, text, number);
    char secret[COOP_SECRET_LEN + 1];
    switch (cmd) {
    case HOS_COOP_CMD_MAKE:
    case HOS_COOP_CMD_JOIN: {
        /* (Make: a new random secret, or HOS_COOP_ROOM's, from the environment only: a test session's known room) */
        const char *room = cmd == HOS_COOP_CMD_MAKE ? getenv("HOS_COOP_ROOM") : text;
        const int have = room && *room && coop_secret_normalize(room, secret);
        if (cmd == HOS_COOP_CMD_JOIN && !have) return 0;
        if (!coop_configure(1, name_now(), server_now())) {
            hos_log("coop: the Co-op tab could not switch co-op on");
            return 0;
        }
        return coop_command(cmd == HOS_COOP_CMD_MAKE ? COOP_CMD_CREATE : COOP_CMD_JOIN, have ? secret : NULL);
    }
    case HOS_COOP_CMD_LEAVE:
        return coop_command(COOP_CMD_LEAVE, NULL);
    case HOS_COOP_CMD_ACCEPT:   /* (progress sync copies the save first, then takes the room's values) */
        return hos_oot_coop_accept(number);
    case HOS_COOP_CMD_REMOVE:
        return text && *text && hos_oot_coop_remove_key(text);
    case HOS_COOP_CMD_RESET_POOLS:
        return hos_oot_coop_reset_pools();
    default:
        return 0;
    }
}
