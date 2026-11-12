"""host/oot_coop.c, co-op's progress sync on the game's side, without the game: its reader, classification, apply rules,
table check, guest calls and gate, cut out of the source by their text and run by tests/oot_coop_test.c on a made-up
flag database in a fake guest memory (a getter and setters stand in for the game's):

- the classification from the entries' reset kinds, save file and default, and the struct memberships: progress, items,
  songs, quest items, key, rupee and ammunition counters; temporary, other-file, option and per-player flags left out;
  a table slot found by probing; a damaged entry index or a container outside the guest skipped, not read;
- the schema follows the classification (a flag classified otherwise is another schema);
- the apply rules: a flag only set, never cleared, a Bool only to its "changed" value (false for one that starts true),
  a count only raised and never past the game's counters (999, small keys 15), another category or a value of another
  kind refused, a pool counter written to the pool's value within its limit; the engine's limits the same;
- a pool write held back while the game's own set of that counter waits on its ring (or the ring does not read);
- the game's set routines (SetBool f_806a2b20, SetInt f_80183b70) called with the manager, value and hash, and the
  guest's registers as they were afterwards; the getter likewise;
- the table checked against the game's getter before anything is shared (a table that reads otherwise is refused);
- the gate: nothing read while no save is played, on the title screen (its event flow opened), while a save loads, a
  scene changes or the play clock stands, or while Link is not in the world;
- the switch: progress sync on at the game's start by HOS_COOP or by coop= in the options file (the environment wins),
  or later, when the session first calls back (co-op switched on in the Co-op tab: its thread runs only then); the
  session's hooks registered at the first present whether co-op is on or not; switched on after the title screen came
  back, nothing read until a save is loaded; the read-only probe alone shares nothing;
- and the wiring: one call at the end of the present (host/nvn_native.c), the RomFS open hook and the save-writer
  bookkeeping in host/nn_fs.c, no direct write of the flag table (HOS_COOP_ACCEPT and HOS_COOP_TEST_SET refused from
  hookshot_env.txt: tests/test_env_file.py).

    python -I -B tests/test_oot_coop.py [--cc gcc]
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]

ROOT = Path(__file__).resolve().parents[1]

# the parts of host/oot_coop.c the fixture runs, in source order (constants, tables, state and functions)
PIECES = [
    "#define GAMEDATA_SLOT", "#define GET_BOOL_FN", "#define SET_BOOL_FN", "#define SET_INT_FN", "#define PLAYER_PTR",
    "enum {\n    C_BOOL", "#define S_POUCH", "#define M_STOCK", "#define S_FIRST_GET", "#define S_DUNGEON", "#define M_SMALL_KEYS",
    "#define S_CAPACITY", "#define F_RUPEES", "#define F_MAGIC_SIZE", "#define F_PLAY_CLOCK", "#define F_LOADING", "#define CAP_RUPEES",
    "static const uint32_t transition_flags[]", "static const uint32_t song_flags[]", "static const uint32_t heart_reward_flags[]",
    "static const uint32_t quest_counts[]", "static const uint32_t own_counts[]", "static const struct { uint32_t count, cap_member; } ammo[]",
    "static int in_list(", "#define COOP_FLAG_IN(", "typedef struct {\n    uint32_t hash;", "typedef struct { uint32_t *h; unsigned cap; } HSet;",
    "static void hset_add(", "static int hset_has(", "typedef struct { HSet first_get", "typedef struct { uint32_t hash; int32_t value; uint32_t frame;",
    "enum { PUB_CONTRIBUTORS", "enum { ST_UNSEEN", "static struct {\n    int state;", "#define TRACE_ON",
    "static int rd_ok(", "static uint64_t entry_of(", "static uint64_t struct_of(", "static void members(",
    "typedef struct { uint64_t mgr; HSet *set; uint32_t want; } Walk;", "static void add_member(", "static void pick(",
    "static void add_nested(", "typedef struct { uint64_t mgr; uint32_t member; uint64_t flag; } FindMember;", "static void find_member(",
    "static uint64_t member_entry(", "typedef struct { int cat; uint8_t inv; uint32_t max, cap_member; } Class;", "static Class classify(",
    "static GFlag *add_flag(", "static int cmp_pool_last(", "static int cmp_by_hash(", "static int build_table(", "static int32_t raw_of(",
    "static uint32_t reg_of(", "static GFlag *flag_by_hash(", "static int int_pending(", "static int apply_value(",
    "static int game_get_bool(", "static int set_flag(", "static int check_table(", "static uint32_t limit_fn(",
    "static void sync_on(", "static void on_view(",
    "static void unbind(", "static void say_counts(", "static void bind_tables(", "static int player_in_world(",
    "static volatile int title_starts;", "void hos_oot_coop_rom_open(", "static int gate(",
    "static int start(", "void hos_oot_coop_present(",
]


def piece(text, marker):
    """The definition or declaration that starts at marker: a #define's line, a function to its closing brace, anything
    else (a table, a struct, an enum, a variable) to the ';' that ends it."""
    if text.count(marker) != 1:
        raise SystemExit("FAIL: %r is not once in host/oot_coop.c" % marker)
    start = text.index(marker)
    if marker.startswith("#define"):
        return text[start:text.index("\n", start)]
    brace, semi = text.find("{", start), text.find(";", start)
    head = text[start:brace] if brace != -1 else ""
    function = brace != -1 and (semi == -1 or brace < semi) and ")" in head and "=" not in head
    end, depth = start, 0
    while True:
        ch = text[end]
        depth += (ch == "{") - (ch == "}")
        end += 1
        if (function and ch == "}" and depth == 0) or (not function and ch == ";" and depth == 0):
            return text[start:end]


def wiring(fs, nvn, coop):
    """The places outside host/oot_coop.c: what a test of the cut-out pieces cannot see."""
    present = nvn[nvn.index("NV(nvnQueuePresentTexture) {"):]
    present = present[:present.index("\n}\n")]
    assert present.rstrip().endswith("hos_oot_coop_present(c);"), "the present ends with co-op's call"
    assert present.index("hos_vi_native_presented(phases);") < present.index("hos_oot_coop_present(c);")
    assert present.index("if (away) hos_vcore_back();") < present.index("hos_oot_coop_present(c);"), "with the core held again"
    open_file = fs[fs.index("HLE(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {"):]
    open_file = open_file[:open_file.index("\n}\n")]
    assert "if (!want_write) hos_oot_coop_rom_open(g);" in open_file, "OpenFile tells co-op of the game's reads"
    assert "save_writer_open(path)" in open_file and "save_writer_close();" in open_file
    for hle, call in (("HLE(_ZN2nn2fs9CloseFileENS0_10FileHandleE)", "if (fh->in_save) save_writer_close();"),
                      ("HLE(_ZN2nn2fs10CreateFileEPKcl)", "save_change_begin(path)"),
                      ("HLE(_ZN2nn2fs10DeleteFileEPKc)", "save_change_begin(path)"),
                      ("HLE(_ZN2nn2fs15CreateDirectoryEPKc)", "save_change_begin(path)"),
                      ("HLE(_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc)", "save_change_begin(path)"),
                      ("HLE(_ZN2nn2fs10RenameFileEPKcS2_)", "save_change_begin(a)"),
                      ("HLE(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE)", "save_mounted(root)")):
        body = fs[fs.index(hle):]
        body = body[:body.index("\n}\n")]
        assert call in body, "%s takes the save lock (%s)" % (hle, call)
    # (the game's own setters only: never a write of the flag table)
    assert not re.search(r"recomp_store(8|16|32|64)\s*\(", coop), "host/oot_coop.c writes guest memory only through the game's routines"
    assert not re.search(r"oot_gamedata_set_bool\s*\(", coop), "host/oot_coop.c never calls the debug tools' direct table write"
    tick = coop[coop.index("static void on_tick("):]
    assert tick[:tick.index("\n}\n")].split("\n")[2].strip() == "sync_on();", "the session's tick switches sync on, as its view hook does"


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default=os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang") or "gcc")
    a = p.parse_args()
    host = ROOT / "host"
    coop = (host / "oot_coop.c").read_text(encoding="utf-8")
    try:
        wiring((host / "nn_fs.c").read_text(encoding="utf-8"), (host / "nvn_native.c").read_text(encoding="utf-8"), coop)
    except AssertionError as e:
        print("FAIL: %s" % e)
        return 1
    print("the wiring: the present's last call, OpenFile's hook, the save lock in the save mount's writes, no direct write")
    with tempfile.TemporaryDirectory(prefix="hookshot-oot-coop-") as temp:
        temp = Path(temp)
        pieces = temp / "oot_coop_pieces.c"
        pieces.write_text("\n".join(piece(coop, m) for m in PIECES) + "\n", encoding="utf-8")
        exe = temp / "oot_coop_test.exe"
        base = [a.cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I" + str(host),
                '-DOOT_COOP_PIECES="%s"' % pieces.as_posix(), str(ROOT / "tests" / "oot_coop_test.c"), str(host / "coop_state.c"),
                str(host / "coop_proto.c"), str(ROOT / "third_party" / "monocypher" / "monocypher.c"), "-lm", "-o", str(exe)]
        tries = [["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined"], []] if os.name != "nt" else [[]]
        for i, extra in enumerate(tries):
            r = subprocess.run(base + extra, capture_output=True, text=True, timeout=300)
            if r.returncode == 0:
                print("compiled%s" % (" with " + " ".join(extra) if extra else ""))
                break
            if i + 1 == len(tries):
                print(r.stdout + r.stderr)
                print("FAIL: the fixture does not compile")
                return 1
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=300)
        sys.stdout.write(r.stdout + r.stderr)
        if r.returncode:
            print("FAIL: tests/oot_coop_test.c exited %d" % r.returncode)
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
