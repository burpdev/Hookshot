"""The Insert menu's text fields: a row whose value is typed (the first one is Custom output size, the Output resolution's
size of the player's own, which before could only be set in hookshot_options.ini).

The menu's own functions are cut out of gpu/vk/vk_menu.cpp by their text and driven with key and text events as the host
gives them (MkwVkHost.take_text): typing, the caret's keys, Backspace and Delete, the length limit, what a field takes (printable
ASCII alone, as the menu's glyphs are baked; a character outside it is left out), Enter and Esc; then Custom output size's
Enter, which sets the Output resolution row to the size typed, or says why not. The host's side, cut out of host/rx_backend.c,
is compiled as C: the queue between the window's thread and the menu, nothing kept while no field has the keyboard, UTF-8 from
SDL's text input and the clipboard, its first line alone, a broken sequence left out. Then the wiring is checked in the
source: the interface's new member last and its version bumped, WM_CHAR and SDL's text input put into the queue, the menu
telling the host whether a field has the keyboard, RX_VK_MENU_SCRIPT's TYPE. No GPU, game or window.

    python -I -B tests/test_menu_text_field.py [--cxx g++]
"""
import argparse
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "host/hos_wide_size.h"
#include "gpu/vk/display_modes.h"
#include "gpu/vk/menu_restart.h"
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); std::exit(1); } } while (0)
enum { VK_BACK = 8, VK_TAB = 9, VK_RETURN = 0x0d, VK_ESCAPE = 0x1b, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_RIGHT = 0x27,
       VK_DELETE = 0x2e };
