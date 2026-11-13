"""The Debug tab's Movement speed (1x to 10x): nx2/oot_patches.py's patch_cct_speed hooks the places a character controller's
forward speed is set (SPEED_PLACES: Link's move f_809552f0 at its two setter calls, the setter f_80559df0 and f_80964bf0), each
once, after its anchor, with s0's unit; a file patched before is given back as it is, and a file without them is left alone.
apply makes them only in the supported build's lift (protect_hearts). host/hos_main.c's hos_oot_cct_speed scales the speed for
Link's controller alone, only while the left stick is pushed and only up to 15 m/s (a ledge climb's 55 is left alone); the menu
sends the row's percent (menu_link_speed), 100 while the debug tools are off.

python -I -B tests/test_movement_speed.py
"""
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import oot_patches  # noqa: E402

BODY = "    c->x[9]=recomp_load32(c,c->x[8] + 32);\nreturn;\n}\n"


def check(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def lifted():
    """One file with the three functions, each holding its anchors once"""
    out, done = '#include "a64.h"\n', set()
    for head, anchor, _ in oot_patches.SPEED_PLACES:
        if head in done:
            continue
        done.add(head)
        anchors = [a for h, a, _ in oot_patches.SPEED_PLACES if h == head]
        text = head + "".join(a if a != head else "" for a in anchors) + BODY
        out += text
    return out


def main():
    src = lifted()
    out, n = oot_patches.patch_cct_speed(src)
    check(n == len(oot_patches.SPEED_PLACES) == 4, "the speed's places hooked %d times, not 4" % n)
    check(out.count("hos_oot_cct_speed(c, c->x[0], 0);") == 2, "Link's move: two hooks with a frame's speed")
    check(out.count("hos_oot_cct_speed(c, c->x[0], 1);") == 2, "the setters: two hooks with a second's speed")
    for head, anchor, per_second in oot_patches.SPEED_PLACES:
        check(anchor + oot_patches.SPEED_HOOK % (per_second, "second" if per_second else "frame") in out,
              "a hook does not follow its anchor: %r" % anchor[:40])
    check(out.count("extern void hos_oot_cct_speed(Ctx *c, uint64_t controller, int per_second);") == 1, "the hook is declared once")
    check(oot_patches.patch_cct_speed(out) == (out, 4), "a file patched before was patched again")
    check(oot_patches.patch_cct_speed('#include "a64.h"\nvoid f_80000000(Ctx *__restrict A64_NOESC c) {\n' + BODY) is None,
          "a file without the places was patched")

    with tempfile.TemporaryDirectory() as gen:
        path = os.path.join(gen, "code_000.c")
        for protect, want in ((False, 0), (True, 4)):
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(src)
            stdout = sys.stdout
            sys.stdout = open(os.devnull, "w")
            try:
                oot_patches.apply(gen, protect_hearts=protect)
            finally:
                sys.stdout.close()
                sys.stdout = stdout
            with open(path, encoding="utf-8") as f:
                got = f.read().count("    hos_oot_cct_speed(c, c->x[0], ")
            check(got == want, "apply(protect_hearts=%s) made %d hooks, not %d" % (protect, got, want))

    host = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    fn = host[host.index("void hos_oot_cct_speed("):]
    fn = fn[:fn.index("\n}\n")]
    check("oot_speed_controller" in fn and "hos_move_stick_held()" in fn, "the host scales only Link's speed while he is steered")
    check(re.search(r"<= 15\.0f", fn) is not None, "the host leaves speeds over 15 m/s alone")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    check("constexpr int link_speeds[] = {100, 125, 150, 200, 300, 400, 500, 600, 700, 800, 900, 1000};" in menu,
          "the row's choices are 1x, 1.25x, 1.5x, 2x, 3x, 4x, 5x, 6x, 7x, 8x, 9x, 10x")
    check(re.search(r"int menu_link_speed\(\) \{ return cheats_on\(\) \? .* : 100; \}", menu) is not None,
          "the row sends 100 while the debug tools are off")
    print("PASS: movement speed hooks, host gate and menu choices")


if __name__ == "__main__":
    main()
