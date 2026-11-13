"""Co-op (host/oot_coop.c, the F1 menu's Co-op tab): what holds it together, without a game.

The lifted game reaches co-op through three calls nx2/oot_patches.py puts in: oot_coop_pouch_add first in the pouch's add by
name (f_8112d570), and in the Life component's update (f_804bd8c0) oot_coop_life_seen first and oot_coop_life_write before each
of its two writes of the life, after the heart protection's guard when that is there. Each is made once, declared, left alone in
a file patched before, and made only in the supported build's lift (protect_hearts): those addresses are its functions.

The host has every entry point on every platform (the lifted C calls them wherever it is built): the pouch's add, the Life
component's two, and the three equipment user's calls the game makes on Link (what he holds goes to the others). The runtime
calls the frame's and the player's, and the menu and the host agree on the option ids: one header (gpu/vk/oot_coop_opts.h), whose numbers are
no other row's, each answered or taken by the menu.

The game build (nx2/make_game.py) runs the data builders (nx2/coop_bodies.py with nx2/byml_rw.py) into the game folder it
finishes, so no co-op game ships without the other players' bodies; what of them needs no game is checked here, and that the
build makes them: a BYML document written and read back is the same, node types kept; an actor pack written and read back
holds the same files; a size table takes a new entry in its place by hash and only ever raises one.

python -I -B tests/test_oot_coop.py
"""
import importlib.util
import os
import re
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

PICKUP = "    oot_coop_pouch_add(c);"
SEEN = "    oot_coop_life_seen(c, c->x[0]);"
WRITE = "oot_coop_life_write(c, c->x[19], (uint32_t)c->x[11], (uint32_t)c->x[12]); "
BODY = "    c->x[19]=c->x[0];\n    c->x[8]=recomp_load64(c,c->x[19] + 56);\nreturn;\n}\n"
LIFE_BODY = ("    c->x[19]=c->x[0];\n    " + oot_patches.LIFE_CAS + "\n    c->x[8]=c->x[11];\n    " + oot_patches.LIFE_CAS +
             "\nreturn;\n}\n")


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def read(*parts):
    return ROOT.joinpath(*parts).read_text(encoding="utf-8")


def lifted(life=LIFE_BODY):
    return '#include "a64.h"\n' + oot_patches.POUCH_ADD_HEAD + BODY + oot_patches.LIFE_HEAD + life


def check_pickup():
    out = oot_patches.patch_coop_pickup(lifted())
    check(out is not None and out.count(PICKUP) == 1, "the pickup hook is not in its function once")
    check(oot_patches.POUCH_ADD_HEAD + PICKUP in out, "the pickup hook is not first in its function")
    check("extern void oot_coop_pouch_add(Ctx *c);" in out, "the lifted file does not declare the pickup hook")
    check(BODY in out, "the pouch function's body was changed")
    check(oot_patches.patch_coop_pickup(out) == out, "a file patched before got the pickup hook again")
    check(oot_patches.patch_coop_pickup('#include "a64.h"\n' + oot_patches.LIFE_HEAD + LIFE_BODY) is None,
          "a file without the pouch function was patched")


def check_life():
    out = oot_patches.patch_coop_life(lifted())
    check(out is not None and out.count(SEEN) == 1 and oot_patches.LIFE_HEAD + SEEN in out,
          "oot_coop_life_seen is not first in the Life update, once")
    check(out.count(WRITE + oot_patches.LIFE_CAS) == 2 and out.count(WRITE) == 2,
          "oot_coop_life_write is not before each of the two writes of the life")
    check("extern void oot_coop_life_seen(Ctx *c, uint64_t life);" in out and
          "extern void oot_coop_life_write(Ctx *c, uint64_t life, uint32_t was, uint32_t now);" in out,
          "the lifted file does not declare the life hooks")
    check(oot_patches.patch_coop_life(out) == out, "a file patched before got the life hooks again")
    one = LIFE_BODY.replace(oot_patches.LIFE_CAS, "", 1)
    check(oot_patches.patch_coop_life(lifted(one)) is None, "a Life update with one write of the life, not the two known, was patched")
    check(oot_patches.patch_coop_life('#include "a64.h"\n' + oot_patches.POUCH_ADD_HEAD + BODY) is None,
          "a file without the Life update was patched")
    # (after the heart protection: its guard decides the life first, and the life it leaves is the life told)
    guarded = oot_patches.patch_coop_life(oot_patches.patch_life(lifted()))
    check(guarded is not None and guarded.count("c->x[12] = c->x[11];\n" + WRITE + oot_patches.LIFE_CAS) == 2,
          "the life hook does not come after the heart protection's guard")


