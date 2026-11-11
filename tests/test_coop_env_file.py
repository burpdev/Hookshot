"""hookshot_env.txt sets none of co-op's switches.

hookshot_env.txt is a file beside the game that players pass around and packs bring. One that could set HOS_COOP,
HOS_COOP_SERVER or HOS_COOP_AUTO with HOS_COOP_ROOM would switch co-op on, name its author's relay and put whoever
uses it into its author's room. So no name that begins HOS_COOP is taken from it (env_line_allowed in host/hos_main.c):
co-op is switched on in hookshot_options.ini or the menu, and the variables are for the player's own environment and
for tests. The file HOS_ENV_FILE names, a test run's own, still sets them.

The names checked are every HOS_COOP name that host/*.c holds as a string, so a switch added later is covered.

python -I -B tests/test_coop_env_file.py [--cc gcc]
"""
import argparse
import re
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
    main_c = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    fn = block(main_c, "static int env_line_allowed(") + "\n" + block(main_c, "static void load_hookshot_env(void) {")
    names = set()
    for source in sorted((ROOT / "host").glob("*.c")):
        names.update(re.findall(r'"(HOS_COOP[A-Z0-9_]*)"', source.read_text(encoding="utf-8")))
    session = {"HOS_COOP", "HOS_COOP_NAME", "HOS_COOP_SERVER", "HOS_COOP_PORT", "HOS_COOP_AUTO", "HOS_COOP_ROOM",
               "HOS_COOP_DROP", "HOS_COOP_TRACE"}
    if not session <= names:
        raise SystemExit("FAIL: host/*.c no longer names %s: this test's list is out of date" % ", ".join(sorted(session - names)))
    names = sorted(names | {"HOS_COOP_ANYTHING_LATER"})
    values = {"HOS_COOP_SERVER": "relay.example:24680", "HOS_COOP_AUTO": "join", "HOS_COOP_ROOM": "H7K3WM-BCDFGH-234567",
              "HOS_COOP_NAME": "Link", "HOS_COOP_PORT": "4000", "HOS_COOP_DROP": "30"}
    lines = "".join("%s=%s\n" % (n, values.get(n, "1")) for n in names)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        game = tmp / "game"
        game.mkdir()
        # (HOS_T_BESIDE and HOS_T_COOPERATIVE: the file is read, and only names that begin HOS_COOP are left out)
        (game / "hookshot_env.txt").write_text("HOS_T_BESIDE=1\n" + lines + "HOS_T_COOPERATIVE=1\n", encoding="utf-8")
        (tmp / "test_env.txt").write_text("# a test run's own switches\n" + lines, encoding="utf-8")
        exe = str(game / "hookshot").replace("\\", "\\\\")
        fixture = r'''#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32   /* (the test's own setenv/unsetenv: Windows has _putenv, which the game itself uses there) */
static int setenv(const char *name, const char *value, int overwrite) {
    char s[1024];
    (void)overwrite;
    snprintf(s, sizeof s, "%s=%s", name, value);
    return _putenv(s);
}
static int unsetenv(const char *name) { return setenv(name, "", 1); }
#endif
#define MAX_PATH 260
typedef unsigned long DWORD;
static int fails;
#define CHECK(x, ...) do { if (!(x)) { fails++; fprintf(stderr, "FAIL: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static DWORD hos_game_path_a(char *out, DWORD size) {   /* (host/hos_paths.h: the game folder's path) */
    snprintf(out, size, "%s", "''' + exe + r'''");
    return (DWORD)strlen(out);
}
''' + fn + r'''
static const char *const names[] = {''' + ", ".join('"%s"' % n for n in names) + r'''};
enum { NAMES = sizeof names / sizeof *names };
int main(int argc, char **argv) {
    (void)argc;
    for (int i = 0; i < NAMES; i++) unsetenv(names[i]);
    unsetenv("HOS_T_BESIDE"), unsetenv("HOS_T_COOPERATIVE");
    unsetenv("HOS_ENV_FILE");                      /* out of the box: the file beside the game */
    load_hookshot_env();
    CHECK(getenv("HOS_T_BESIDE") && getenv("HOS_T_COOPERATIVE"), "hookshot_env.txt was not read, or a name that only resembles co-op's was left out");
    for (int i = 0; i < NAMES; i++)
        CHECK(!getenv(names[i]), "hookshot_env.txt set %s=%s: co-op's switches are not taken from a file players pass around", names[i], getenv(names[i]));
    CHECK(!env_line_allowed("HOS_COOP", "0") && !env_line_allowed("HOS_COOP_SERVER", ""), "whatever the value");

    setenv("HOS_COOP", "0", 1);                    /* the player's own environment stays as it is */
    load_hookshot_env();
    CHECK(getenv("HOS_COOP") && !strcmp(getenv("HOS_COOP"), "0"), "hookshot_env.txt changed the environment's HOS_COOP");
    unsetenv("HOS_COOP");

    setenv("HOS_ENV_FILE", argv[1], 1);            /* a test run's own file sets them, as it sets anything */
    load_hookshot_env();
    for (int i = 0; i < NAMES; i++) CHECK(getenv(names[i]) != NULL, "the file HOS_ENV_FILE names did not set %s", names[i]);
    CHECK(getenv("HOS_COOP_ROOM") && !strcmp(getenv("HOS_COOP_ROOM"), "H7K3WM-BCDFGH-234567"), "with its value");
    if (!fails) printf("PASS: hookshot_env.txt sets none of the %d HOS_COOP names; the environment and HOS_ENV_FILE's file do\n", NAMES);
    return fails != 0;
}
'''
        src = tmp / "fixture.c"
        src.write_text(fixture, encoding="utf-8")
        out = tmp / "fixture"
        subprocess.run([a.cc, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Werror", str(src), "-o", str(out)],
                       check=True)
        r = subprocess.run([str(out), str(tmp / "test_env.txt")], capture_output=True, text=True)
        print((r.stdout + (r.stderr if r.returncode else "")).strip())   # (the game's own "left out" lines only at a failure)
        if r.returncode:
            raise SystemExit("FAIL: hookshot_env.txt can set a co-op switch (host/hos_main.c env_line_allowed)")


if __name__ == "__main__":
    main()
