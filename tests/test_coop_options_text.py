"""hookshot_options.ini's text values (host/nn_applet.c ini_text, hos_ini_text): a long line is read whole.

Co-op keeps the relay's address in the options file (coop_server=, up to 159 bytes in host/coop.c; a host name alone may
be 253). The reader took a line in pieces of 79 bytes: a longer value was cut, and the rest of the line was read as a
line of its own, so text inside a value could be taken for a setting. Its buffer is 256 bytes now, the size the menu's
own save (gpu/vk/vk_menu.cpp) reads the lines it keeps with.

python -I -B tests/test_coop_options_text.py [--cc gcc]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    applet = (ROOT / "host" / "nn_applet.c").read_text(encoding="utf-8")
    fn = block(applet, "static int ini_text(const char *key, char *out, size_t size) {")
    if "int hos_ini_text(const char *key, char *out, size_t size) { return ini_text(key, out, size); }" not in applet:
        raise SystemExit("FAIL: host/nn_applet.c's hos_ini_text is not ini_text any more: this test reads the wrong function")
    # (159 bytes, the longest relay text host/coop.c keeps; from its 68th byte on it reads like a line that switches co-op off)
    relay = "a" * 67 + "coop=0." + "b" * (159 - 67 - 7 - 6) + ":24680"
    assert len(relay) == 159

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "hookshot_options.ini").write_bytes(("resolution=3\ncoop=1\ncoop_name=first\ncoop_server=%s\ncoop_name=Link\r\n"
                                                    "coop_port=4000" % relay).encode())
        exe = str(tmp / "hookshot").replace("\\", "\\\\")
        fixture = r'''#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_PATH 260
typedef unsigned long DWORD;
static int fails;
#define CHECK(x, ...) do { if (!(x)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static DWORD hos_game_path_a(char *out, DWORD size) {   /* (host/hos_paths.h: the game's path; the options file is beside it) */
    snprintf(out, size, "%s", "''' + exe + r'''");
    return (DWORD)strlen(out);
}
''' + fn + r'''
int main(void) {
    static const char relay[] = "''' + relay + r'''";
    char text[256] = "";
    CHECK(ini_text("coop_server", text, sizeof text) && !strcmp(text, relay), "coop_server= of %d bytes is read whole (%d bytes read: %s)",
          (int)strlen(relay), (int)strlen(text), text);
    CHECK(ini_text("coop", text, sizeof text) && !strcmp(text, "1"), "coop= is its own line's 1, not text inside a longer line (%s)", text);
    CHECK(ini_text("coop_name", text, sizeof text) && !strcmp(text, "Link"), "the last line that sets a key is taken, its line end cut (%s)", text);
    CHECK(ini_text("coop_port", text, sizeof text) && !strcmp(text, "4000"), "a last line without a line end (%s)", text);
    CHECK(ini_text("resolution", text, sizeof text) && !strcmp(text, "3"), "the other options are read as before (%s)", text);
    char small[8] = "";
    CHECK(ini_text("coop_server", small, sizeof small) && !strcmp(small, "aaaaaaa"), "a value is cut to the caller's buffer (%s)", small);
    CHECK(!ini_text("coop_serve", text, sizeof text) && !ini_text("missing", text, sizeof text), "a key that is not there");
    if (!fails) printf("PASS: hookshot_options.ini's text values: a relay address of %d bytes is read whole\n", (int)strlen(relay));
    return fails != 0;
}
'''
        src = tmp / "fixture.c"
        src.write_text(fixture, encoding="utf-8")
        out = tmp / "fixture"
        subprocess.run([a.cc, "-std=c11", "-Wall", "-Werror", str(src), "-o", str(out)], check=True)
        r = subprocess.run([str(out)], capture_output=True, text=True)
        print((r.stdout + r.stderr).strip())
        if r.returncode:
            raise SystemExit("FAIL: host/nn_applet.c ini_text does not read a long options line whole")


if __name__ == "__main__":
    main()