def check_equip():
    heads = (oot_patches.EQUIP_MAKE_HEAD,) + oot_patches.EQUIP_STATE_HEADS
    src = '#include "a64.h"\n' + "".join(h + "    return;\n}\n\n" for h in heads)
    out = oot_patches.patch_coop_equip(src)
    check(out is not None, "the equipment user's calls were not patched")
    for k, h in enumerate(heads):
        call = "    oot_coop_equip_seen(c, %d);" % k
        check(out.count(call) == 1 and h + call in out,
              "oot_coop_equip_seen(c, %d) is not first in its function, once" % k)
    check(out.count("extern void oot_coop_equip_seen(Ctx *c, int kind);") == 1, "the equipment hook is not declared exactly once")
    check(oot_patches.patch_coop_equip(out) == out, "a file patched before got the equipment hooks again")
    check(oot_patches.patch_coop_equip('#include "a64.h"\n' + oot_patches.POUCH_ADD_HEAD + BODY) is None,
          "a file without the equipment user's calls was patched")
    two = '#include "a64.h"\n' + heads[0] + "    return;\n}\n\n" + heads[1] + "    return;\n}\n\n"
    got = oot_patches.patch_coop_equip(two)   # (the three lifted functions can sit in different files)
    check(got is not None and got.count("oot_coop_equip_seen(c, 0);") == 1 and got.count("oot_coop_equip_seen(c, 1);") == 1
          and "oot_coop_equip_seen(c, 2);" not in got,
          "the equipment user's calls present in a file were not each patched at their head")
    check(oot_patches.patch_coop_equip(got) == got, "a file patched before got its equipment hooks again")


def check_presence_fixes():
    """what the room told us it got wrong, checked in the source it was wrong in"""
    coop = read("host", "oot_coop.c")
    # (the animation inputs: child and adult Link wear different controllers, so a hardcoded vtable was wrong
    #  as often as right — the adult sender said no inputs at all and its puppet stood still on the others' screens)
    inp = re.search(r"static uint64_t co_inputs_at\(uint64_t actor, uint32_t \*words\) \{.*?\n    return lo;\n\}", coop, re.S)
    check(inp and "0x84c497c8" not in inp.group(0),
          "host/oot_coop.c: co_inputs_at still gates on one hardcoded controller vtable")
    # (a paused game stops updating Link: it must keep saying where it is, or the others read silence as leaving)
    frame = re.search(r"void oot_coop_frame\(void\) \{.*?\n\}\n", coop, re.S)
    check(frame and "co_place_sent > 1000000000LL" in frame.group(0) and "co_place_sent = now" in frame.group(0),
          "host/oot_coop.c: oot_coop_frame does not keep-alive the place while the game is paused")
    check("co_place_sent = now" in coop and coop.count("co_place_sent = now") >= 2,
          "host/oot_coop.c: the normal place send does not note that it said it")
    # (a body put under is not destroyed: an age flicker used to leave two puppets of one player standing together)
    bodies = re.search(r"static void co_bodies_step\(Ctx \*c.*?\n\}\n\n", coop, re.S)
    check(bodies and "now - b->child_at > 500000000LL" in bodies.group(0),
          "host/oot_coop.c: a body's age is not asked to hold for half a second before it is changed")
    check(bodies and "b->retired_of" in bodies.group(0) and "co_body_put(b->retired, under, 0)" in bodies.group(0),
          "host/oot_coop.c: a retired body is not kept sunk below the world")
    # (the age a puppet shows comes from a flag that reads only while sync is open: hold the last known one)
    check("InterlockedExchange(&co_my_child" in coop,
          "host/oot_coop.c: the said age is not held across a shut flag table")


def applied(protect_hearts):
    with tempfile.TemporaryDirectory() as gen:
        path = os.path.join(gen, "code_000.c")
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(lifted())
        stdout = sys.stdout
        sys.stdout = open(os.devnull, "w")
        try:
            oot_patches.apply(gen, protect_hearts=protect_hearts)
        finally:
            sys.stdout.close()
            sys.stdout = stdout
        with open(path, encoding="utf-8", newline="") as f:
            return f.read()


def check_apply():
    every = applied(True)
    check(every.count(PICKUP) == 1 and every.count(SEEN) == 1 and every.count(WRITE) == 2,
          "the supported build's lift did not get co-op's hooks once each")
    plain = applied(False)
    check(not any(h in plain for h in (PICKUP, SEEN, WRITE)), "another build's lift got co-op's hooks")


ENTRY = ("void oot_coop_frame(void)", "void oot_coop_player(Ctx *c, uint64_t actor)", "void oot_coop_pouch_add(Ctx *c)",
         "void oot_coop_life_seen(Ctx *c, uint64_t life)", "void oot_coop_life_write(Ctx *c, uint64_t life, uint32_t was, uint32_t now",
         "void oot_coop_equip_seen(Ctx *c, int kind)")