@@TEXT_ENUMS@@
struct { int output_res, preset, ultrawide, handheld; } opts;
struct Row { int* value; int hi; bool restart; };
Row rows[] = {{&opts.ultrawide, 0, true}, {&opts.output_res, 0, true}};
constexpr int kRows = 2;
int restart_slot[kRows] = {0, 1};
namespace mr = menu_restart;
struct RestartState : mr::State {
    int noted = 0;
    void note(const int* now) { noted++; mr::State::note(now); }
} restart_state;
bool menu_refresh = false;
int ultrawide_read = 1;
uint32_t screen_w = 1920, screen_h = 1080;
bool screen_size(uint32_t* w, uint32_t* h) {
    return screen_w && screen_h ? (*w = screen_w, *h = screen_h, true) : false;
}
bool host_screen_size(uint32_t* w, uint32_t* h) { return screen_size(w, h); }
std::vector<display_modes::Size> screen_modes() { return {{1920, 1080}}; }
void logf(const char*, ...) {}
static int saves = 0;
void save() { saves++; }
static const int* released = nullptr;   // (main's env_release: the row the player set is theirs, over HOS_OUTPUT_RES)
void env_release(const int* value) { released = value; }
@@CUT@@
static const uint32_t K = HOS_TEXT_KEY;
static void type(TextField& f, const char* s) {
    for (; *s; s++) CHECK(text_edit(f, (unsigned char)*s) == kTextGoOn);
}
static bool is(const TextField& f, const char* text, int caret) {
    return f.len == (int)strlen(text) && !strcmp(f.text, text) && f.caret == caret;
}
static const char* apply(const char* typed) {
    text_set(output_field, typed);
    return output_field_apply();
}
static void startup(const char* output, int follow = 0) {
    custom_output = {0, 0};
    output_extras.clear();
    (void)output_shapes();
    opts.output_res = parse_output(output), opts.ultrawide = follow;
    outputs_sync();
    restart_state = {};
    for (const Row& row : rows) restart_state.add(restart_value(row));
    output_field_show();
    saves = 0, menu_refresh = false, released = nullptr;
}
int main() {
    TextField f;
    text_set(f, "");
    CHECK(is(f, "", 0));
    type(f, "abc");
    CHECK(is(f, "abc", 3));
    CHECK(text_edit(f, K | VK_LEFT) == kTextGoOn && text_edit(f, K | VK_LEFT) == kTextGoOn && is(f, "abc", 1));
    type(f, "X");   // (typed at the caret)
    CHECK(is(f, "aXbc", 2));
    text_edit(f, K | VK_HOME), text_edit(f, K | VK_DELETE);
    CHECK(is(f, "Xbc", 0));
    text_edit(f, K | VK_BACK);   // (nothing before the caret)
    CHECK(is(f, "Xbc", 0));
    text_edit(f, K | VK_END), text_edit(f, K | VK_BACK);
    CHECK(is(f, "Xb", 2));
    text_edit(f, K | VK_DELETE), text_edit(f, K | VK_RIGHT);   // (nothing after it)
    CHECK(is(f, "Xb", 2));
    text_edit(f, K | VK_LEFT), text_edit(f, K | VK_LEFT), text_edit(f, K | VK_LEFT);   // (stops at the start)
    CHECK(is(f, "Xb", 0));
    text_edit(f, K | VK_TAB);   // (not an editing key: nothing)
    CHECK(is(f, "Xb", 0));
    // (printable ASCII alone: the menu's glyphs are baked for it. Another character typed is left out, whatever the keyboard)
    for (uint32_t c : {0xE9u, 0xFCu, 0x4E2Du, 0x1F600u, 0x09u, 0x0Au, 0x7Fu, 0x00u, 0x80u, 0xFFu}) CHECK(text_edit(f, c) == kTextGoOn);
    CHECK(is(f, "Xb", 0));
    type(f, " ~");   // (the ends of the range)
    CHECK(is(f, " ~Xb", 2));
    // (Enter and Esc end the edit and leave the text as it is: the menu sets it or puts it back)
    CHECK(text_edit(f, K | VK_RETURN) == kTextDone && text_edit(f, K | VK_ESCAPE) == kTextCancel && is(f, " ~Xb", 2));
    // (the length limit)
    TextField small;
    small.max = 5;
    text_set(small, "");
    type(small, "abcdefgh");
    CHECK(is(small, "abcde", 5));
    text_set(small, "0123456789");   // (set whole: cut to what it holds)
    CHECK(is(small, "01234", 5));
    text_set(small, nullptr);
    CHECK(is(small, "", 0));
    small.max = 1000;   // (never past its buffer)
    for (int i = 0; i < 100; i++) text_edit(small, 'z');
    CHECK(small.len == (int)sizeof small.text - 1 && small.text[small.len] == 0);
    small.len = 500, small.caret = -3;   // (out of range: taken back into it)
    text_edit(small, K | VK_END);
    CHECK(small.len == (int)sizeof small.text - 1 && small.caret == small.len);
    // (a field that takes less: Custom output size, digits and the x)
    text_set(output_field, "");
    type(output_field, "3440 x1440a");
    CHECK(!strcmp(output_field.text, "3440x1440") && output_field.max == 11);

    // ---- Custom output size's Enter: the Output resolution row on the size typed ----
    startup("0");
    CHECK(custom_index() != 6);   // (the display's common sizes, not the former fixed six choices)
    opts.preset = 3;
    CHECK(apply("3440x1440") == nullptr);
    CHECK(opts.output_res == custom_index() && custom_output.w == 3440 && custom_output.h == 1440 && saves == 1);
    CHECK(aspect_shape(opts.ultrawide) == "21:9" && rows[0].hi == aspect_last() && rows[1].hi == last_choice());
    CHECK(menu_refresh && restart_changed(rows[0]) && restart_changed(rows[1]));
    // (the size typed is the player's from now on, also over HOS_OUTPUT_RES; the Quality preset stays: the Output
    // resolution is a row no preset sets, preset_exempt, as tests/test_preset_exempt.py has it)
    CHECK(!strcmp(output_field.text, "3440x1440") && opts.preset == 3 && restart_state.noted == 1 && released == &opts.output_res);
    opts.preset = 4;
    CHECK(apply("3440x1440") == nullptr && opts.preset == 4);
    CHECK(apply("1366X768") == nullptr && custom_output.w == 1360 && custom_output.h == 768);   // (sides to a multiple of 8)
    CHECK(!strcmp(output_field.text, "1360x768") && opts.preset == 4);
    CHECK(apply("1920x1080") == nullptr);   // (a size the row offers: chosen there, none of the player's own)
    const int fixed = output_choice_of(1920, 1080);
    CHECK(opts.output_res == fixed && custom_output.w == 0 && !strcmp(output_field.text, ""));
    const int saved = saves;
    for (const char* bad : {"320x200", "9999x9999", "7688x1440", "640x4320"})   // (not a size the game draws)
        CHECK(apply(bad) && strstr(apply(bad), "Not a size") && opts.output_res == fixed);
    for (const char* bad : {"3440x", "x1440", "3440", "123456x1", "34x40x10", "3440xx1440"})   // (not width x height)
        CHECK(apply(bad) && strstr(apply(bad), "Type the width") && opts.output_res == fixed);
    CHECK(saves == saved);   // (refused: nothing saved)
    CHECK(apply("5120x2160") == nullptr && opts.output_res == custom_index());
    CHECK(apply("") == nullptr && opts.output_res == 0 && custom_output.w == 0);   // (emptied: none, the game's own size)
    opts.output_res = fixed;
    CHECK(apply("") == nullptr && opts.output_res == fixed);   // (emptied with an offered size: that one stays)
    // (the file's size of the player's own shows in the field at the start)
    CHECK(parse_output("2560x1080") == custom_index() && custom_index() == last_choice());
    output_field_show();
    CHECK(!strcmp(output_field.text, "2560x1080") && output_field.caret == 9);
    // (sizes discovered after start are ordinary choices too, including the rounded dimensions)
    startup("0");
    output_extras.push_back({1600, 896});
    const int extra = last_choice();
    CHECK(apply("1600x900") == nullptr && opts.output_res == extra && !custom_output.w && !output_field.len);
    CHECK(rows[1].hi == extra && aspect_shape(opts.ultrawide) == "16:9");
    // (adding/removing a narrower custom shape remaps the startup index without resetting what runs)
    startup("0");
    const int original_output = restart_state.start[1];
    CHECK(apply("1440x1080") == nullptr && aspect_shape(opts.ultrawide) == "4:3");
    CHECK(aspect_shape(aspect_started) == "16:9" && restart_state.start[0] == aspect_started);
    CHECK(restart_state.start[1] == original_output && restart_changed(rows[0]) && restart_changed(rows[1]));
    CHECK(apply("2048x1152") == nullptr && !restart_changed(rows[0]) && !restart_changed(rows[1]));
    // (replacing the run's original custom size preserves it once, so returning to its shape still restores it)
    startup("2400x1600", 1);
    CHECK(apply("3440x1440") == nullptr && output_extras.size() == 1 && output_started > custom_index());
    CHECK(apply("2560x1080") == nullptr && output_extras.size() == 1);
    const int back = output_for_aspect(aspect_started);
    CHECK(output_at(back).w == 2400 && output_at(back).h == 1600);
    opts.output_res = back, opts.ultrawide = aspect_started;
    CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]));
    CHECK(apply("2400x1600") == nullptr && opts.output_res == output_started && !custom_output.w);
    CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]));
    // (an unknown Monitor's shape is a distinct value, even when a custom shape changes its index)
    screen_w = screen_h = 0;
    startup("0", 1);
    CHECK(apply("2400x1600") == nullptr && aspect_started == aspect_monitor_value());
    opts.output_res = output_started, opts.ultrawide = aspect_started;
    int now[mr::kMax] = {};
    restart_values(now);
    restart_state.choose_later(now);
    CHECK(apply("") == nullptr && opts.ultrawide == aspect_monitor_value() && aspect_started == aspect_monitor_value());
    CHECK(!restart_changed(rows[0]) && !restart_changed(rows[1]) && restart_state.later);
    std::puts("PASS: typing, the caret's keys, the limits, ASCII alone, Enter and Esc; Custom output size sets the Output resolution");
}
'''

HOST_FIXTURE = r'''#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef long LONG;
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static int locked;
static void AcquireSRWLockExclusive(SRWLOCK *l) { (void)l; CHECK(!locked); locked = 1; }
static void ReleaseSRWLockExclusive(SRWLOCK *l) { (void)l; CHECK(locked); locked = 0; }
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
static LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) { LONG o = *p; if (o == cmp) *p = v; return o; }
@@TEXT_ENUMS@@
static int pastes;
void hos_display_paste(void) { pastes++; }
@@CUT@@
static int take(int focus, uint32_t *out) { return host_take_text(focus, out, 512); }
int main(void) {
    uint32_t out[512];
    hos_text_put('a');   /* (no field has the keyboard: dropped) */
    CHECK(!hos_text_focus() && take(HOS_TEXT_OFF, out) == 0);
    CHECK(take(HOS_TEXT_ON, out) == 0 && hos_text_focus());
    hos_text_put('h'), hos_text_put('i'), hos_text_put(HOS_TEXT_KEY | 8);
    CHECK(take(HOS_TEXT_ON, out) == 3 && out[0] == 'h' && out[1] == 'i' && out[2] == (HOS_TEXT_KEY | 8));
    CHECK(take(HOS_TEXT_ON, out) == 0);
    /* (UTF-8, as SDL's text input gives it: each character's code point, whatever its length) */
    hos_text_put_utf8("a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80z", 0);
    CHECK(take(HOS_TEXT_ON, out) == 5 && out[0] == 'a' && out[1] == 0xE9 && out[2] == 0x4E2D && out[3] == 0x1F600 && out[4] == 'z');
    /* (a broken sequence left out, the byte that broke it read as a character of its own) */
    hos_text_put_utf8("\xC3(\xE4\xB8x\x80\xFFq", 0);
    CHECK(take(HOS_TEXT_ON, out) == 3 && out[0] == '(' && out[1] == 'x' && out[2] == 'q');
    /* (a paste: its first line alone; a typed line break is a character, which the menu leaves out) */
    hos_text_put_utf8("abc\r\ndef", 1);
    CHECK(take(HOS_TEXT_ON, out) == 3 && out[2] == 'c');
    hos_text_put_utf8("one\ntwo", 0);
    CHECK(take(HOS_TEXT_ON, out) == 7 && out[3] == '\n');
    hos_text_put_utf8(NULL, 1);
    CHECK(take(HOS_TEXT_ON, out) == 0);
    /* (taken in order a few at a time) */
    for (uint32_t c = 0; c < 10; c++) hos_text_put('0' + c);
    CHECK(host_take_text(HOS_TEXT_ON, out, 4) == 4 && out[3] == '3' && host_take_text(HOS_TEXT_ON, out, 64) == 6 && out[0] == '4');
    /* (the queue holds TEXT_QUEUE, the rest dropped) */
    for (int i = 0; i < TEXT_QUEUE + 50; i++) hos_text_put('x');
    CHECK(take(HOS_TEXT_ON, out) == TEXT_QUEUE);
    /* (Paste asks the window for the clipboard; the field losing the keyboard drops what was typed) */
    CHECK(take(HOS_TEXT_PASTE, out) == 0 && pastes == 1 && hos_text_focus());
    hos_text_put('q');
    CHECK(take(HOS_TEXT_OFF, out) == 0 && !hos_text_focus());
    hos_text_put('r');
    CHECK(take(HOS_TEXT_ON, out) == 0 && !locked);
    puts("PASS: the text queue: only while a field has the keyboard, in order, UTF-8 to characters, a paste's first line");
    return 0;
}
'''


def compile_run(cxx, lang, std, source, name, tmp):
    """source compiled as lang (c++ or c, the C++ driver given -x) and run"""
    src, exe = Path(tmp) / (name + ".src"), Path(tmp) / name
    src.write_text(source, encoding="utf-8")
    subprocess.run([cxx, "-x", lang, "-std=" + std, "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT), str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    api = (ROOT / "gpu/vk/vk_api.h").read_text(encoding="utf-8")
    backend = (ROOT / "host/rx_backend.c").read_text(encoding="utf-8")
    enums = "\n".join(re.findall(r"^enum \{ HOS_TEXT_[^\n]*\};$", api, re.M))
    if enums.count("enum") != 2:
        raise SystemExit("FAIL: vk_api.h: no HOS_TEXT_KEY and HOS_TEXT_OFF/ON/PASTE")
    cut = "\n".join([block(menu, "struct TextField {"), between(menu, "enum { kTextGoOn", "\n") + "\n",
                     block(menu, "int text_edit("), block(menu, "void text_set("),
                     between(menu, "struct Out {", "Out shown_output()"),
                     block(menu, "void outputs_sync("), block(menu, "int restart_value("),
                     block(menu, "int restart_slot_of("), block(menu, "bool restart_changed("),
                     block(menu, "void restart_values("),
                     between(menu, "bool output_size_char(", "struct FieldRow {")])
    host_cut = between(backend, "enum { TEXT_QUEUE", "static const MkwVkHost vk_host")
    with tempfile.TemporaryDirectory(prefix="menu-text-") as tmp:
        compile_run(a.cxx, "c++", "c++20", MENU_FIXTURE.replace("@@TEXT_ENUMS@@", enums).replace("@@CUT@@", cut), "menu", tmp)
        compile_run(a.cxx, "c", "c11", HOST_FIXTURE.replace("@@TEXT_ENUMS@@", enums).replace("@@CUT@@", host_cut), "host", tmp)

    # ---- the wiring ----
    version = int(re.search(r"#define MKWVK_API_VERSION (\d+)", api).group(1))
    if version < 21:
        raise SystemExit("FAIL: vk_api.h: MkwVkHost grew without an MKWVK_API_VERSION bump")
    # (host/rx_backend.c fills MkwVkHost by position: take_text's place in the struct is host_take_text's in vk_host)
    def code(text):   # (comments out: they hold ; and ,)
        return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", text, flags=re.S))
    decls = [d.strip() for d in code(between(api, "typedef struct MkwVkHost {", "} MkwVkHost;")).split("{", 1)[1].split(";")]
    fields = [re.search(r"\(\*(\w+)\)", d).group(1) if "(*" in d else d.split()[-1] for d in decls if d]
    table = [v.strip() for v in code(between(backend, "static const MkwVkHost vk_host = {", "};")).split("{", 1)[1].split(",")]
    table = [v for v in table if v]
    if "take_text" not in fields or len(table) != len(fields) or table[fields.index("take_text")] != "host_take_text":
        raise SystemExit("FAIL: vk_api.h / rx_backend.c: vk_host does not give host_take_text in take_text's place (%d members, "
                         "%d values)" % (len(fields), len(table)))
    if fields[-2:] != ["screen_size", "take_text"] or table[fields.index("screen_size")] != "host_screen_size":
        raise SystemExit("FAIL: take_text must follow screen_size, with both callbacks in their interface slots")
    win = (ROOT / "host/display.c").read_text(encoding="utf-8")
    need(win, r"case WM_CHAR: \{.*?hos_text_put\(c\);.*?return 0;", "display.c: WM_CHAR does not reach the menu's text field")
    need(win, r"case WM_KEYDOWN: \{.*?if \(hos_text_focus\(\)\) \{.*?paste_clipboard\(w\);.*?hos_text_put\(HOS_TEXT_KEY \| \(uint32_t\)wp\);",
         "display.c: the editing keys and Ctrl+V do not reach the menu's text field")
    need(win, r"case WM_APP \+ 12:[^\n]*\n\s*paste_clipboard\(w\);", "display.c: the menu's Paste is not WM_APP + 12")
    if len(re.findall(r"case WM_APP \+ 12:", win)) != 1:
        raise SystemExit("FAIL: display.c: WM_APP + 12 is handled twice")
    need(win, r"GetClipboardData\(CF_UNICODETEXT\)", "display.c: the clipboard is not read as Unicode")
    sdl = (ROOT / "host/posix/display_sdl.c").read_text(encoding="utf-8")
    need(sdl, r"case HOS_SDL_EV_TEXT:\s*hos_text_put_utf8\(e\.text, 0\);", "display_sdl.c: SDL's text input does not reach the field")
    need(sdl, r"if \(hos_text_focus\(\)\) text_key\(&e\);", "display_sdl.c: the editing keys do not reach the field")
    need(sdl, r"if \(hos_text_focus\(\) != text_on\) hos_sdl_text_input\(window, text_on = hos_text_focus\(\)\);",
         "display_sdl.c: SDL's text input is not on only while a field has the keyboard")
    sdl_lib = (ROOT / "host/hos_sdl.c").read_text(encoding="utf-8")
    if len(re.findall(r"case 0x303:", sdl_lib)) != 2 or "HOS_SDL_EV_TEXT" not in sdl_lib:
        raise SystemExit("FAIL: hos_sdl.c: SDL3's and SDL2's text input events are not both read")
    tick = block(menu, "void menu_tick() {")
    need(tick, r"host->take_text\(!editing \? HOS_TEXT_OFF : pasting \? HOS_TEXT_PASTE : HOS_TEXT_ON, typed, 64\)",
         "menu_tick: the host is not told whether a field has the keyboard")
    # (while a field has the keyboard, its branch comes before every page's: no key does anything else)
    first = tick.index("if (open_now.load() && editing) {")
    if first > tick.index("} else if (open_now.load() && page == 5) {"):
        raise SystemExit("FAIL: menu_tick: a page's keys are read before the field's")
    need(tick, r"edit_end\(false\);\s*// \(hookshot: a text field being edited", "menu_tick: closing the menu does not end the edit")
    need(tick, r'!strcmp\(key, "TYPE"\)', "menu_tick: RX_VK_MENU_SCRIPT has no TYPE")
    need(tick, r"pad\(pad_paste\)", "menu_tick: a controller cannot paste")
    need(menu, r'\{TAB_GRAPHICS, "Custom output size", nullptr,', "vk_menu.cpp: no Custom output size row")
    need(menu, r'const FieldRow field_rows\[\] = \{\{"Custom output size", &output_field, output_field_apply\}\};',
         "vk_menu.cpp: Custom output size is not a text field")
    hid = (ROOT / "host/nn_hid.c").read_text(encoding="utf-8")
    need(hid, r"static int key\(int vk\) \{ return !rx_option\(1, 0\) &&",
         "nn_hid.c: the game's keys are not held back while the menu (and so a field) has the keyboard")
    print("PASS: the text field's wiring: the interface's take_text, WM_CHAR, SDL's text input, the menu's focus and TYPE")


if __name__ == "__main__":
    main()
