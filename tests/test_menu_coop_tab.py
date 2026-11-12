"""The Insert menu's Co-op tab: making, joining, accepting and leaving a co-op room from the menu (host/coop.h's session).

The menu's side is cut out of gpu/vk/vk_menu.cpp by its text and driven as menu_tick drives it: which rows the tab shows in
each state of the room (Make a room and Join a room outside one; the room's secret and Leave in one; Accept while the room
is offered), what the relay's and the secret's fields take, what each press asks of the host (Make at once; Accept and Leave
at the second press only; the secret shown and hidden without asking anything), the fields' Enter (Join emptied once taken,
or why not; a relay the host refuses not saved), and the selection staying on its row while rows come and go. The host's
side, host/coop_menu.c, is compiled as C with the wire format's real secrets (host/coop_proto.c, Monocypher) and the session
stubbed: the view as the tab gets it (you, the maker), the commands turned into coop_configure and coop_command (a name or a
relay set at the start never restarts a running session; a secret that is not one never switches co-op on), and
HOS_COOP_DEMO=1's made-up room walked through on a made-up clock. The Room rules page (what a room this game makes shares)
is checked against the engine itself: a C program with host/coop_state.h's categories, modes and defaults and the rows'
keys, ranges, steps and defaults as the source has them. Then the wiring is checked in the source: the interface's
two new members last and its version bumped, the host's table, the tab in the strip, the page, the options file's
coop_name= and coop_server=. No GPU, game, window or network.

    python -I -B tests/test_menu_coop_tab.py [--cxx g++] [--cc gcc]
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

RUN_ALL_ARGS = ["--cxx", "{cxx}"]

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    """The text from marker to the end of the brace block that follows it (a function, or a struct with its ;)"""
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + (";" if text[end:end + 1] == ";" else "")


def between(text, first, last):
    start = text.index(first)
    return text[start:text.index(last, start)]


def need(text, pattern, what):
    if not re.search(pattern, text, re.S):
        raise SystemExit("FAIL: " + what)


MENU_FIXTURE = r'''#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum { VK_BACK = 8, VK_RETURN = 0x0d, VK_ESCAPE = 0x1b, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_RIGHT = 0x27, VK_DELETE = 0x2e };
@@API@@
@@TABS@@
@@ROW@@
@@TEXT@@
static struct { int coop_progress = 1; } opts;
@@RULES@@
Row rows[] = {
    {TAB_GAMEPLAY, "Restart", nullptr, 0, 0, 0, nullptr, nullptr},
@@ROWS@@
};
constexpr int kRows = sizeof rows / sizeof rows[0];
const Row* pending = nullptr;
int tab_selected[TAB_RULES + 1] = {};
static int saves = 0;
void save() { saves++; }
struct Asked { int cmd; char text[64]; uint32_t number; } asked[32];
static int n_asked = 0, answer = 1;
static MkwVkCoopView room;
static void host_view(MkwVkCoopView* out) { *out = room; }
static int host_command(int cmd, const char* text, uint32_t number) {
    CHECK(n_asked < 32);
    asked[n_asked].cmd = cmd, asked[n_asked].number = number;
    std::snprintf(asked[n_asked].text, sizeof asked[n_asked].text, "%s", text ? text : "(null)");
    n_asked++;
    return answer;
}
struct { void (*coop_view)(MkwVkCoopView*); int (*coop_command)(int, const char*, uint32_t); } the_host{host_view, host_command},
    *host = &the_host;
@@COOP@@
bool row_shown(int i) { return rows[i].tab != TAB_COOP || coop_row_shown(rows[i], coop_now); }   // (vk_menu.cpp's Co-op clause)
@@ROWMAP@@
@@APPLY@@
@@PRESS@@
/// The Co-op tab's rows as shown, by kind, ending with -1
static void shown(int* out) {
    int n = 0;
    for (int i = 0; i < kRows; i++)
        if (rows[i].tab == TAB_COOP && row_shown(i)) out[n++] = coop_kind(rows[i]);
    out[n] = -1;
}
static bool shows(std::initializer_list<int> want) {
    int got[16];
    shown(got);
    int i = 0;
    for (int k : want) if (got[i++] != k) return false;
    return got[i] == -1;
}
static const Row& row(int kind) {
    for (const Row& r : rows) if (coop_kind(r) == kind) return r;
    std::fprintf(stderr, "FAIL: no row of kind %d\n", kind);
    std::exit(1);
}
static int selected_kind() { return coop_kind(rows[tab_row_to_global(TAB_COOP, tab_selected[TAB_COOP])]); }
static void set_room(int state, const char* code, uint32_t offer, int accepted) {
    room = MkwVkCoopView{};
    room.state = state, room.offer = offer, room.accepted = accepted, room.version = (uint32_t)(state * 10 + offer + accepted);
    std::snprintf(room.code, sizeof room.code, "%s", code);
    bool changed = false;
    coop_refresh(&changed);
    CHECK(changed);
}
int main() {
    // ---- the rows by the room's state ----
    CHECK(kRows == 1 + kCoopRows);   // (every row of the tab is one coop_kind knows)
    for (const Row& r : rows) CHECK(r.tab != TAB_COOP || (coop_kind(r) >= 0 && !r.value && r.help && std::strlen(r.help) > 20));
    CHECK(coop_kind(rows[0]) == -1);
    CHECK(shows({kCoopName, kCoopRelay, kCoopMake, kCoopJoin, kCoopRules}));   // (off)
    set_room(HOS_COOP_STATE_LEFT, "", 0, 0);
    CHECK(shows({kCoopName, kCoopRelay, kCoopMake, kCoopJoin, kCoopRules}));
    tab_selected[TAB_COOP] = 2;   // (on Make a room)
    set_room(HOS_COOP_STATE_CONNECTING, "K7XG2M-RTB9DH-W4PZ5N", 0, 0);
    CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopLeave}) && selected_kind() == kCoopSecret);   // (its row gone: the first after the fields)
    set_room(HOS_COOP_STATE_JOINED, "K7XG2M-RTB9DH-W4PZ5N", 0, 0);
    CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopLeave}));
    tab_selected[TAB_COOP] = 3;   // (on Leave)
    set_room(HOS_COOP_STATE_JOINED, "K7XG2M-RTB9DH-W4PZ5N", 77, 0);   // (offered)
    CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopAccept, kCoopLeave}) && selected_kind() == kCoopLeave);   // (kept on its row)
    set_room(HOS_COOP_STATE_JOINED, "K7XG2M-RTB9DH-W4PZ5N", 77, 1);   // (accepted)
    CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopLeave}) && selected_kind() == kCoopLeave);
    set_room(HOS_COOP_STATE_RECONNECTING, "K7XG2M-RTB9DH-W4PZ5N", 77, 0);   // (not offered while reconnecting)
    CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopLeave}));
    {   // (the same view again: nothing changed, nothing to draw)
        bool changed = false;
        coop_refresh(&changed);
        CHECK(!changed);
    }

    // ---- what each press asks of the host ----
    set_room(HOS_COOP_STATE_JOINED, "K7XG2M-RTB9DH-W4PZ5N", 77, 0);
    n_asked = 0;
    coop_press(row(kCoopSecret));
    CHECK(coop_secret_shown && n_asked == 0);
    coop_press(row(kCoopSecret));
    CHECK(!coop_secret_shown && n_asked == 0);
    coop_press(row(kCoopAccept));   // (armed: nothing asked yet)
    CHECK(pending == &row(kCoopAccept) && n_asked == 0);
    coop_press(row(kCoopAccept));
    CHECK(!pending && n_asked == 1 && asked[0].cmd == HOS_COOP_CMD_ACCEPT && asked[0].number == 77);
    coop_press(row(kCoopLeave));
    CHECK(pending == &row(kCoopLeave) && n_asked == 1);
    coop_press(row(kCoopAccept));   // (another row: arms that one, the first disarmed)
    CHECK(pending == &row(kCoopAccept) && n_asked == 1);
    pending = &row(kCoopAccept);
    set_room(HOS_COOP_STATE_JOINED, "K7XG2M-RTB9DH-W4PZ5N", 77, 1);   // (accepted elsewhere: the armed Accept goes with its row)
    CHECK(!pending);
    coop_press(row(kCoopLeave)), coop_press(row(kCoopLeave));
    CHECK(n_asked == 2 && asked[1].cmd == HOS_COOP_CMD_LEAVE);
    set_room(HOS_COOP_STATE_LEFT, "", 0, 0);
    coop_press(row(kCoopMake));   // (at once)
    CHECK(n_asked == 3 && asked[2].cmd == HOS_COOP_CMD_MAKE && !pending);
    // (the maker's remedies: only to the game holding the maker's key; Remove a player among the others with a key)
    room = MkwVkCoopView{};
    room.state = HOS_COOP_STATE_JOINED, room.remedies = 1, room.n = 3, room.version = 99, room.member[0].self = 1;
    std::snprintf(room.code, sizeof room.code, "K7XG2M-RTB9DH-W4PZ5N");
    std::snprintf(room.member[1].name, sizeof room.member[1].name, "Saria"), std::snprintf(room.member[1].key, sizeof room.member[1].key, "7c1d-e842-3b90-a5f6");
    std::snprintf(room.member[2].name, sizeof room.member[2].name, "Darunia"), std::snprintf(room.member[2].key, sizeof room.member[2].key, "d03a-5f17-c2e9-8b44");
    {
        bool changed = false;
        coop_refresh(&changed);
        CHECK(changed && shows({kCoopName, kCoopRelay, kCoopSecret, kCoopRemove, kCoopReset, kCoopLeave, kCoopRules}));
    }
    n_asked = 0, coop_remove_at = 0;
    coop_press(row(kCoopRemove));
    CHECK(pending == &row(kCoopRemove) && n_asked == 0);
    coop_press(row(kCoopRemove));
    CHECK(n_asked == 1 && asked[0].cmd == HOS_COOP_CMD_REMOVE && !std::strcmp(asked[0].text, "7c1d-e842-3b90-a5f6"));
    coop_remove_at = -1;   // (Left from the first: the last)
    coop_press(row(kCoopRemove)), coop_press(row(kCoopRemove));
    CHECK(n_asked == 2 && !std::strcmp(asked[1].text, "d03a-5f17-c2e9-8b44"));
    coop_press(row(kCoopReset)), coop_press(row(kCoopReset));
    CHECK(n_asked == 3 && asked[2].cmd == HOS_COOP_CMD_RESET_POOLS);
    room.member[2].key[0] = 0, room.member[1].key[0] = 0, room.version++;   // (no other with a key: nobody to remove)
    {
        bool changed = false;
        coop_refresh(&changed);
        CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopReset, kCoopLeave, kCoopRules}));
    }
    tab_selected[TAB_COOP] = 3;   // (on Reset the pools)
    room.offer = 78, room.accepted = 0, room.version++;   // (a new offer, a player came: the remedies and Room rules stand back
    {                                                      // until it is accepted, so that the members' keys have the room)
        bool changed = false;
        coop_refresh(&changed);
        CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopAccept, kCoopLeave}) && selected_kind() == kCoopSecret);
    }
    room.accepted = 1, room.version++;
    {
        bool changed = false;
        coop_refresh(&changed);
        CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopReset, kCoopLeave, kCoopRules}));
    }
    room.remedies = 0, room.version++;   // (not the maker's game: no remedies)
    {
        bool changed = false;
        coop_refresh(&changed);
        CHECK(shows({kCoopName, kCoopRelay, kCoopSecret, kCoopLeave}));
    }
    // ---- the Room rules page's values and what is said under them ----
    CHECK(!std::strcmp(tx_rule_shared(1), "Shared") && !std::strcmp(tx_rule_shared(0), "Per player"));
    CHECK(!std::strcmp(tx_rule_pool(2), "One pool") && !std::strcmp(tx_rule_pool(0), "Per player"));
    opts.coop_progress = 1;   // (a shared world: items, songs and quest items cannot be per player)
    CHECK(hn_rule_follows(0, -1) && !hn_rule_follows(1, -1) && !hn_rule_keys(2, -1) && !hn_rule_keys(0, -1));
    opts.coop_progress = 0;   // (each their own world: small keys cannot be a pool)
    CHECK(!hn_rule_follows(0, -1) && !hn_rule_follows(1, -1) && hn_rule_keys(2, -1) && !hn_rule_keys(0, -1));
    CHECK(hn_rule_follows(1, 0) && !hn_rule_follows(1, 1) && hn_rule_keys(0, 0) && !hn_rule_keys(0, 1));   // (the help box's height)

    // ---- the fields: what they take, and their Enter ----
    text_set(coop_relay_field, "");
    for (const char* s = "relay.example.org:24680 /@[::1]_x"; *s; s++) text_edit(coop_relay_field, (unsigned char)*s);
    CHECK(!std::strcmp(coop_relay_field.text, "relay.example.org:24680[::1]_x"));
    text_set(coop_join_field, "");
    for (const char* s = "k7xg2m-rtb9dh w4pz5n./!"; *s; s++) text_edit(coop_join_field, (unsigned char)*s);
    CHECK(!std::strcmp(coop_join_field.text, "k7xg2m-rtb9dh w4pz5n"));
    CHECK(coop_name_field.max == 24 && coop_relay_field.max == 63 && coop_join_field.max <= (int)sizeof coop_join_field.text - 1);
    n_asked = 0, saves = 0;
    answer = 0;   // (the host says it is not a secret)
    CHECK(coop_join_apply() && std::strstr(coop_join_apply(), "Not a room's secret") && n_asked == 2);
    CHECK(asked[0].cmd == HOS_COOP_CMD_JOIN && !std::strcmp(asked[0].text, "k7xg2m-rtb9dh w4pz5n") && coop_join_field.len == 20);
    answer = 1;
    CHECK(coop_join_apply() == nullptr && n_asked == 3 && coop_join_field.len == 0);   // (taken: emptied)
    n_asked = 0;
    CHECK(coop_join_apply() && std::strstr(coop_join_apply(), "Type the room's secret") && n_asked == 0);   // (empty: nothing asked)
    answer = 0;
    CHECK(coop_relay_apply() && std::strstr(coop_relay_apply(), "host or host:port") && saves == 0);   // (refused: not saved)
    CHECK(asked[0].cmd == HOS_COOP_CMD_SERVER && asked[0].number == 1);
    answer = 1, n_asked = 0, coop_relay_whole = false;
    CHECK(coop_relay_apply() == nullptr && saves == 1 && coop_relay_whole);
    text_set(coop_name_field, "Malon");
    CHECK(coop_name_apply() == nullptr && saves == 2 && asked[1].cmd == HOS_COOP_CMD_NAME && !std::strcmp(asked[1].text, "Malon") &&
          asked[1].number == 1);
    std::puts("PASS: the Co-op tab's rows by the room's state, its presses, its fields and its selection; the rules' words");
}
'''

HOST_FIXTURE = r'''#define _POSIX_C_SOURCE 200809L   /* (setenv) */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "coop.h"
#include "coop_state.h"
#include "../gpu/vk/vk_api.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
static void set_env(const char *name, const char *value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}
void hos_coop_menu_view(MkwVkCoopView *out);
int hos_coop_menu_command(int cmd, const char *text, uint32_t number);
static int64_t clock_ms;
int64_t hos_now_ns(void) { return clock_ms * 1000000; }
void hos_log(const char *fmt, ...) { (void)fmt; }
static const char *ini_server = "";
int hos_ini_text(const char *key, char *out, size_t size) {
    if (strcmp(key, "coop_server") || !*ini_server) return 0;
    snprintf(out, size, "%s", ini_server);
    return 1;
}
/* (the session, stubbed: what the tab asks of it) */
static CoopView session;
static int configures, commands, last_cmd, last_on;
static char last_name[64], last_server[64], last_secret[64];
void coop_view_get(CoopView *out) { *out = session; }
int coop_configure(int on, const char *name, const char *server) {
    configures++, last_on = on;
    snprintf(last_name, sizeof last_name, "%s", name), snprintf(last_server, sizeof last_server, "%s", server);
    if (session.state == COOP_OFF) session.state = COOP_LEFT;
    return 1;
}
int coop_command(CoopCmd cmd, const char *secret) {
    commands++, last_cmd = (int)cmd;
    snprintf(last_secret, sizeof last_secret, "%s", secret ? secret : "(null)");
    return session.state != COOP_OFF;
}
int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port) {   /* (host[:port], nothing else) */
    (void)cap;
    if (!*text || strpbrk(text, " /@")) return 0;
    host[0] = 0, *port = 24680;
    return 1;
}
/* (progress sync, stubbed: its offer, acceptance, remedies and the pools' list, as host/oot_coop.c gives them) */
static CoopOffer sync_offer;
static int sync_has, sync_maker, resets;
static uint32_t accepted_token;
static char removed_key[32];
int hos_oot_coop_offer(CoopOffer *out) { *out = sync_offer; return sync_has; }
int hos_oot_coop_accept(uint32_t token) { accepted_token = token; return sync_has && token == sync_offer.token; }
int hos_oot_coop_is_maker(void) { return sync_maker; }
int hos_oot_coop_reset_pools(void) { resets++; return sync_maker; }
int hos_oot_coop_remove_key(const char *key) { snprintf(removed_key, sizeof removed_key, "%s", key); return sync_maker; }
int hos_oot_coop_contributors(CoopContributor *out, int max) {
    if (max < 1) return 0;
    memset(out, 0, sizeof *out);
    out->self = 1, out->member = 2;
    snprintf(out->key, sizeof out->key, "4be1-07d2-9a3f-c615");
    return 1;
}
static void real(void) {
    MkwVkCoopView v;
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_OFF && v.n == 0 && v.offer == 0);
    /* (the saved name and relay at the start: kept, nothing switched on) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_NAME, "Malon", 0) && hos_coop_menu_command(HOS_COOP_CMD_SERVER, "", 0) && configures == 0);
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_SERVER, "relay host", 1) && configures == 0);   /* (not an address: refused) */
    /* (a field emptied: the options file's, as coop_init takes it) */
    ini_server = "relay.example.org";
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_JOIN, "not a secret", 0) && configures == 0 && commands == 0);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_JOIN, "k7xg2m rtb9dh-w4pz5n", 0));
    CHECK(configures == 1 && last_on == 1 && !strcmp(last_name, "Malon") && !strcmp(last_server, "relay.example.org"));
    CHECK(commands == 1 && last_cmd == COOP_CMD_JOIN && !strcmp(last_secret, "K7XG2MRTB9DHW4PZ5N"));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_SERVER, "10.0.0.2:24680", 1) && configures == 2 && !strcmp(last_server, "10.0.0.2:24680"));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_NAME, "Talon", 0) && configures == 2);   /* (0: kept, a running session left alone) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_MAKE, NULL, 0) && configures == 3 && !strcmp(last_name, "Talon") && last_cmd == COOP_CMD_CREATE &&
          !strcmp(last_secret, "(null)"));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_LEAVE, NULL, 0) && last_cmd == COOP_CMD_LEAVE && configures == 3);
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_ACCEPT, NULL, 5));   /* (progress sync has no offer) */
    /* (the room as the tab gets it) */
    session.state = COOP_JOINED, session.self = 2, session.maker = 1, session.n = 2, session.ping_ms = 31, session.version = 9;
    snprintf(session.code, sizeof session.code, "K7XG2M-RTB9DH-W4PZ5N");
    snprintf(session.status, sizeof session.status, "in the room");
    session.member[0].id = 1, snprintf(session.member[0].name, sizeof session.member[0].name, "Malon");
    session.member[1].id = 2, session.member[1].away = 1, snprintf(session.member[1].name, sizeof session.member[1].name, "Talon");
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_JOINED && v.n == 2 && v.ping_ms == 31 && v.version == 9 && !strcmp(v.code, "K7XG2M-RTB9DH-W4PZ5N"));
    CHECK(v.member[0].maker && !v.member[0].self && v.member[1].self && !v.member[1].maker && v.member[1].away);
    CHECK(!strcmp(v.member[0].name, "Malon") && !v.member[0].key[0] && !v.offer && !strcmp(v.status, "in the room"));
    CHECK(!strcmp(v.member[1].key, "4be1-07d2-9a3f-c615") && !v.remedies);   /* (its own key, from the pools' list) */
    /* (progress sync's offer: the others' keys, the token to accept, accepted, the maker's remedies) */
    sync_has = 1, sync_offer.token = 77, sync_offer.n = 1, sync_offer.member[0].id = 1;
    snprintf(sync_offer.member[0].key, sizeof sync_offer.member[0].key, "a9e0-31c7-6d58-f2b4");
    hos_coop_menu_view(&v);
    CHECK(v.offer == 77 && !v.accepted && !strcmp(v.member[0].key, "a9e0-31c7-6d58-f2b4") && !v.member[0].accepted);
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_ACCEPT, NULL, 78) && hos_coop_menu_command(HOS_COOP_CMD_ACCEPT, NULL, 77) && accepted_token == 77);
    sync_offer.accepted = 1, sync_maker = 1;
    hos_coop_menu_view(&v);
    CHECK(v.accepted && v.member[0].accepted && !v.member[1].accepted && v.remedies);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_REMOVE, "a9e0-31c7-6d58-f2b4", 0) && !strcmp(removed_key, "a9e0-31c7-6d58-f2b4"));
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_REMOVE, "", 0) && hos_coop_menu_command(HOS_COOP_CMD_RESET_POOLS, NULL, 0) && resets == 1);
    session.state = COOP_LEFT, session.n = 0;   /* (out of the room: progress sync is not asked) */
    hos_coop_menu_view(&v);
    CHECK(!v.offer && !v.remedies && v.n == 0);
    /* (Make with HOS_COOP_ROOM in the environment: that room, as a test session's second game knows it) */
    set_env("HOS_COOP_ROOM", "k7xg2m rtb9dh w4pz5n");
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_MAKE, NULL, 0) && last_cmd == COOP_CMD_CREATE && !strcmp(last_secret, "K7XG2MRTB9DHW4PZ5N"));
    set_env("HOS_COOP_ROOM", NULL);
    puts("PASS: the tab's commands reach the session and progress sync; its view of the room");
}
static void demo(void) {
    MkwVkCoopView v;
    clock_ms = 1000;
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_OFF && strstr(v.status, "made-up"));
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_LEAVE, NULL, 0) && !hos_coop_menu_command(HOS_COOP_CMD_JOIN, "nope", 0));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_NAME, "Link", 1) && hos_coop_menu_command(HOS_COOP_CMD_MAKE, NULL, 0));
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_CONNECTING && strlen(v.code) == 20 && v.code[6] == '-' && v.n == 0);
    const uint32_t was = v.version;
    clock_ms += 1300;   /* (in the room: the maker, alone) */
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_JOINED && v.version != was && v.n == 1 && v.member[0].self && v.member[0].maker && v.member[0].key[0]);
    CHECK(!strcmp(v.name, "Link") && !strcmp(v.member[0].name, "Link") && !v.offer);
    clock_ms += 2000;   /* (Saria, not identified yet: no offer) */
    hos_coop_menu_view(&v);
    CHECK(v.n == 2 && !v.member[1].key[0] && v.unidentified && !v.offer);
    clock_ms += 3000;   /* (her key, Darunia's; the room offered) */
    hos_coop_menu_view(&v);
    CHECK(v.n == 3 && v.member[1].key[0] && v.member[2].key[0] && !v.unidentified && v.offer && !v.accepted && !v.member[2].away);
    clock_ms += 3500;
    hos_coop_menu_view(&v);
    CHECK(v.member[2].away);
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_ACCEPT, NULL, v.offer + 1));   /* (another offer's number) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_ACCEPT, NULL, v.offer));
    hos_coop_menu_view(&v);
    CHECK(v.accepted && v.member[1].accepted && v.member[2].accepted && !v.member[0].accepted);
    CHECK(v.remedies && hos_coop_menu_command(HOS_COOP_CMD_REMOVE, v.member[2].key, 0) && !hos_coop_menu_command(HOS_COOP_CMD_REMOVE, "nope", 0));
    hos_coop_menu_view(&v);
    CHECK(v.n == 2 && strcmp(v.member[1].name, "Darunia") && hos_coop_menu_command(HOS_COOP_CMD_RESET_POOLS, NULL, 0));
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_ACCEPT, NULL, v.offer) && !hos_coop_menu_command(HOS_COOP_CMD_MAKE, NULL, 0));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_LEAVE, NULL, 0));
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_LEFT && v.n == 0 && !v.code[0] && !v.offer);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_JOIN, "k7xg2m-rtb9dh-w4pz5n", 0));   /* (third in Malon's room, offered at once) */
    clock_ms += 1300;
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_JOINED && v.n == 3 && v.member[0].maker && !v.member[0].self && v.member[2].self && v.offer);
    CHECK(!strcmp(v.code, "K7XG2M-RTB9DH-W4PZ5N"));
    CHECK(configures == 0 && commands == 0);   /* (the session never asked) */
    puts("PASS: HOS_COOP_DEMO=1's made-up room through its states, the session never started");
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "demo")) demo();
    else real();
    return 0;
}
'''

RULES_FIXTURE = r'''#include <stdio.h>
#include <string.h>
#include "coop_proto.h"
#include "coop_state.h"
/* (a Room rules row as the source has it: the options file's key, its range and step, its default) */
static const struct { const char *key; int lo, hi, step, def; } row[] = {
@@TABLE@@
};
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s (category %d)\n", __LINE__, #x, c); return 1; } } while (0)
int main(void) {
    CoopRules d;
    coop_rules_default(&d);
    int c = -1;
    CHECK(sizeof row / sizeof *row == COOP_CAT_N);
    for (c = 0; c < COOP_CAT_N; c++) {
        char key[32];
        snprintf(key, sizeof key, "coop_%s", coop_cat_name(c));
        CHECK(!strcmp(row[c].key, key));                                   /* (what host/oot_coop.c read_rules asks the file for) */
        CHECK(row[c].lo == COOP_MODE_OWN);
        CHECK(row[c].hi == (coop_cat_is_pool(c) ? COOP_MODE_POOL : COOP_MODE_SHARED));
        CHECK(row[c].step == row[c].hi - row[c].lo);                       /* (the two modes the category has, nothing between) */
        CHECK(row[c].def == d.mode[c]);                                    /* (a menu that saves leaves a room's rules the defaults) */
    }
    puts("PASS: the Room rules page's rows are the engine's categories, modes and defaults");
    return 0;
}
'''


def run(cmd, env=None):
    subprocess.run(cmd, check=True, env=env)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    backend = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    if "HOS_COOP_STATE_OFF" not in api:
        raise SystemExit("FAIL: vk_api.h: no Co-op tab's view (MkwVkCoopView)")
    api_cut = between(api, "enum { HOS_TEXT_KEY", "\n") + "\n" + between(api, "enum { HOS_COOP_STATE_OFF", "/* what the executable gives")
    cut = {
        "@@API@@": api_cut,
        "@@TABS@@": re.search(r"enum MenuTab \{.*?\};", menu, re.S).group(0),
        "@@ROW@@": block(menu, "struct Row {"),
        "@@TEXT@@": "\n".join([block(menu, "struct TextField {"), between(menu, "enum { kTextGoOn", "\n") + "\n",
                               block(menu, "int text_edit("), block(menu, "void text_set(")]),
        "@@RULES@@": between(menu, "const char* tx_rule_shared(int v)", "const char* tx_onoff(int v)"),
        "@@ROWS@@": between(menu, "    // (hookshot) Co-op (host/coop_menu.c)", "    // (hookshot) the Gameplay tab's Bug report"),
        "@@COOP@@": between(menu, "MkwVkCoopView coop_now{};", "// ---- the rows that take a restart"),
        "@@ROWMAP@@": block(menu, "int tab_row_to_global("),
        "@@APPLY@@": between(menu, "/// (hookshot) Enter on the Co-op tab's Your name", "/// Custom output size: the Output"),
        "@@PRESS@@": between(menu, "/// (hookshot) Enter (or a click) on a Co-op row", "// ---- (hookshot) the Controls page"),
    }
    source = MENU_FIXTURE
    for k, v in cut.items():
        source = source.replace(k, v)
    with tempfile.TemporaryDirectory(prefix="menu-coop-") as tmp:
        tmp = Path(tmp)
        (tmp / "menu.cpp").write_text(source, encoding="utf-8")
        run([a.cxx, "-std=c++20", "-D_CRT_SECURE_NO_WARNINGS", "-Wall", "-Wextra", "-Werror",
             "-Wno-missing-field-initializers", str(tmp / "menu.cpp"), "-o", str(tmp / "menu")])
        run([str(tmp / "menu")])
        (tmp / "host").mkdir()
        (tmp / "host" / "fixture.c").write_text(HOST_FIXTURE, encoding="utf-8")
        mono = ROOT / "third_party" / "monocypher"
        run([a.cc, "-std=c99", "-O1", "-w", "-c", str(mono / "monocypher.c"), "-o", str(tmp / "monocypher.o")])
        run([a.cc, "-std=c11", "-D_CRT_SECURE_NO_WARNINGS", "-Wall", "-Wextra", "-Werror",
             "-I" + str(ROOT / "host"), "-I" + str(mono), str(tmp / "host" / "fixture.c"),
             str(ROOT / "host" / "coop_menu.c"), str(ROOT / "host" / "coop_proto.c"), str(tmp / "monocypher.o"),
             "-o", str(tmp / "host_test")])
        env = {k: v for k, v in os.environ.items() if not k.startswith("HOS_COOP")}
        run([str(tmp / "host_test")], env=env)
        run([str(tmp / "host_test"), "demo"], env=dict(env, HOS_COOP_DEMO="1"))
        # ---- the Room rules page against the engine: the rows as the source has them ----
        header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
        order = re.findall(r'\{&opts\.(\w+), "(\w+)"\}', between(menu, "} coop_options[] = {", "};"))
        if len(order) != 8 or order[0] != ("coop", "coop"):
            raise SystemExit("FAIL: vk_menu.cpp: coop_options is not co-op at the start and the seven rules")
        table = []
        for field, key in order[1:]:
            r = re.search(r'\{TAB_RULES, "[^"]+", &opts\.%s, (\d+), (\d+), (\d+), tx_rule_(?:shared|pool),' % field, menu)
            default = re.search(r"\b%s = (\d+)[,;]" % field, header)
            if not (r and default):
                raise SystemExit("FAIL: the Room rules page has no row or default for " + field)
            table.append('    {"%s", %s, %s, %s, %s},' % (key, *r.groups(), default.group(1)))
        (tmp / "rules.c").write_text(RULES_FIXTURE.replace("@@TABLE@@", "\n".join(table)), encoding="utf-8")
        run([a.cc, "-std=c11", "-D_CRT_SECURE_NO_WARNINGS", "-Wall", "-Wextra", "-Werror",
             "-I" + str(ROOT / "host"), "-I" + str(mono), str(tmp / "rules.c"), str(ROOT / "host" / "coop_state.c"),
             str(ROOT / "host" / "coop_proto.c"), str(tmp / "monocypher.o"), "-o", str(tmp / "rules")])
        run([str(tmp / "rules")])

    # ---- the wiring ----
    version = int(re.search(r"#define MKWVK_API_VERSION (\d+)", api).group(1))
    host_struct = between(api, "typedef struct MkwVkHost {", "} MkwVkHost;")
    members = [l.strip() for l in host_struct.splitlines() if l.strip().endswith(");") and "(*" in l]
    if members[-3:] != ["int (*take_text)(int focus, uint32_t *out, int max);", "void (*coop_view)(MkwVkCoopView *out);",
                        "int (*coop_command)(int cmd, const char *text, uint32_t number);"]:
        raise SystemExit("FAIL: vk_api.h: coop_view and coop_command are not MkwVkHost's last members, after take_text "
                         "(host/rx_backend.c fills it by position)")
    if version < 23:
        raise SystemExit("FAIL: vk_api.h: MkwVkHost grew without an MKWVK_API_VERSION bump")
    need(backend, r"host_take_text, hos_coop_menu_view, hos_coop_menu_command,\s*\};", "rx_backend.c: vk_host does not end with the Co-op tab's two")
    need(menu, r"TAB_DEBUG = 4,[^\n]*\n\s*TAB_COOP = 5,[^\n]*\n\s*TAB_COUNT = 6,", "vk_menu.cpp: the Co-op tab is not the strip's last")
    need(menu, r'tab_titles\[TAB_COUNT\] = \{"Graphics", "Audio", "Gameplay", "Controls", "Debug", "Co-op"\}', "vk_menu.cpp: no Co-op title")
    slots = re.search(r"int scrolled_to\[TAB_RULES \+ 1\] = \{([^}]*)\}", menu).group(1)
    if slots.count("-1") != 9:
        raise SystemExit("FAIL: vk_menu.cpp: scrolled_to does not start every slot at -1 (TAB_RULES + 1 = 9 of them)")
    need(menu, r"TAB_PAGE = TAB_MOTION \+ 1,[^\n]*\n\s*TAB_RULES = TAB_PAGE \+ 1\b", "vk_menu.cpp: the Room rules page has no slot of its own")
    for call in ("key == row_key(r, ini_key(r))", 'fprintf(f, "%s=%d\\n", row_key(r, ini_key(r)), saved_value(r.value))'):
        if call not in block(menu, "void save() {"):
            raise SystemExit("FAIL: save: a Room rules row is not written under the key host/oot_coop.c reads")
    need(block(menu, "void menu_init() {"), r"if \(coop_option_line\(line, eq \+ 1\)\) continue;[^\n]*\n\s*for \(Row& r : rows\) \{",
         "menu_init: a Room rules row is not read by its key")
    need(block(menu, "bool coop_option_line("), r'if \(!strcmp\(key, row_key\(r, ""\)\)\) \*r\.value = std::min\(std::max\(atoi\(text\), r\.lo\), r\.hi\);',
         "coop_option_line: a Room rules row's value is not taken from its key's line")
    # (the control bridge, docs/control.md, finds and names a setting by its key in the options file: a Room rules row's too)
    if 'snprintf(out->key, sizeof out->key, "%s", row_key(r, ini_key(r)));' not in block(menu, "void bridge_fill(") or \
            "!strcmp(row_key(rows[i], ini_key(rows[i])), key)" not in block(menu, "int menu_setting_set("):
        raise SystemExit("FAIL: the control bridge does not name and find a Room rules row by its key (coop_keys, ...)")
    need(block(menu, "void bridge_fill("), r"static_assert\(sizeof tabs / sizeof tabs\[0\] == TAB_RULES \+ 1",
         "bridge_fill: the control bridge has no tab name for the Co-op tab and the Room rules page")
    need(menu, r'\{TAB_RULES, "Co-op at start", &opts\.coop, 0, 1, 1, tx_onoff,', "vk_menu.cpp: no Co-op at start row")
    need(block(menu, "bool row_shown(int i) {"), r"\(rows\[i\]\.tab != TAB_COOP \|\| coop_row_shown\(rows\[i\], coop_now\)\)",
         "row_shown: the Co-op tab's rows do not follow the room")
    need(menu, r'\{"Your name", &coop_name_field, coop_name_apply\},\s*\{"Relay", &coop_relay_field, coop_relay_apply\},\s*'
               r'\{"Join a room", &coop_join_field, coop_join_apply\}\};', "vk_menu.cpp: the Co-op tab's fields are not text fields")
    tick = block(menu, "void menu_tick() {")
    need(tick, r"if \(open_now\.load\(\)\) coop_refresh\(&changed\);", "menu_tick: the room is not read while the menu is open")
    need(tick, r'!strcmp\(key, "ROW"\) && next\[n\] == \':\'.*?if \(want == rows\[i\]\.name\) tab_selected\[cur_tab\] = k',
         "menu_tick: RX_VK_MENU_SCRIPT has no ROW:<name> (a scripted session must choose Accept by name, not by counting rows)")
    if tick.index("else if (press[7] && coop_kind(r) >= 0) coop_press(r)") > tick.index("else if (press[7] && !r.value && can_bind())"):
        raise SystemExit("FAIL: menu_tick: the Co-op tab's rows would open the Controls page")
    need(tick, r"pending != &r \|\| \(!mine && coop_kind\(\*pending\) < 0\)", "menu_tick: Accept and Leave are not armed without the focus")
    need(tick, r"else if \(press\[7\] && coop_kind\(r\) == kCoopRules\) cur_tab = TAB_RULES, tab_selected\[TAB_RULES\] = 0, press\[7\] = false",
         "menu_tick: Room rules does not open its page on confirm")
    if tick.index("coop_kind(r) == kCoopRules) cur_tab = TAB_RULES") > tick.index("else if (press[7] && coop_kind(r) >= 0) coop_press(r)"):
        raise SystemExit("FAIL: menu_tick: Room rules would be pressed as a room action")
    need(tick, r"if \(press\[5\] && cur_tab == TAB_RULES\) cur_tab = TAB_COOP, press\[5\] = false", "menu_tick: back does not leave the Room rules page")
    need(tick, r"if \(\(tab_left \|\| tab_right\) && cur_tab == TAB_RULES\) cur_tab = TAB_COOP;", "menu_tick: the tabs' keys from the Room rules page index past the strip")
    need(block(menu, "void menu_init() {"), r"if \(opts\.coop_keys == 1\) opts\.coop_keys = 2;\s*if \(opts\.coop_rupees == 1\) opts\.coop_rupees = 0;\s*"
         r"if \(opts\.coop_ammo == 1\) opts\.coop_ammo = 0;", "menu_init: a mode a pool row does not have is shown and saved as it is")
    need(block(menu, "void save() {"), r'key == "coop_name" \|\|\s*\(key == "coop_server" && coop_relay_whole\).*'
         r'fprintf\(f, "coop_name=%s\\n", coop_name_field\.text\).*fprintf\(f, "coop_server=%s\\n", coop_relay_field\.text\)',
         "save: hookshot_options.ini's coop_name= and coop_server= (host/coop.c reads them at the start) are not the tab's")
    need(block(menu, "void menu_init() {"), r'!strcmp\(line, "coop_name"\) \|\| !strcmp\(line, "coop_server"\).*'
         r'host->coop_command\(HOS_COOP_CMD_NAME, coop_name_field\.text, 0\)', "menu_init: the saved name and relay are not read")
    need(block(menu, "void draw_options(ui::Ctx& c) {"), r"if \(cur_tab == TAB_COOP\) \{.*?draw_coop_rows\(", "draw_options: no Co-op layout")
    print("PASS: the Co-op tab's wiring: the interface's coop_view and coop_command, the host's table, the strip, the Room "
          "rules page, the options file")


if __name__ == "__main__":
    main()