def check_host():
    coop = read("host", "oot_coop.c")
    # (one file for both platforms: co-op is on Linux too, so it holds no #if defined(_WIN32) split and no stand-ins.
    #  host/posix/windows.h is where CRITICAL_SECTION and Interlocked* come from there.)
    check("#if defined(_WIN32)" not in coop and "\n#else" not in coop and "#endif" not in coop,
          "host/oot_coop.c splits itself between Windows and everywhere else")
    check("#include <windows.h>" in coop, "host/oot_coop.c does not take the lock and Interlocked helpers from <windows.h>")
    for entry in ENTRY:
        check(coop.count("\n" + entry) == 1, "host/oot_coop.c: not defined once, on every platform: " + entry)
    check(re.search(r"oot_coop_player\(c, actor\);", read("host", "hos_main.c")), "hos_oot_lift_player does not tell co-op of the player")
    check(re.search(r"\n(int hos_oot_travel\(Ctx \*c, uint64_t actor, const char \*banc\)) \{", read("host", "hos_main.c")),
          "host/hos_main.c has no hos_oot_travel (co-op's go to a player)")
    check("oot_coop_frame();" in read("host", "nn_vi.c"), "hos_vi_native_presented does not call co-op's frame")
    check(read("host", "oot_menu.c").count("\nint hos_oot_menu_in_front(void)") == 2,
          "host/oot_menu.c: hos_oot_menu_in_front for Windows and its stand-in elsewhere")
    check("coop_command" in read("gpu", "vk", "vk_api.h"), "MkwVkHost has no coop_command (the Co-op tab's rows to host/coop_menu.c)")


def enum_ids(text):
    """name -> number of every HOS_OPT_* in the text's enums"""
    out = {}
    for body in re.findall(r"enum\s*\{([^}]*)\}", text):
        if "HOS_OPT_" not in body:
            continue
        value = -1
        for item in re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S).split(","):
            item = item.strip()
            if not item:
                continue
            name, _, number = (part.strip() for part in item.partition("="))
            value = int(number, 0) if number else value + 1
            out[name] = value
    return out


def check_ids():
    ours = enum_ids(read("gpu", "vk", "oot_coop_opts.h"))
    names = ["HOS_OPT_COOP_" + n for n in ("ACTION", "CHAR", "TEXT", "PVP", "LABEL", "HIDE")]
    check(sorted(ours) == sorted(names), "gpu/vk/oot_coop_opts.h: the ids are not the six expected: %s" % sorted(ours))
    check(len(set(ours.values())) == len(ours), "two co-op option ids are one number")
    theirs = enum_ids(read("gpu", "vk", "vk_api.h"))
    clash = sorted(n for n, v in theirs.items() if v in ours.values())
    check(not clash, "a co-op option id is also vk_api.h's %s" % clash)
    check(min(ours.values()) > max(theirs.values()), "the co-op ids are not after vk_api.h's rows")
    coop = read("host", "oot_coop.c")
    check('#include "../gpu/vk/oot_coop_opts.h"' in coop and not re.search(r"#define\s+HOS_OPT_COOP_", coop),
          "host/oot_coop.c does not take its option ids from gpu/vk/oot_coop_opts.h alone")
    check('#include "oot_coop_opts.h"' in read("gpu", "vk", "vk_menu.h"), "gpu/vk/vk_menu.h does not include oot_coop_opts.h")
    for name in names:
        check(name in coop, "host/oot_coop.c never uses " + name)
    replay, menu = read("gpu", "vk", "vk_replay.cpp"), read("gpu", "vk", "vk_menu.cpp")
    # (what the host asks, rx_option: vk_replay.cpp's option hands the whole range to the menu, first id to last)
    check(ours["HOS_OPT_COOP_ACTION"] == min(ours.values()) and ours["HOS_OPT_COOP_HIDE"] == max(ours.values()) and
          max(ours.values()) - min(ours.values()) == len(ours) - 1, "the co-op ids are not one run from ACTION to HIDE")
    check("if (id >= HOS_OPT_COOP_ACTION && id <= HOS_OPT_COOP_HIDE) return menu_coop_option(id);" in replay,
          "vk_replay.cpp's option does not answer co-op's rows")
    start = menu.index("int menu_coop_option(int id) {")
    asked = menu[start:menu.index("\n}\n", start)]
    for name in ("HOS_OPT_COOP_ACTION", "HOS_OPT_COOP_PVP"):
        check("id == " + name in asked, "menu_coop_option does not answer " + name)
    start = menu.index("void menu_option_set(int id, int value) {")
    setter = menu[start:menu.index("\n}\n", start)]
    for name in ("HOS_OPT_COOP_CHAR", "HOS_OPT_COOP_TEXT", "HOS_OPT_COOP_LABEL", "HOS_OPT_COOP_HIDE"):
        check("if (id == %s)" % name in setter, "menu_option_set does not take " + name)


