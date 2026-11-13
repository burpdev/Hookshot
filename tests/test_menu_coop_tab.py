"""The Insert menu's Co-op tab: one way into a co-op room, from the menu (host/coop.h's session).

The menu's side is cut out of gpu/vk/vk_menu.cpp by its text and driven as menu_tick drives it: which rows the tab shows in
each state of the room (Host, Join, Host address, Port, Player name and Status always; Room rules outside a room and to the
game that holds its key; the host's remedies while it has them and another player has a key), what each press asks of the
host (Host and Join at once outside a room, at the second press inside one, since leaving is what the others are playing
in; Remove a player and Reset the pools always at the second), which of the others Left and Right choose for Remove, the name and host address fields (what they take, an address
the host refuses not kept, a name told to the room this game is in), and the selection staying on its row while rows come
and go. The host's side, host/coop_menu.c, is compiled as C with the wire format (host/coop_proto.c, Monocypher) and the
session stubbed: the view as the tab gets it (you, the room's maker, the others' keys), the commands turned into
coop_configure, coop_command and coop_set_port (a name or an address kept at the start never restarts a running session;
Join with nothing to join switches nothing on; the row of the room this game is in leaves it), and HOS_COOP_DEMO=1's made-up
room walked through on a made-up clock. The Room rules page (what a room this game makes shares) is checked against the
engine itself: a C program with host/coop_state.h's categories, modes and defaults and the rows' keys, ranges, steps and
defaults as the source has them. Then the wiring is checked in the source: the interface's two members last and its version
bumped, the host's table, the tab in the strip, the Port row's key and its command, the options file's coop_name=,
coop_server= and coop_port=. No GPU, game, window or network.

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
// (the Co-op tab's two rows that have a value of their own: the Port and PvP. The words under them, as vk_menu.cpp's)
static int coop_port = 43400, coop_pvp = 0;
const char* tx_onoff(int v) { return v ? "On" : "Off"; }
const char* tx_port(int v) { static char t[16]; std::snprintf(t, sizeof t, "%d", v); return t; }
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
/// The room's own rows as shown, by kind, ending with -1 (Go to player, Port and PvP are not the room's rows)
static void shown(int* out) {
    int n = 0;
    for (int i = 0; i < kRows; i++)
        if (rows[i].tab == TAB_COOP && row_shown(i) && coop_kind(rows[i]) >= 0) out[n++] = coop_kind(rows[i]);
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
/// A room row's place on the tab, as tab_selected counts it (shown rows of the tab, in their order)
static int tab_index(int kind) {
    int n = 0;
    for (int i = 0; i < kRows; i++) {
        if (rows[i].tab != TAB_COOP || !row_shown(i)) continue;
        if (coop_kind(rows[i]) == kind) return n;
        n++;
    }
    std::fprintf(stderr, "FAIL: no room row of kind %d is shown\n", kind);
    std::exit(1);
}
/// The room as host/coop_menu.c would give it: its state, who holds it, its offer and whether it was accepted, the maker's
/// remedies, and n_other others (the first Saria with her key, the second Darunia with his)
static void set_room(int state, int hosting, int remedies, int n_other, int with_keys) {
    room = MkwVkCoopView{};
    room.state = state, room.hosting = hosting, room.remedies = remedies;
    room.port = 43400, room.version = (uint32_t)(state * 100 + remedies + n_other);
    std::snprintf(room.status, sizeof room.status, hosting ? "Hosting" : "Joined");
    std::snprintf(room.name, sizeof room.name, "You");
    std::snprintf(room.addr, sizeof room.addr, "192.168.1.20:43400");
    room.n = 1 + n_other;
    room.member[0].id = 1, room.member[0].self = 1, room.member[0].maker = (uint8_t)hosting;
    room.member[0].accepted = (uint8_t)with_keys;   /* (accepted as soon as the keys are in the room) */
    std::snprintf(room.member[0].name, sizeof room.member[0].name, "You");
    std::snprintf(room.member[0].key, sizeof room.member[0].key, with_keys ? "4be1-07d2-9a3f-c615" : "");
    for (int i = 0; i < n_other; i++) {
        MkwVkCoopMember& m = room.member[1 + i];
        m.id = 2 + i;
        m.accepted = (uint8_t)with_keys;
        std::snprintf(m.name, sizeof m.name, i ? "Darunia" : "Saria");
        std::snprintf(m.key, sizeof m.key, with_keys ? (i ? "d03a-5f17-c2e9-8b44" : "7c1d-e842-3b90-a5f6") : "");
    }
    bool changed = false;
    coop_refresh(&changed);
    CHECK(changed);
}
static void press_twice(const Row& r) { coop_press(r), coop_press(r); }

int main() {
    // ---- the tab's rows ----
    int named = 0, other = 0;
    for (const Row& r : rows) {
        if (r.tab != TAB_COOP) continue;
        if (coop_kind(r) >= 0) { named++; CHECK(r.help && std::strlen(r.help) > 20); continue; }
        other++;   // (Go to player, Port and PvP: the tab's rows that are not the room's own)
        CHECK(!std::strcmp(r.name, "Go to player") || !std::strcmp(r.name, "PvP") || !std::strcmp(r.name, "Port"));
    }
    CHECK(named == kCoopRows && other == 9);
    CHECK(coop_kind(rows[0]) == -1 && rows[0].tab != TAB_COOP);
    int valued = 0;
    for (const Row& r : rows)
        if (r.tab == TAB_COOP && r.value) { valued++; CHECK(r.value == &coop_port || r.value == &coop_pvp); }
    CHECK(valued == 2);   // (Port and PvP: every other row of the tab has no value of its own)

    // ---- the rows by the room's state ----
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopRules}));   // (before anything)
    set_room(HOS_COOP_STATE_LEFT, 0, 0, 0, 0);
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopRules}));
    tab_selected[TAB_COOP] = tab_index(kCoopRules);   // (on Room rules)
    set_room(HOS_COOP_STATE_CONNECTING, 1, 0, 0, 0);
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus}) && selected_kind() == kCoopAddress);   // (its row gone: the third of the tab)
    set_room(HOS_COOP_STATE_JOINED, 1, 1, 0, 1);   // (hosting, alone, this game's key known)
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopReset, kCoopRules}));
    set_room(HOS_COOP_STATE_JOINED, 1, 1, 1, 1);   // (Saria in, with her key: a player can be removed)
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopRemove, kCoopReset, kCoopRules}));
    set_room(HOS_COOP_STATE_JOINED, 1, 1, 2, 1);   // (the room shares at once: nothing to accept, the remedies are the room's rows)
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopRemove, kCoopReset, kCoopRules}));
    set_room(HOS_COOP_STATE_JOINED, 0, 0, 2, 1);   // (joined another player's room, and not its maker: no remedies)
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus}));
    set_room(HOS_COOP_STATE_RECONNECTING, 1, 0, 0, 0);   // (nothing known, while reconnecting)
    CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus}));
    {   // (the same view again: nothing changed, nothing to draw)
        bool changed = false;
        coop_refresh(&changed);
        CHECK(!changed);
    }

    // ---- what each press asks of the host ----
    set_room(HOS_COOP_STATE_LEFT, 0, 0, 0, 0);
    n_asked = 0;
    coop_press(row(kCoopHost));   // (outside a room: at once, and with nothing to say: host/coop_menu.c listens on its own)
    CHECK(n_asked == 1 && asked[0].cmd == HOS_COOP_CMD_HOST && !std::strcmp(asked[0].text, "(null)") && !pending);
    text_set(coop_addr_field, "10.0.0.2:43400");
    coop_press(row(kCoopJoin));
    CHECK(n_asked == 2 && asked[1].cmd == HOS_COOP_CMD_JOIN && !std::strcmp(asked[1].text, "10.0.0.2:43400") && !pending);
    set_room(HOS_COOP_STATE_JOINED, 1, 1, 0, 1);   // (in a room this game holds: leaving is asked for twice)
    n_asked = 0;
    coop_press(row(kCoopHost));
    CHECK(pending == &row(kCoopHost) && n_asked == 0);
    coop_press(row(kCoopJoin));   // (another row: arms that one, the first disarmed)
    CHECK(pending == &row(kCoopJoin) && n_asked == 0);
    coop_press(row(kCoopJoin));
    CHECK(!pending && n_asked == 1 && asked[0].cmd == HOS_COOP_CMD_JOIN);
    pending = &row(kCoopHost);
    set_room(HOS_COOP_STATE_LEFT, 0, 0, 0, 0);   // (the room went: what was armed for it goes with it)
    CHECK(!pending);
    set_room(HOS_COOP_STATE_JOINED, 1, 1, 2, 1);
    coop_remove_at = 0, n_asked = 0;
    press_twice(row(kCoopRemove));
    CHECK(n_asked == 1 && asked[0].cmd == HOS_COOP_CMD_REMOVE && !std::strcmp(asked[0].text, "7c1d-e842-3b90-a5f6"));
    coop_remove_at = -1;   // (Left from the first: the last)
    press_twice(row(kCoopRemove));
    CHECK(n_asked == 2 && !std::strcmp(asked[1].text, "d03a-5f17-c2e9-8b44"));
    coop_remove_at = 98;   // (Left and Right wrap: the count is how many have a key)
    press_twice(row(kCoopRemove));
    CHECK(n_asked == 3 && !std::strcmp(asked[2].text, "7c1d-e842-3b90-a5f6"));
    n_asked = 0;
    press_twice(row(kCoopReset));
    CHECK(n_asked == 1 && asked[0].cmd == HOS_COOP_CMD_RESET_POOLS);
    coop_press(row(kCoopRules));   // (the page, not a room action: menu_tick opens it, coop_press asks nothing)
    CHECK(n_asked == 1 && !pending);
    {   // (nobody left to remove: the row is not shown, and pressing it would ask for nothing)
        room.member[1].key[0] = 0, room.member[2].key[0] = 0, room.version++;
        bool changed = false;
        coop_refresh(&changed);
        CHECK(changed && !shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopRemove, kCoopReset, kCoopRules}));
        CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopReset, kCoopRules}));
        const MkwVkCoopMember* chosen = nullptr;
        CHECK(coop_removable(coop_now, &chosen) == 0 && !chosen);
    }
    tab_selected[TAB_COOP] = tab_index(kCoopReset);   // (on Reset the pools)
    room.remedies = 0, room.version++;   // (this game does not hold the room's key: the remedies stand back)
    {
        bool changed = false;
        coop_refresh(&changed);
        CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus}));
        CHECK(selected_kind() == kCoopAddress);   // (its row gone: the third of the tab, after Host and Join)
    }
    tab_selected[TAB_COOP] = tab_index(kCoopName);   // (on Player name, which is always there)
    std::snprintf(room.member[1].key, sizeof room.member[1].key, "7c1d-e842-3b90-a5f6");   // (Saria's key is known again)
    room.remedies = 1, room.version++;   // (the key is back: the remedies are back)
    {
        bool changed = false;
        coop_refresh(&changed);
        CHECK(shows({kCoopHost, kCoopJoin, kCoopAddress, kCoopName, kCoopStatus, kCoopRemove, kCoopReset, kCoopRules}));
        CHECK(selected_kind() == kCoopName);   // (kept on its row while the others came and went)
    }

    // ---- the Room rules page's values and what is said under them ----
    CHECK(!std::strcmp(tx_rule_shared(1), "Shared") && !std::strcmp(tx_rule_shared(0), "Per player"));
    CHECK(!std::strcmp(tx_rule_pool(2), "One pool") && !std::strcmp(tx_rule_pool(0), "Per player"));
    opts.coop_progress = 1;   // (a shared world: items, songs and quest items cannot be per player)
    CHECK(hn_rule_follows(0, -1) && !hn_rule_follows(1, -1) && !hn_rule_keys(2, -1) && !hn_rule_keys(0, -1));
    opts.coop_progress = 0;   // (each their own world: small keys cannot be a pool)
    CHECK(!hn_rule_follows(0, -1) && !hn_rule_follows(1, -1) && hn_rule_keys(2, -1) && !hn_rule_keys(0, -1));
    CHECK(hn_rule_follows(1, 0) && !hn_rule_follows(1, 1) && hn_rule_keys(0, 0) && !hn_rule_keys(0, 1));   // (the help box's height)

    // ---- the two fields: what they take, and their Enter ----
    text_set(coop_addr_field, "");
    for (const char* s = "192.168.1.20:43400 [2001:db8::7]_x /@!"; *s; s++) text_edit(coop_addr_field, (unsigned char)*s);
    CHECK(!std::strcmp(coop_addr_field.text, "192.168.1.20:43400[2001:db8::7]_x"));   // (a host, host:port or [IPv6]: no spaces)
    text_set(coop_name_field, "");
    for (const char* s = "Link66 Link!"; *s; s++) text_edit(coop_name_field, (unsigned char)*s);
    CHECK(!std::strcmp(coop_name_field.text, "Link66 Link!"));   // (any printable ASCII: host/coop.c keeps what is not a control character)
    text_edit(coop_name_field, 9u);   // (a tab: left out)
    CHECK(coop_name_field.len == 12 && !std::strcmp(coop_name_field.text, "Link66 Link!"));
    CHECK(coop_name_field.max == 24 && coop_addr_field.max == 63);
    CHECK(coop_name_field.max <= (int)sizeof coop_name_field.text - 1 && coop_addr_field.max <= (int)sizeof coop_addr_field.text - 1);
    n_asked = 0, saves = 0, answer = 0;
    text_set(coop_addr_field, "two words");
    const char* why = coop_addr_apply();
    CHECK(why && std::strstr(why, "host or host:port") && saves == 0 && n_asked == 1 && coop_addr_whole);   // (refused: nothing saved, the file's line left as it was)
    CHECK(asked[0].cmd == HOS_COOP_CMD_ADDRESS && asked[0].number == 1);   // (the field's Enter, applied to a session that runs)
    answer = 1, n_asked = 0, coop_addr_whole = false;
    text_set(coop_addr_field, "coop.example.org:43400");
    CHECK(coop_addr_apply() == nullptr && saves == 1 && coop_addr_whole && n_asked == 1);   // (taken: kept in the file)
    n_asked = 0;
    text_set(coop_name_field, "Malon");
    CHECK(coop_name_apply() == nullptr && saves == 2 && asked[0].cmd == HOS_COOP_CMD_NAME && !std::strcmp(asked[0].text, "Malon") &&
          asked[0].number == 1);
    std::puts("PASS: the Co-op tab's rows by the room's state, its presses, its fields and its selection; the rules' words");
    return 0;
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
int hos_ini_option(const char *key, int otherwise) { (void)key; return otherwise; }
static const char *ini_name = "", *ini_server = "";
int hos_ini_text(const char *key, char *out, size_t size) {
    const char *v = !strcmp(key, "coop_name") ? ini_name : !strcmp(key, "coop_server") ? ini_server : "";
    if (!*v) return 0;
    snprintf(out, size, "%s", v);
    return 1;
}
/* (the session, stubbed: what the tab asks of it) */
static CoopView session;
static int configures, commands, last_cmd, last_on, last_port, sets;
static char last_name[64], last_server[160], last_text[160];
void coop_view_get(CoopView *out) { *out = session; }
int coop_configure(int on, const char *name, const char *server) {
    configures++, last_on = on;
    snprintf(last_name, sizeof last_name, "%s", name), snprintf(last_server, sizeof last_server, "%s", server);
    if (session.state == COOP_OFF) session.state = COOP_LEFT;
    return 1;
}
int coop_command(CoopCmd cmd, const char *text) {
    commands++, last_cmd = (int)cmd;
    snprintf(last_text, sizeof last_text, "%s", text ? text : "(null)");
    return session.state != COOP_OFF;
}
void coop_set_port(uint16_t port) { sets++, last_port = port; }
int coop_server_parse(const char *text, char *host, size_t cap, uint16_t *port) {   /* (host[:port], nothing else) */
    (void)cap;
    if (!*text || strpbrk(text, " /@")) return 0;
    host[0] = 0, *port = 43400;
    return 1;
}
/* (progress sync, stubbed: its offer, acceptance, remedies and the pools' list, as host/coop_oot.c gives them) */
static CoopOffer sync_offer;
static int sync_has, sync_maker, resets, sync_refused;
static char removed_key[32];
int hos_oot_coop_offer(CoopOffer *out) { *out = sync_offer; return sync_has; }
int hos_oot_coop_is_maker(void) { return sync_maker; }
int hos_oot_coop_reset_pools(void) {   /* (host/coop_oot.c: the maker's, and only the maker's is asked for) */
    if (sync_maker) resets++;
    else sync_refused++;
    return sync_maker;
}
int hos_oot_coop_remove_key(const char *key) { snprintf(removed_key, sizeof removed_key, "%s", key); return sync_maker; }
int hos_oot_coop_contributors(CoopContributor *out, int max) {
    if (max < 1) return 0;
    memset(out, 0, sizeof *out);
    out->self = 1, out->member = 1;
    snprintf(out->key, sizeof out->key, "4be1-07d2-9a3f-c615");
    return 1;
}
static void real(void) {
    set_env("HOS_COOP_SERVER", NULL), set_env("HOS_COOP_NAME", NULL), set_env("HOS_COOP_PORT", NULL);
    MkwVkCoopView v;
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_OFF && v.n == 0 && !v.remedies && !v.hosting);
    /* (the saved name and address at the start (number 0): kept for the next Host or Join, nothing switched on) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_NAME, "Malon", 0) && hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "", 0) &&
          configures == 0 && commands == 0);
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "two words", 0) && configures == 0);   /* (not an address: refused) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "coop.example.org:43400", 0) && configures == 0);
    /* (the Port row: its digits to coop_set_port; a number it cannot be, refused; empty, the default) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_PORT, "43410", 0) && last_port == 43410 && sets == 1 && configures == 0);
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_PORT, "0", 0) && !hos_coop_menu_command(HOS_COOP_CMD_PORT, "70000", 0) &&
          last_port == 43410);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_PORT, "", 0) && last_port == 0);   /* (0: host/coop.h's default) */
    /* (Join with nothing to join: refused before co-op is switched on, not a room half started) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "", 0));
    ini_server = "";
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_JOIN, "", 0) && configures == 0 && commands == 0);
    /* (Join with an address: the settings first, then the session's JOIN with what was typed) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "10.0.0.2:43400", 0) && hos_coop_menu_command(HOS_COOP_CMD_JOIN, "", 0));
    CHECK(configures == 1 && last_on == 1 && !strcmp(last_name, "Malon") && !strcmp(last_server, "10.0.0.2:43400"));
    CHECK(commands == 1 && last_cmd == COOP_CMD_JOIN && !strcmp(last_text, "10.0.0.2:43400"));
    /* (Host makes this game the room; the same row again leaves it, and so does Join while joined to it) */
    session.state = COOP_JOINED, session.hosting = 1, session.self = session.maker = 1, session.port = 43400;
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_JOIN, "10.0.0.2:43400", 0) && commands == 2 && last_cmd == COOP_CMD_JOIN && configures == 2);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_HOST, NULL, 0) && commands == 3 && last_cmd == COOP_CMD_LEAVE);   /* (Host again) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_JOIN, "10.0.0.2:43400", 0) && commands == 4 && last_cmd == COOP_CMD_JOIN);
    /* (a field's Enter (number 1) reaches the session that runs: a new name is told to the room, a new host address
       leaves it. A kept value (number 0) is not applied to a session that runs) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_NAME, "Talon", 1) && configures == 4 && !strcmp(last_name, "Talon"));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "192.168.1.7:43400", 1) && configures == 5 && !strcmp(last_server, "192.168.1.7:43400"));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_ADDRESS, "192.168.1.7:43400", 0) && configures == 5);
    /* (the room as the tab gets it: the members, their keys from the offer, this player's from the pools' list) */
    session.n = 3, session.self = 2, session.maker = 1, session.hosting = 0, session.state = COOP_JOINED;
    session.member[0] = (CoopPeer){1, 0, 1, "Malon", 101, 0};
    session.member[1] = (CoopPeer){2, 1, 0, "Talon", 202, 0};
    session.member[2] = (CoopPeer){3, 0, 0, "Darunia", 303, 0};
    sync_has = 1, sync_maker = 1;
    memset(&sync_offer, 0, sizeof sync_offer);
    sync_offer.token = 0x5eed0c0b, sync_offer.n = 2, sync_offer.unidentified = 0, sync_offer.accepted = 0;
    sync_offer.member[0] = (CoopOfferMember){1, 1, 1, 0, "Malon", "a9e0-31c7-6d58-f2b4", ""};
    sync_offer.member[1] = (CoopOfferMember){3, 1, 0, 0, "Darunia", "d03a-5f17-c2e9-8b44", "Darunia"};
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_JOINED && v.n == 3 && !v.unidentified);
    CHECK(v.remedies && v.port == 43400);
    /* (in the roster's order: 1 Malon the room's maker, 2 this player, 3 Darunia. The others' keys come from the offer,
       this player's from the pools' list; a key accepted under another name is a new offer) */
    CHECK(!v.member[0].self && !strcmp(v.member[0].name, "Malon") && v.member[0].maker &&
          !strcmp(v.member[0].key, "a9e0-31c7-6d58-f2b4") && !v.member[0].accepted);
    CHECK(v.member[1].self && !strcmp(v.member[1].name, "Talon") && !strcmp(v.member[1].key, "4be1-07d2-9a3f-c615") &&
          v.member[1].away);
    CHECK(!strcmp(v.member[2].name, "Darunia") && !strcmp(v.member[2].key, "d03a-5f17-c2e9-8b44") && !v.member[2].accepted);
    /* (the members' keys are in force as soon as the room has them: it accepts itself, host/coop_oot.c. A key the room
       holds under another name than this one is not in force yet, and the host's remedies) */
    sync_offer.accepted = 1;
    hos_coop_menu_view(&v);
    CHECK(v.member[0].accepted && !v.member[2].accepted);   /* (Malon's own key is; Darunia's, held as "Darunia", is not) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_REMOVE, "d03a-5f17-c2e9-8b44", 0) && !strcmp(removed_key, "d03a-5f17-c2e9-8b44"));
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_REMOVE, "", 0));
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_RESET_POOLS, NULL, 0) && resets == 1);
    sync_maker = 0;
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_RESET_POOLS, NULL, 0) && resets == 1 && sync_refused == 1);   /* (refused: only the room's maker resets the pools) */
    hos_coop_menu_view(&v);
    CHECK(!v.remedies);
    puts("PASS: host/coop_menu.c: the room as the tab gets it, its commands to the session, the fields and the port");
}
static void demo(void) {
    set_env("HOS_COOP_SERVER", NULL), set_env("HOS_COOP_NAME", NULL), set_env("HOS_COOP_PORT", NULL);
    MkwVkCoopView v;
    clock_ms = 0;
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_HOST, NULL, 0));
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_CONNECTING && v.hosting && configures == 0 && commands == 0);   /* (the session never asked) */
    clock_ms = 1300;
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_JOINED && v.remedies && v.n == 1 && v.member[0].self && v.member[0].maker);
    clock_ms = 3100;
    hos_coop_menu_view(&v);
    CHECK(v.n == 2 && v.unidentified && !strcmp(v.member[1].name, "Saria") && !v.member[1].key[0]);
    clock_ms = 6100;
    hos_coop_menu_view(&v);
    CHECK(v.n == 3 && !v.unidentified && v.member[1].key[0] && v.member[1].accepted && v.member[2].accepted);   /* (in force the moment the keys are in) */
    clock_ms = 9500;
    hos_coop_menu_view(&v);
    CHECK(v.member[2].away);   /* (Darunia has not been heard from) */
    clock_ms = 13500;
    hos_coop_menu_view(&v);
    CHECK(!v.member[2].away);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_REMOVE, v.member[1].key, 0));
    hos_coop_menu_view(&v);
    CHECK(v.n == 2 && !strcmp(v.member[1].name, "Darunia"));   /* (Saria is out of the made-up room) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_RESET_POOLS, NULL, 0));
    CHECK(!hos_coop_menu_command(HOS_COOP_CMD_REMOVE, "7c1d-e842-3b90-a5f6", 0));   /* (she is out of the made-up room already) */
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_HOST, NULL, 0));   /* (the row again: stopped hosting) */
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_LEFT && !v.hosting);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_JOIN, "192.168.1.20:43400", 0));
    clock_ms = 20000;
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_JOINED && !v.hosting && v.n == 3 && v.member[2].self && v.member[0].maker);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_JOIN, "192.168.1.20:43400", 0));   /* (the row again: left the room) */
    hos_coop_menu_view(&v);
    CHECK(v.state == HOS_COOP_STATE_LEFT);
    CHECK(hos_coop_menu_command(HOS_COOP_CMD_NAME, "Link", 1) && hos_coop_menu_command(HOS_COOP_CMD_PORT, "43410", 0));
    hos_coop_menu_view(&v);
    CHECK(!strcmp(v.name, "Link") && v.port == 43410);
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
        CHECK(!strcmp(row[c].key, key));                                   /* (what host/coop_oot.c read_rules asks the file for) */
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
        "@@ROWS@@": between(menu, "    // (hookshot) Co-op (host/coop_menu.c)",
                            "    // (hookshot) the Gameplay tab's Bug report"),
        "@@COOP@@": between(menu, "MkwVkCoopView coop_now{};", "// ---- the rows that take a restart"),
        "@@ROWMAP@@": block(menu, "int tab_row_to_global("),
        "@@APPLY@@": between(menu, "/// (hookshot) Enter on the Co-op tab's Player name", "/// Custom output size"),
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
    if version < 24:
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
            raise SystemExit("FAIL: save: a Room rules row is not written under the key host/coop_oot.c reads")
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
    need(menu, r'\{"Player name", &coop_name_field, coop_name_apply\},\s*\{"Host address", &coop_addr_field, coop_addr_apply\}\};',
         "vk_menu.cpp: the Co-op tab's name and host address are not text fields")
    # (the Port row: its key in the options file, its value told to host/coop_menu.c when the row changes and at the start)
    need(menu, r'if \(r\.value == &coop_port\) return "coop_port";', "ini_key: the Port row's key is not coop_port (host/coop.c reads it at the start)")
    need(block(menu, "void set_row("), r"if \(r\.value == &coop_port && host && host->coop_command\)\s*"
         r"host->coop_command\(HOS_COOP_CMD_PORT, tx_port\(coop_port\), 0\);", "set_row: the Port row does not tell the host its port")
    need(block(menu, "void menu_init() {"), r'if \(!strcmp\(line, "coop_port"\)\)', "menu_init: hookshot_options.ini's coop_port= is not read")
    need(block(menu, "void menu_init() {"), r"host->coop_command\(HOS_COOP_CMD_NAME, coop_name_field\.text, 0\);\s*"
         r"host->coop_command\(HOS_COOP_CMD_ADDRESS, coop_addr_field\.text, 0\);\s*"
         r"host->coop_command\(HOS_COOP_CMD_PORT, tx_port\(coop_port\), 0\);",
         "menu_init: the saved name, host address and port are not given to the host")
    need(tick := block(menu, "void menu_tick() {"), r"if \(open_now\.load\(\)\) coop_refresh\(&changed\);", "menu_tick: the room is not read while the menu is open")
    need(tick, r'!strcmp\(key, "ROW"\) && next\[n\] == \':\'.*?if \(want == rows\[i\]\.name\) tab_selected\[cur_tab\] = k',
         "menu_tick: RX_VK_MENU_SCRIPT has no ROW:<name> (a scripted session must choose Accept by name, not by counting rows)")
    if tick.index("else if (press[7] && coop_kind(r) >= 0) coop_press(r)") > tick.index("else if (press[7] && !r.value && can_bind())"):
        raise SystemExit("FAIL: menu_tick: the Co-op tab's rows would open the Controls page")
    need(tick, r"pending != &r \|\| \(!mine && coop_kind\(\*pending\) < 0\)", "menu_tick: what is armed is not kept without the focus")
    need(tick, r"else if \(press\[7\] && coop_kind\(r\) == kCoopRules\) cur_tab = TAB_RULES", "menu_tick: Room rules does not open its page on confirm")
    if tick.index("coop_kind(r) == kCoopRules) cur_tab = TAB_RULES") > tick.index("else if (press[7] && coop_kind(r) >= 0) coop_press(r)"):
        raise SystemExit("FAIL: menu_tick: Room rules would be pressed as a room action")
    need(tick, r"if \(press\[5\] && cur_tab == TAB_RULES\) cur_tab = TAB_COOP, press\[5\] = false", "menu_tick: back does not leave the Room rules page")
    need(tick, r"if \(\(tab_left \|\| tab_right\) && cur_tab == TAB_RULES\) cur_tab = TAB_COOP;", "menu_tick: the tabs' keys from the Room rules page index past the strip")
    need(block(menu, "void menu_init() {"), r"if \(opts\.coop_keys == 1\) opts\.coop_keys = 2;\s*if \(opts\.coop_rupees == 1\) opts\.coop_rupees = 0;\s*"
         r"if \(opts\.coop_ammo == 1\) opts\.coop_ammo = 0;", "menu_init: a mode a pool row does not have is shown and saved as it is")
    need(block(menu, "void save() {"), r'key == "coop_name" \|\|\s*\(key == "coop_server" && coop_addr_whole\)',
         "save: the Co-op tab's name and host address are the file's, not what a longer hand-written line had")
    need(block(menu, "void save() {"), r'fprintf\(f, "coop_name=%s\\n", coop_name_field\.text\).*'
         r'fprintf\(f, "coop_server=%s\\n", coop_addr_field\.text\)',
         "save: hookshot_options.ini's coop_name= and coop_server= (host/coop.c reads them at the start) are not the tab's")
    need(block(menu, "void menu_init() {"), r'!strcmp\(line, "coop_name"\) \|\| !strcmp\(line, "coop_server"\).*'
         r'coop_name_field', "menu_init: the saved name and host address are not read")
    need(block(menu, "void draw_options(ui::Ctx& c) {"), r"if \(cur_tab == TAB_COOP\) \{.*?draw_coop_rows\(", "draw_options: no Co-op layout")
    print("PASS: the Co-op tab's wiring: the interface's coop_view and coop_command, the host's table, the strip, the Port "
          "row's key and command, the Room rules page, the options file")


if __name__ == "__main__":
    main()