def check_rows():
    menu = read("gpu", "vk", "vk_menu.cpp")
    rows = re.findall(r'\{TAB_COOP, "([^"]+)", (&?\w+),', menu)
    # (the Co-op tab is one room: Host and Join, where to reach the host, its port, the name, the status, then the
    #  maker's remedies and the rules page, the seven ways to follow a player and PvP. In the order they are in the
    #  table. There is no Accept row: joining shares at once, the save copied first, so the tab has nothing to ask.)
    direct = (["Host", "Join", "Host address", "Port", "Player name", "Status", "Remove a player",
               "Reset the pools", "Room rules"] + ["Go to player"] * 7 + ["PvP"])
    names = [name for name, _ in rows]
    check(names == direct, "the Co-op tab's rows are not the ones expected: %s" % names)
    # (a row with a value is saved by its name, as every row: port, pvp. No preset sets them: preset_exempt)
    for name, value in (("Port", "&coop_port"), ("PvP", "&coop_pvp")):
        check((name, value) in rows, "the Co-op tab's %s row is not %s" % (name, value))
        at = menu.index('{TAB_COOP, "%s", %s,' % (name, value))
        check(menu[at:menu.index("},\n", at)].endswith("nullptr, false, true"), "the Co-op tab's %s row would turn the preset to Custom" % name)
    check(re.search(r'tab_titles\[TAB_COUNT\] = \{[^}]*"Co-op"\}', menu), "the tab strip has no Co-op title")


def check_builders():
    import byml_rw
    doc = {"Components": {"ASRef": "?AS/Player_Child.root.asb", "n": byml_rw.S32(-3), "u": byml_rw.U32(7), "f": byml_rw.F32(1.5),
                          "big": byml_rw.U64(2 ** 40), "on": True, "none": None},
           "rows": [{"__RowId": "A"}, {"__RowId": "B"}]}
    raw = byml_rw.write(doc)
    back = byml_rw.read(raw)
    check(back == doc and byml_rw.write(back) == raw, "a BYML document written and read back is not the same")
    check(type(back["Components"]["n"]) is byml_rw.S32 and type(back["Components"]["big"]) is byml_rw.U64 and
          type(back["Components"]["f"]) is byml_rw.F32, "a BYML value came back as another node type")
    # (coop_bodies.py imports zstandard only where it reads the game: its packs and tables load without it)
    spec = importlib.util.spec_from_file_location("coop_bodies", ROOT / "nx2" / "coop_bodies.py")
    bodies = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bodies)
    files = {"Actor/CoopPlayer_Child.engine__actor__ActorParam.bgyml": raw, "Component/x.bin": b"12345", "a/b.byml": b"YB"}
    check(bodies.sarc_read(bodies.sarc_write(files)) == files, "an actor pack written and read back does not hold the same files")
    table = bytearray(b"RESTBL" + struct.pack("<IIII", 1, 160, 0, 0))   # (version, name length, hashed entries, named entries)
    names = ["Pack/Actor/CoopPlayer_Child.pack", "Pack/Actor/CoopFoe_Child.pack", "Pack/Actor/CoopPlayer_Adult.pack"]
    for size, name in enumerate(names, 100):
        check(bodies.table_set(table, name, size) and bodies.table_get(table, name)[1] == size, "the size table did not take " + name)
    check(not bodies.table_set(table, names[0], 50) and bodies.table_get(table, names[0])[1] == 100, "a size entry was lowered")
    check(bodies.table_set(table, names[0], 500) and bodies.table_get(table, names[0])[1] == 500, "a size entry was not raised")
    count = struct.unpack_from("<I", table, 14)[0]
    hashes = [struct.unpack_from("<I", table, 22 + 8 * i)[0] for i in range(count)]
    check(count == 3 and hashes == sorted(hashes) and len(table) == 22 + 8 * 3, "the size table's entries are not in their hashes' order")
    check(bodies.table_get(table, "Pack/Actor/Nobody.pack")[1] is None, "the size table found an entry it does not have")


def check_build():
    """The game build (nx2/make_game.py) makes the bodies into the game folder it finishes, so no folder needs a step by
    hand for them; when it cannot, the build says what the game will be missing and ships the game anyway."""
    text = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
    check('run(PY + [os.path.join(HERE, "coop_bodies.py"), out], check=False, output=sys.stderr)' in text,
          "the game build does not make the other players' bodies into the game folder")
    check("the co-op bodies were not made: the other players will stand and slide" in text and
          "the co-op bodies were not made (no zstandard)" in text,
          "a build that could not make the co-op bodies does not say what is missing")


def main():
    check_builders()
    check_build()
    check_pickup()
    check_life()
    check_equip()
    check_presence_fixes()
    check_apply()
    check_host()
    check_ids()
    check_rows()
    print("test_oot_coop: ok")


if __name__ == "__main__":
    main()
